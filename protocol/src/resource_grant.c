/* SPDX-License-Identifier: MIT */

#include <kobox2/closure_layout.h>
#include <kobox2/resource_grant.h>
#include <kobox2/sha256.h>

#include <limits.h>
#include <string.h>

enum {
    SLOTS = 0,
    OBJECTS,
    HANDLE_BINDINGS,
};

static const uint8_t schema_digest[KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE] =
    KB2_RESOURCE_GRANT_SCHEMA_SHA256_BYTES;
static const uint8_t abi_identity[KB2_RESOURCE_GRANT_ABI_IDENTITY_SIZE] =
    KB2_RESOURCE_GRANT_ABI_IDENTITY_BYTES;

static uint32_t load_u32(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static uint64_t load_u64(const uint8_t *source) {
    return (uint64_t)load_u32(source) | ((uint64_t)load_u32(source + 4) << 32u);
}

static void store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static void store_u64(uint8_t *destination, uint64_t value) {
    store_u32(destination, (uint32_t)value);
    store_u32(destination + 4, (uint32_t)(value >> 32u));
}

static int bytes_are_zero(const uint8_t *bytes, size_t size) {
    size_t index;

    for (index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            return 0;
        }
    }
    return 1;
}

static int digest_is_zero(const uint8_t digest[KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE]) {
    return bytes_are_zero(digest, KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE);
}

static uint64_t known_rights(uint32_t type) {
    switch (type) {
    case KB2_CLOSURE_RESOURCE_MEMORY:
        return KB2_CLOSURE_MEMORY_RIGHT_READ | KB2_CLOSURE_MEMORY_RIGHT_WRITE |
               KB2_CLOSURE_MEMORY_RIGHT_MAP | KB2_CLOSURE_MEMORY_RIGHT_DMA;
    case KB2_CLOSURE_RESOURCE_DEVICE:
        return KB2_CLOSURE_DEVICE_RIGHT_COMMAND | KB2_CLOSURE_DEVICE_RIGHT_MAP |
               KB2_CLOSURE_DEVICE_RIGHT_DMA;
    case KB2_CLOSURE_RESOURCE_STORAGE:
        return KB2_CLOSURE_STORAGE_RIGHT_READ_BLOCKS |
               KB2_CLOSURE_STORAGE_RIGHT_WRITE_BLOCKS | KB2_CLOSURE_STORAGE_RIGHT_FLUSH |
               KB2_CLOSURE_STORAGE_RIGHT_DISCARD;
    case KB2_CLOSURE_RESOURCE_NOTIFICATION:
        return KB2_CLOSURE_NOTIFICATION_RIGHT_WAIT |
               KB2_CLOSURE_NOTIFICATION_RIGHT_SIGNAL;
    case KB2_CLOSURE_RESOURCE_CHANNEL:
        return KB2_CLOSURE_CHANNEL_RIGHT_SEND | KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE;
    default:
        return 0;
    }
}

static int add_size(size_t *value, size_t count, size_t item_size) {
    if (count != 0 && item_size > (SIZE_MAX - *value) / count) {
        return 0;
    }
    *value += count * item_size;
    return 1;
}

static size_t source_slot_index(const kb2_resource_grant_source_t *source,
                                uint32_t slot_id) {
    size_t left = 0;
    size_t right = source->slot_count;

    while (left < right) {
        size_t middle = left + (right - left) / 2u;

        if (source->slots[middle].slot_id == slot_id) {
            return middle;
        }
        if (source->slots[middle].slot_id < slot_id) {
            left = middle + 1u;
        } else {
            right = middle;
        }
    }
    return SIZE_MAX;
}

static size_t source_object_index(const kb2_resource_grant_source_t *source,
                                  uint64_t object_id) {
    size_t left = 0;
    size_t right = source->object_count;

    while (left < right) {
        size_t middle = left + (right - left) / 2u;

        if (source->objects[middle].object_id == object_id) {
            return middle;
        }
        if (source->objects[middle].object_id < object_id) {
            left = middle + 1u;
        } else {
            right = middle;
        }
    }
    return SIZE_MAX;
}

static int source_valid(const kb2_resource_grant_source_t *source) {
    uint32_t prior_slot_id = 0;
    uint32_t prior_object_slot = 0;
    uint64_t prior_object_id = 0;
    uint64_t prior_handle_object = 0;
    uint32_t prior_role = 0;
    size_t object_cursor = 0;
    size_t index;

    if (source == NULL || source->generation == 0 ||
        digest_is_zero(source->closure_manifest_digest) ||
        source->slot_count > KB2_RESOURCE_GRANT_MAX_SLOTS ||
        source->object_count > KB2_RESOURCE_GRANT_MAX_OBJECTS ||
        source->handle_binding_count > KB2_RESOURCE_GRANT_MAX_HANDLE_BINDINGS ||
        (source->slot_count != 0 && source->slots == NULL) ||
        (source->object_count != 0 && source->objects == NULL) ||
        (source->handle_binding_count != 0 && source->handle_bindings == NULL)) {
        return 0;
    }
    for (index = 0; index < source->slot_count; ++index) {
        const kb2_resource_grant_slot_source_t *slot = &source->slots[index];

        if (slot->slot_id <= prior_slot_id ||
            slot->resource_type > KB2_CLOSURE_RESOURCE_CHANNEL ||
            slot->state > KB2_RESOURCE_GRANT_SLOT_PRESENT ||
            digest_is_zero(slot->interface_schema_digest)) {
            return 0;
        }
        prior_slot_id = slot->slot_id;
    }
    for (index = 0; index < source->object_count; ++index) {
        const kb2_resource_grant_object_source_t *object = &source->objects[index];
        size_t slot_index = source_slot_index(source, object->slot_id);
        uint64_t rights;

        if (slot_index == SIZE_MAX || object->slot_id < prior_object_slot ||
            object->object_id <= prior_object_id || object->granted_rights == 0 ||
            source->slots[slot_index].state != KB2_RESOURCE_GRANT_SLOT_PRESENT) {
            return 0;
        }
        rights = known_rights(source->slots[slot_index].resource_type);
        if (rights == 0 || (object->granted_rights & ~rights) != 0) {
            return 0;
        }
        prior_object_slot = object->slot_id;
        prior_object_id = object->object_id;
    }
    for (index = 0; index < source->slot_count; ++index) {
        const kb2_resource_grant_slot_source_t *slot = &source->slots[index];
        size_t object_start = object_cursor;

        while (object_cursor < source->object_count &&
               source->objects[object_cursor].slot_id == slot->slot_id) {
            ++object_cursor;
        }
        if ((slot->state == KB2_RESOURCE_GRANT_SLOT_ABSENT &&
             object_cursor != object_start) ||
            (slot->state == KB2_RESOURCE_GRANT_SLOT_PRESENT &&
             object_cursor == object_start)) {
            return 0;
        }
    }
    if (object_cursor != source->object_count) {
        return 0;
    }
    for (index = 0; index < source->handle_binding_count; ++index) {
        const kb2_resource_grant_handle_binding_t *binding =
            &source->handle_bindings[index];

        if (source_object_index(source, binding->object_id) == SIZE_MAX ||
            binding->transfer_handle_index != index ||
            binding->object_id < prior_handle_object ||
            (binding->object_id == prior_handle_object &&
             (index == 0 || binding->role <= prior_role))) {
            return 0;
        }
        prior_handle_object = binding->object_id;
        prior_role = binding->role;
    }
    return 1;
}

const char *kb2_resource_grant_schema_sha256_hex(void) {
    return KB2_RESOURCE_GRANT_SCHEMA_SHA256_HEX;
}

kb2_protocol_status_t kb2_resource_grant_copy_schema_digest(uint8_t *digest_out,
                                                            size_t digest_size) {
    if (digest_out == NULL || digest_size != sizeof(schema_digest)) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    memcpy(digest_out, schema_digest, sizeof(schema_digest));
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_resource_grant_encoded_size(
    const kb2_resource_grant_source_t *source, size_t *size_out) {
    size_t size = KB2_RESOURCE_GRANT_GRANT_HEADER_SIZE;

    if (size_out == NULL || !source_valid(source) ||
        !add_size(&size, source->slot_count, KB2_RESOURCE_GRANT_SLOT_GRANT_SIZE) ||
        !add_size(&size, source->object_count, KB2_RESOURCE_GRANT_OBJECT_SIZE) ||
        !add_size(&size,
                  source->handle_binding_count,
                  KB2_RESOURCE_GRANT_HANDLE_BINDING_SIZE) ||
        size > UINT32_MAX) {
        return source == NULL || size_out == NULL ? KB2_PROTOCOL_INVALID_ARGUMENT
                                                  : KB2_PROTOCOL_MALFORMED;
    }
    *size_out = size;
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_resource_grant_encode(
    uint8_t *buffer,
    size_t buffer_size,
    size_t *encoded_size_out,
    const kb2_resource_grant_source_t *source) {
    uint32_t slot_offset = KB2_RESOURCE_GRANT_GRANT_HEADER_SIZE;
    uint32_t object_offset;
    uint32_t handle_offset;
    size_t encoded_size;
    size_t object_cursor = 0;
    size_t handle_cursor = 0;
    size_t index;

    if (buffer == NULL || encoded_size_out == NULL || source == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (kb2_resource_grant_encoded_size(source, &encoded_size) != KB2_PROTOCOL_OK) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (buffer_size < encoded_size) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    object_offset = slot_offset + (uint32_t)source->slot_count *
                                      KB2_RESOURCE_GRANT_SLOT_GRANT_SIZE;
    handle_offset = object_offset + (uint32_t)source->object_count *
                                        KB2_RESOURCE_GRANT_OBJECT_SIZE;
    memset(buffer, 0, encoded_size);
    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_TOTAL_SIZE_OFFSET,
              (uint32_t)encoded_size);
    memcpy(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_ABI_IDENTITY_OFFSET,
           abi_identity,
           sizeof(abi_identity));
    memcpy(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_SCHEMA_DIGEST_OFFSET,
           schema_digest,
           sizeof(schema_digest));
    store_u64(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_GENERATION_OFFSET,
              source->generation);
    memcpy(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_CLOSURE_MANIFEST_DIGEST_OFFSET,
           source->closure_manifest_digest,
           sizeof(source->closure_manifest_digest));
    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_SLOT_COUNT_OFFSET,
              (uint32_t)source->slot_count);
    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_OBJECT_COUNT_OFFSET,
              (uint32_t)source->object_count);
    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_HANDLE_BINDING_COUNT_OFFSET,
              (uint32_t)source->handle_binding_count);
    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_SLOT_TABLE_OFFSET_OFFSET,
              slot_offset);
    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_OBJECT_TABLE_OFFSET_OFFSET,
              object_offset);
    store_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_HANDLE_BINDING_TABLE_OFFSET_OFFSET,
              handle_offset);

    for (index = 0; index < source->slot_count; ++index) {
        const kb2_resource_grant_slot_source_t *slot = &source->slots[index];
        uint8_t *record = buffer + slot_offset + index * KB2_RESOURCE_GRANT_SLOT_GRANT_SIZE;
        size_t start = object_cursor;

        while (object_cursor < source->object_count &&
               source->objects[object_cursor].slot_id == slot->slot_id) {
            ++object_cursor;
        }
        store_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_SLOT_ID_OFFSET, slot->slot_id);
        store_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_RESOURCE_TYPE_OFFSET,
                  slot->resource_type);
        store_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_STATE_OFFSET, slot->state);
        store_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_OBJECT_START_OFFSET,
                  (uint32_t)start);
        store_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_OBJECT_COUNT_OFFSET,
                  (uint32_t)(object_cursor - start));
        memcpy(record + KB2_RESOURCE_GRANT_SLOT_GRANT_INTERFACE_SCHEMA_DIGEST_OFFSET,
               slot->interface_schema_digest,
               sizeof(slot->interface_schema_digest));
    }
    for (index = 0; index < source->object_count; ++index) {
        const kb2_resource_grant_object_source_t *object = &source->objects[index];
        uint8_t *record = buffer + object_offset + index * KB2_RESOURCE_GRANT_OBJECT_SIZE;
        size_t start = handle_cursor;

        while (handle_cursor < source->handle_binding_count &&
               source->handle_bindings[handle_cursor].object_id == object->object_id) {
            ++handle_cursor;
        }
        store_u32(record + KB2_RESOURCE_GRANT_OBJECT_SLOT_ID_OFFSET, object->slot_id);
        store_u64(record + KB2_RESOURCE_GRANT_OBJECT_OBJECT_ID_OFFSET, object->object_id);
        store_u64(record + KB2_RESOURCE_GRANT_OBJECT_GRANTED_RIGHTS_OFFSET,
                  object->granted_rights);
        store_u32(record + KB2_RESOURCE_GRANT_OBJECT_HANDLE_START_OFFSET,
                  (uint32_t)start);
        store_u32(record + KB2_RESOURCE_GRANT_OBJECT_HANDLE_COUNT_OFFSET,
                  (uint32_t)(handle_cursor - start));
    }
    for (index = 0; index < source->handle_binding_count; ++index) {
        const kb2_resource_grant_handle_binding_t *binding =
            &source->handle_bindings[index];
        uint8_t *record = buffer + handle_offset +
                          index * KB2_RESOURCE_GRANT_HANDLE_BINDING_SIZE;

        store_u64(record + KB2_RESOURCE_GRANT_HANDLE_BINDING_OBJECT_ID_OFFSET,
                  binding->object_id);
        store_u32(record + KB2_RESOURCE_GRANT_HANDLE_BINDING_ROLE_OFFSET, binding->role);
        store_u32(record + KB2_RESOURCE_GRANT_HANDLE_BINDING_TRANSFER_HANDLE_INDEX_OFFSET,
                  binding->transfer_handle_index);
    }
    *encoded_size_out = encoded_size;
    return KB2_PROTOCOL_OK;
}

static size_t decoded_slot_index(const kb2_resource_grant_t *grant, uint32_t slot_id) {
    size_t left = 0;
    size_t right = grant->counts[SLOTS];

    while (left < right) {
        kb2_resource_grant_slot_t slot;
        size_t middle = left + (right - left) / 2u;

        if (kb2_resource_grant_slot(grant, middle, &slot) != KB2_PROTOCOL_OK) {
            return SIZE_MAX;
        }
        if (slot.slot_id == slot_id) {
            return middle;
        }
        if (slot.slot_id < slot_id) {
            left = middle + 1u;
        } else {
            right = middle;
        }
    }
    return SIZE_MAX;
}

static size_t decoded_object_index(const kb2_resource_grant_t *grant, uint64_t object_id) {
    size_t left = 0;
    size_t right = grant->counts[OBJECTS];

    while (left < right) {
        kb2_resource_grant_object_t object;
        size_t middle = left + (right - left) / 2u;

        if (kb2_resource_grant_object(grant, middle, &object) != KB2_PROTOCOL_OK) {
            return SIZE_MAX;
        }
        if (object.object_id == object_id) {
            return middle;
        }
        if (object.object_id < object_id) {
            left = middle + 1u;
        } else {
            right = middle;
        }
    }
    return SIZE_MAX;
}

kb2_protocol_status_t kb2_resource_grant_decode(const uint8_t *buffer,
                                                size_t buffer_size,
                                                kb2_resource_grant_t *grant_out) {
    static const uint32_t limits[3] = {
        KB2_RESOURCE_GRANT_MAX_SLOTS,
        KB2_RESOURCE_GRANT_MAX_OBJECTS,
        KB2_RESOURCE_GRANT_MAX_HANDLE_BINDINGS,
    };
    static const uint32_t sizes[3] = {
        KB2_RESOURCE_GRANT_SLOT_GRANT_SIZE,
        KB2_RESOURCE_GRANT_OBJECT_SIZE,
        KB2_RESOURCE_GRANT_HANDLE_BINDING_SIZE,
    };
    static const uint32_t offset_fields[3] = {
        KB2_RESOURCE_GRANT_GRANT_HEADER_SLOT_TABLE_OFFSET_OFFSET,
        KB2_RESOURCE_GRANT_GRANT_HEADER_OBJECT_TABLE_OFFSET_OFFSET,
        KB2_RESOURCE_GRANT_GRANT_HEADER_HANDLE_BINDING_TABLE_OFFSET_OFFSET,
    };
    kb2_resource_grant_t grant = {0};
    uint32_t expected_offset = KB2_RESOURCE_GRANT_GRANT_HEADER_SIZE;
    uint32_t prior_slot_id = 0;
    uint64_t prior_object_id = 0;
    uint32_t prior_object_slot = 0;
    uint64_t prior_handle_object = 0;
    uint32_t prior_role = 0;
    size_t object_cursor = 0;
    size_t handle_cursor = 0;
    size_t index;

    if (buffer == NULL || grant_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    memset(grant_out, 0, sizeof(*grant_out));
    if (buffer_size < KB2_RESOURCE_GRANT_GRANT_HEADER_SIZE) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    if (buffer_size > UINT32_MAX) {
        return KB2_PROTOCOL_OVERFLOW;
    }
    if (memcmp(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_ABI_IDENTITY_OFFSET,
               abi_identity,
               sizeof(abi_identity)) != 0 ||
        memcmp(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_SCHEMA_DIGEST_OFFSET,
               schema_digest,
               sizeof(schema_digest)) != 0) {
        return KB2_PROTOCOL_SCHEMA_MISMATCH;
    }
    if (load_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_TOTAL_SIZE_OFFSET) != buffer_size ||
        !bytes_are_zero(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_RESERVED_OFFSET, 24u)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    grant.bytes = buffer;
    grant.size = buffer_size;
    grant.generation =
        load_u64(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_GENERATION_OFFSET);
    memcpy(grant.closure_manifest_digest,
           buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_CLOSURE_MANIFEST_DIGEST_OFFSET,
           sizeof(grant.closure_manifest_digest));
    grant.counts[SLOTS] =
        load_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_SLOT_COUNT_OFFSET);
    grant.counts[OBJECTS] =
        load_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_OBJECT_COUNT_OFFSET);
    grant.counts[HANDLE_BINDINGS] =
        load_u32(buffer + KB2_RESOURCE_GRANT_GRANT_HEADER_HANDLE_BINDING_COUNT_OFFSET);
    for (index = 0; index < 3; ++index) {
        uint64_t next;

        grant.offsets[index] = load_u32(buffer + offset_fields[index]);
        if (grant.counts[index] > limits[index] ||
            grant.offsets[index] != expected_offset) {
            return KB2_PROTOCOL_MALFORMED;
        }
        next = (uint64_t)expected_offset + (uint64_t)grant.counts[index] * sizes[index];
        if (next > buffer_size) {
            return KB2_PROTOCOL_MALFORMED;
        }
        expected_offset = (uint32_t)next;
    }
    if (expected_offset != buffer_size || grant.generation == 0 ||
        digest_is_zero(grant.closure_manifest_digest)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    *grant_out = grant;
    for (index = 0; index < grant.counts[SLOTS]; ++index) {
        kb2_resource_grant_slot_t slot;
        size_t slot_object_index;

        if (kb2_resource_grant_slot(grant_out, index, &slot) != KB2_PROTOCOL_OK ||
            slot.slot_id <= prior_slot_id || slot.object_start != object_cursor ||
            slot.object_count > grant.counts[OBJECTS] - object_cursor ||
            (slot.state == KB2_RESOURCE_GRANT_SLOT_ABSENT && slot.object_count != 0) ||
            (slot.state == KB2_RESOURCE_GRANT_SLOT_PRESENT && slot.object_count == 0)) {
            goto malformed;
        }
        for (slot_object_index = 0; slot_object_index < slot.object_count;
             ++slot_object_index) {
            kb2_resource_grant_object_t object;

            if (kb2_resource_grant_object(
                    grant_out, object_cursor + slot_object_index, &object) !=
                    KB2_PROTOCOL_OK ||
                object.slot_id != slot.slot_id) {
                goto malformed;
            }
        }
        object_cursor += slot.object_count;
        prior_slot_id = slot.slot_id;
    }
    if (object_cursor != grant.counts[OBJECTS]) {
        goto malformed;
    }
    for (index = 0; index < grant.counts[OBJECTS]; ++index) {
        kb2_resource_grant_object_t object;
        kb2_resource_grant_slot_t slot;
        size_t object_handle_index;
        size_t slot_index;
        uint64_t rights;

        if (kb2_resource_grant_object(grant_out, index, &object) != KB2_PROTOCOL_OK ||
            object.slot_id < prior_object_slot || object.object_id <= prior_object_id ||
            object.handle_start != handle_cursor ||
            object.handle_count > grant.counts[HANDLE_BINDINGS] - handle_cursor) {
            goto malformed;
        }
        slot_index = decoded_slot_index(grant_out, object.slot_id);
        if (slot_index == SIZE_MAX ||
            kb2_resource_grant_slot(grant_out, slot_index, &slot) != KB2_PROTOCOL_OK ||
            slot.state != KB2_RESOURCE_GRANT_SLOT_PRESENT) {
            goto malformed;
        }
        rights = known_rights(slot.resource_type);
        if (rights == 0 || (object.granted_rights & ~rights) != 0) {
            goto malformed;
        }
        for (object_handle_index = 0; object_handle_index < object.handle_count;
             ++object_handle_index) {
            kb2_resource_grant_handle_binding_t binding;

            if (kb2_resource_grant_handle_binding(
                    grant_out, handle_cursor + object_handle_index, &binding) !=
                    KB2_PROTOCOL_OK ||
                binding.object_id != object.object_id) {
                goto malformed;
            }
        }
        handle_cursor += object.handle_count;
        prior_object_slot = object.slot_id;
        prior_object_id = object.object_id;
    }
    if (handle_cursor != grant.counts[HANDLE_BINDINGS]) {
        goto malformed;
    }
    for (index = 0; index < grant.counts[HANDLE_BINDINGS]; ++index) {
        kb2_resource_grant_handle_binding_t binding;

        if (kb2_resource_grant_handle_binding(grant_out, index, &binding) !=
                KB2_PROTOCOL_OK ||
            decoded_object_index(grant_out, binding.object_id) == SIZE_MAX ||
            binding.transfer_handle_index != index ||
            binding.object_id < prior_handle_object ||
            (binding.object_id == prior_handle_object &&
             (index == 0 || binding.role <= prior_role))) {
            goto malformed;
        }
        prior_handle_object = binding.object_id;
        prior_role = binding.role;
    }
    return KB2_PROTOCOL_OK;

malformed:
    memset(grant_out, 0, sizeof(*grant_out));
    return KB2_PROTOCOL_MALFORMED;
}

kb2_protocol_status_t kb2_resource_grant_validate_manifest(
    const kb2_resource_grant_t *grant,
    const kb2_closure_manifest_t *manifest) {
    uint8_t manifest_digest[KB2_SHA256_DIGEST_SIZE];
    size_t index;

    if (grant == NULL || manifest == NULL || grant->bytes == NULL ||
        manifest->bytes == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    kb2_sha256(manifest->bytes, manifest->size, manifest_digest);
    if (memcmp(grant->closure_manifest_digest,
               manifest_digest,
               sizeof(manifest_digest)) != 0) {
        return KB2_PROTOCOL_SCHEMA_MISMATCH;
    }
    if (grant->counts[SLOTS] != kb2_closure_manifest_resource_count(manifest)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    for (index = 0; index < grant->counts[SLOTS]; ++index) {
        kb2_closure_manifest_resource_t requirement;
        kb2_resource_grant_slot_t slot;
        size_t object_index;

        if (kb2_closure_manifest_resource(manifest, index, &requirement) !=
                KB2_PROTOCOL_OK ||
            kb2_resource_grant_slot(grant, index, &slot) != KB2_PROTOCOL_OK) {
            return KB2_PROTOCOL_MALFORMED;
        }
        if (slot.slot_id != requirement.slot_id ||
            slot.resource_type != requirement.type ||
            memcmp(slot.interface_schema_digest,
                   requirement.interface_schema_digest,
                   sizeof(slot.interface_schema_digest)) != 0) {
            return KB2_PROTOCOL_SCHEMA_MISMATCH;
        }
        if (slot.state == KB2_RESOURCE_GRANT_SLOT_ABSENT) {
            if ((requirement.flags & KB2_CLOSURE_RESOURCE_FLAG_REQUIRED) != 0) {
                return KB2_PROTOCOL_MALFORMED;
            }
            continue;
        }
        if (slot.object_count < requirement.minimum_count ||
            slot.object_count > requirement.maximum_count) {
            return KB2_PROTOCOL_MALFORMED;
        }
        for (object_index = 0; object_index < slot.object_count; ++object_index) {
            kb2_resource_grant_object_t object;

            if (kb2_resource_grant_object(
                    grant, slot.object_start + object_index, &object) !=
                    KB2_PROTOCOL_OK ||
                (object.granted_rights & requirement.required_rights) !=
                    requirement.required_rights ||
                (object.granted_rights & ~requirement.maximum_rights) != 0) {
                return KB2_PROTOCOL_MALFORMED;
            }
        }
    }
    return KB2_PROTOCOL_OK;
}

size_t kb2_resource_grant_slot_count(const kb2_resource_grant_t *grant) {
    return grant == NULL ? 0 : grant->counts[SLOTS];
}

size_t kb2_resource_grant_object_count(const kb2_resource_grant_t *grant) {
    return grant == NULL ? 0 : grant->counts[OBJECTS];
}

size_t kb2_resource_grant_handle_binding_count(const kb2_resource_grant_t *grant) {
    return grant == NULL ? 0 : grant->counts[HANDLE_BINDINGS];
}

kb2_protocol_status_t kb2_resource_grant_slot(const kb2_resource_grant_t *grant,
                                              size_t index,
                                              kb2_resource_grant_slot_t *slot_out) {
    const uint8_t *record;

    if (grant == NULL || slot_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index >= grant->counts[SLOTS]) {
        return KB2_PROTOCOL_MALFORMED;
    }
    record = grant->bytes + grant->offsets[SLOTS] +
             index * KB2_RESOURCE_GRANT_SLOT_GRANT_SIZE;
    memset(slot_out, 0, sizeof(*slot_out));
    slot_out->slot_id = load_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_SLOT_ID_OFFSET);
    slot_out->resource_type =
        load_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_RESOURCE_TYPE_OFFSET);
    slot_out->state = load_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_STATE_OFFSET);
    slot_out->object_start =
        load_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_OBJECT_START_OFFSET);
    slot_out->object_count =
        load_u32(record + KB2_RESOURCE_GRANT_SLOT_GRANT_OBJECT_COUNT_OFFSET);
    memcpy(slot_out->interface_schema_digest,
           record + KB2_RESOURCE_GRANT_SLOT_GRANT_INTERFACE_SCHEMA_DIGEST_OFFSET,
           sizeof(slot_out->interface_schema_digest));
    if (slot_out->slot_id == 0 ||
        slot_out->resource_type > KB2_CLOSURE_RESOURCE_CHANNEL ||
        slot_out->state > KB2_RESOURCE_GRANT_SLOT_PRESENT ||
        digest_is_zero(slot_out->interface_schema_digest) ||
        !bytes_are_zero(record + KB2_RESOURCE_GRANT_SLOT_GRANT_RESERVED_OFFSET, 12u)) {
        memset(slot_out, 0, sizeof(*slot_out));
        return KB2_PROTOCOL_MALFORMED;
    }
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_resource_grant_object(const kb2_resource_grant_t *grant,
                                                size_t index,
                                                kb2_resource_grant_object_t *object_out) {
    const uint8_t *record;

    if (grant == NULL || object_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index >= grant->counts[OBJECTS]) {
        return KB2_PROTOCOL_MALFORMED;
    }
    record = grant->bytes + grant->offsets[OBJECTS] +
             index * KB2_RESOURCE_GRANT_OBJECT_SIZE;
    memset(object_out, 0, sizeof(*object_out));
    object_out->slot_id = load_u32(record + KB2_RESOURCE_GRANT_OBJECT_SLOT_ID_OFFSET);
    object_out->object_id = load_u64(record + KB2_RESOURCE_GRANT_OBJECT_OBJECT_ID_OFFSET);
    object_out->granted_rights =
        load_u64(record + KB2_RESOURCE_GRANT_OBJECT_GRANTED_RIGHTS_OFFSET);
    object_out->handle_start =
        load_u32(record + KB2_RESOURCE_GRANT_OBJECT_HANDLE_START_OFFSET);
    object_out->handle_count =
        load_u32(record + KB2_RESOURCE_GRANT_OBJECT_HANDLE_COUNT_OFFSET);
    if (object_out->slot_id == 0 || object_out->object_id == 0 ||
        object_out->granted_rights == 0 ||
        !bytes_are_zero(record + KB2_RESOURCE_GRANT_OBJECT_RESERVED_OFFSET, 4u)) {
        memset(object_out, 0, sizeof(*object_out));
        return KB2_PROTOCOL_MALFORMED;
    }
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_resource_grant_handle_binding(
    const kb2_resource_grant_t *grant,
    size_t index,
    kb2_resource_grant_handle_binding_t *binding_out) {
    const uint8_t *record;

    if (grant == NULL || binding_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    memset(binding_out, 0, sizeof(*binding_out));
    if (index >= grant->counts[HANDLE_BINDINGS]) {
        return KB2_PROTOCOL_MALFORMED;
    }
    record = grant->bytes + grant->offsets[HANDLE_BINDINGS] +
             index * KB2_RESOURCE_GRANT_HANDLE_BINDING_SIZE;
    binding_out->object_id =
        load_u64(record + KB2_RESOURCE_GRANT_HANDLE_BINDING_OBJECT_ID_OFFSET);
    binding_out->role = load_u32(record + KB2_RESOURCE_GRANT_HANDLE_BINDING_ROLE_OFFSET);
    binding_out->transfer_handle_index =
        load_u32(record + KB2_RESOURCE_GRANT_HANDLE_BINDING_TRANSFER_HANDLE_INDEX_OFFSET);
    return binding_out->object_id == 0 ? KB2_PROTOCOL_MALFORMED : KB2_PROTOCOL_OK;
}
