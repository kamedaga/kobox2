/* SPDX-License-Identifier: MIT */
#include <kobox2/virtqueue_memory.h>

#include <string.h>

static int valid_rights(uint32_t rights) {
    return rights &&
           !(rights & ~(KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE));
}

static int overlaps(uint64_t a, uint64_t size_a, uint64_t b, uint64_t size_b) {
    return a < b + size_b && b < a + size_a;
}

static void *
resolve(kb2_vq_mapped_memory_t *memory, uint64_t address, size_t size, uint32_t rights) {
    for (size_t i = 0; i < memory->mapping_count; ++i) {
        const kb2_vq_mapping_t *mapping = &memory->mappings[i];
        if (address < mapping->transport_base || (mapping->local_rights & rights) != rights) {
            continue;
        }
        uint64_t offset = address - mapping->transport_base;
        if (offset <= mapping->length && size <= mapping->length - offset) {
            return (unsigned char *)mapping->address + (size_t)offset;
        }
    }
    return NULL;
}

kb2_vq_status_t kb2_vq_mapped_memory_init(kb2_vq_mapped_memory_t *out,
                                          const kb2_vq_mapping_t *mappings,
                                          size_t mapping_count,
                                          const kb2_vq_arena_t *arenas,
                                          size_t arena_count,
                                          const kb2_vq_atomic_ops_t *atomics) {
    if (!out || !mappings || !mapping_count || !arenas || !arena_count || !atomics ||
        !atomics->load_acquire || !atomics->store_release || !atomics->fence) {
        return KB2_VQ_INVALID;
    }
    for (size_t i = 0; i < mapping_count; ++i) {
        const kb2_vq_mapping_t *mapping = &mappings[i];
        uintptr_t native = (uintptr_t)mapping->address;
        if (!native || !mapping->length || mapping->transport_base > UINT64_MAX - mapping->length ||
            native > UINTPTR_MAX - mapping->length || !valid_rights(mapping->local_rights)) {
            return KB2_VQ_INVALID;
        }
        for (size_t j = 0; j < i; ++j) {
            if (overlaps(mapping->transport_base,
                         mapping->length,
                         mappings[j].transport_base,
                         mappings[j].length) ||
                overlaps(
                    native, mapping->length, (uintptr_t)mappings[j].address, mappings[j].length)) {
                return KB2_VQ_INVALID;
            }
        }
    }
    for (size_t i = 0; i < arena_count; ++i) {
        const kb2_vq_arena_t *arena = &arenas[i];
        int registered = 0;
        if (!arena->queue_id || !arena->length ||
            arena->transport_base > UINT64_MAX - arena->length || !valid_rights(arena->rights)) {
            return KB2_VQ_INVALID;
        }
        for (size_t j = 0; j < mapping_count; ++j) {
            if (arena->transport_base >= mappings[j].transport_base &&
                arena->transport_base + arena->length <=
                    mappings[j].transport_base + mappings[j].length) {
                registered = 1;
            }
        }
        if (!registered) {
            return KB2_VQ_INVALID;
        }
        for (size_t j = 0; j < i; ++j) {
            if (overlaps(arena->transport_base,
                         arena->length,
                         arenas[j].transport_base,
                         arenas[j].length)) {
                return KB2_VQ_INVALID;
            }
        }
    }
    *out = (kb2_vq_mapped_memory_t){.mappings = mappings,
                                    .mapping_count = mapping_count,
                                    .arenas = arenas,
                                    .arena_count = arena_count,
                                    .atomics = *atomics};
    return KB2_VQ_OK;
}

static int read_bytes(void *context, uint64_t address, void *out, size_t size) {
    void *source = resolve(context, address, size, KB2_PROTOCOL_REGION_RIGHT_READ);
    if (!source || (!out && size)) {
        return -1;
    }
    if (size) {
        memcpy(out, source, size);
    }
    return 0;
}

static int write_bytes(void *context, uint64_t address, const void *bytes, size_t size) {
    void *destination = resolve(context, address, size, KB2_PROTOCOL_REGION_RIGHT_WRITE);
    if (!destination || (!bytes && size)) {
        return -1;
    }
    if (size) {
        memcpy(destination, bytes, size);
    }
    return 0;
}

static int load_index(void *context, uint64_t address, uint16_t *value) {
    kb2_vq_mapped_memory_t *memory = context;
    const void *source = resolve(memory, address, sizeof(*value), KB2_PROTOCOL_REGION_RIGHT_READ);
    if (!source || (address & 1) || ((uintptr_t)source & 1) || !value) {
        return -1;
    }
    *value = memory->atomics.load_acquire(source);
    return 0;
}

static int store_index(void *context, uint64_t address, uint16_t value) {
    kb2_vq_mapped_memory_t *memory = context;
    void *destination = resolve(memory, address, sizeof(value), KB2_PROTOCOL_REGION_RIGHT_WRITE);
    if (!destination || (address & 1) || ((uintptr_t)destination & 1)) {
        return -1;
    }
    memory->atomics.store_release(destination, value);
    return 0;
}

static void full_fence(void *context) {
    kb2_vq_mapped_memory_t *memory = context;
    memory->atomics.fence();
}

static int payload_allowed(
    void *context, uint32_t queue_id, uint64_t address, uint64_t size, uint32_t rights) {
    kb2_vq_mapped_memory_t *memory = context;
    if (!size || address > UINT64_MAX - size) {
        return 0;
    }
    for (size_t i = 0; i < memory->arena_count; ++i) {
        const kb2_vq_arena_t *arena = &memory->arenas[i];
        if (arena->queue_id == queue_id && (arena->rights & rights) == rights &&
            address >= arena->transport_base &&
            address + size <= arena->transport_base + arena->length) {
            return 1;
        }
    }
    return 0;
}

kb2_vq_memory_t kb2_vq_mapped_memory_ops(kb2_vq_mapped_memory_t *memory) {
    return (kb2_vq_memory_t){.context = memory,
                             .read = read_bytes,
                             .write = write_bytes,
                             .load_acquire = load_index,
                             .store_release = store_index,
                             .fence = full_fence,
                             .payload_allowed = payload_allowed};
}
