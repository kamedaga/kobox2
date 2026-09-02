/* SPDX-License-Identifier: MIT */

#include <kobox2_test/split_virtqueue.h>

#include <limits.h>
#include <string.h>

#define KB2_TEST_VQ_DESCRIPTOR_SIZE 16u
#define KB2_TEST_VQ_DESCRIPTOR_ADDRESS_OFFSET 0u
#define KB2_TEST_VQ_DESCRIPTOR_LENGTH_OFFSET 8u
#define KB2_TEST_VQ_DESCRIPTOR_FLAGS_OFFSET 12u
#define KB2_TEST_VQ_DESCRIPTOR_NEXT_OFFSET 14u
#define KB2_TEST_VQ_AVAILABLE_INDEX_OFFSET 2u
#define KB2_TEST_VQ_AVAILABLE_RING_OFFSET 4u
#define KB2_TEST_VQ_USED_INDEX_OFFSET 2u
#define KB2_TEST_VQ_USED_RING_OFFSET 4u
#define KB2_TEST_VQ_USED_ELEMENT_SIZE 8u

static uint16_t kb2_test_load_u16(const uint8_t *source) {
    return (uint16_t)((uint16_t)source[0] | (uint16_t)((uint16_t)source[1] << 8u));
}

static uint32_t kb2_test_load_u32(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static uint64_t kb2_test_load_u64(const uint8_t *source) {
    return (uint64_t)kb2_test_load_u32(source) |
           ((uint64_t)kb2_test_load_u32(source + 4) << 32u);
}

static void kb2_test_store_u16(uint8_t *destination, uint16_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
}

static void kb2_test_store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static void kb2_test_store_u64(uint8_t *destination, uint64_t value) {
    kb2_test_store_u32(destination, (uint32_t)value);
    kb2_test_store_u32(destination + 4, (uint32_t)(value >> 32u));
}

static uint16_t kb2_test_atomic_load_acquire(const uint8_t *source) {
    uint16_t value;

    __atomic_load((const uint16_t *)(const void *)source, &value, __ATOMIC_ACQUIRE);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap16(value);
#endif
    return value;
}

static void kb2_test_atomic_store_release(uint8_t *destination, uint16_t value) {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap16(value);
#endif
    __atomic_store((uint16_t *)(void *)destination, &value, __ATOMIC_RELEASE);
}

static int kb2_test_range_valid(uint64_t base, uint64_t length) {
    return length != 0 && base <= UINT64_MAX - length;
}

static uint8_t *kb2_test_resolve(kb2_test_vq_t *virtqueue,
                                 uint64_t address,
                                 uint64_t length,
                                 uint32_t required_rights) {
    uint64_t offset;
    size_t index;

    if (!kb2_test_range_valid(address, length)) {
        return NULL;
    }
    for (index = 0; index < virtqueue->region_count; ++index) {
        const kb2_protocol_region_t *region = &virtqueue->regions[index];

        if ((region->rights & required_rights) == required_rights &&
            address >= region->transport_base &&
            address + length <= region->transport_base + region->length) {
            break;
        }
    }
    if (index == virtqueue->region_count || address < virtqueue->mapping_transport_base) {
        return NULL;
    }
    offset = address - virtqueue->mapping_transport_base;
    if (offset > virtqueue->shared_memory_size ||
        length > virtqueue->shared_memory_size - (size_t)offset) {
        return NULL;
    }
    return virtqueue->shared_memory + (size_t)offset;
}

static int kb2_test_notification_needed(uint16_t event_index,
                                        uint16_t new_index,
                                        uint16_t old_index) {
    return (uint16_t)(new_index - event_index - 1u) < (uint16_t)(new_index - old_index);
}

static int kb2_test_outstanding(const kb2_test_vq_t *virtqueue, uint16_t head) {
    return (virtqueue->outstanding[head / 8u] & (uint8_t)(1u << (head % 8u))) != 0;
}

static void kb2_test_set_outstanding(kb2_test_vq_t *virtqueue, uint16_t head, int value) {
    uint8_t mask = (uint8_t)(1u << (head % 8u));

    if (value) {
        virtqueue->outstanding[head / 8u] |= mask;
    } else {
        virtqueue->outstanding[head / 8u] &= (uint8_t)~mask;
    }
}

kb2_test_vq_status_t kb2_test_vq_bind(kb2_test_vq_t *virtqueue,
                                      uint8_t *shared_memory,
                                      size_t shared_memory_size,
                                      const kb2_protocol_queue_t *queue,
                                      const kb2_protocol_region_t *regions,
                                      size_t region_count,
                                      int initialize) {
    uint64_t descriptor_length;
    uint64_t available_length;
    uint64_t used_length;
    size_t index;

    if (virtqueue == NULL || shared_memory == NULL || queue == NULL || regions == NULL ||
        region_count == 0 ||
        queue->queue_size < KB2_PROTOCOL_QUEUE_SIZE_MIN ||
        queue->queue_size > KB2_PROTOCOL_QUEUE_SIZE_MAX ||
        (queue->queue_size & (queue->queue_size - 1u)) != 0 ||
        queue->queue_size > UINT16_MAX || queue->max_chain_length == 0 ||
        queue->max_chain_length > queue->queue_size || queue->max_indirect_length == 0 ||
        queue->max_outstanding == 0 || queue->max_outstanding > queue->queue_size) {
        return KB2_TEST_VQ_INVALID_ARGUMENT;
    }
    memset(virtqueue, 0, sizeof(*virtqueue));
    virtqueue->shared_memory = shared_memory;
    virtqueue->shared_memory_size = shared_memory_size;
    virtqueue->queue = *queue;
    virtqueue->regions = regions;
    virtqueue->region_count = region_count;
    virtqueue->mapping_transport_base = UINT64_MAX;
    for (index = 0; index < region_count; ++index) {
        if (!kb2_test_range_valid(regions[index].transport_base, regions[index].length)) {
            memset(virtqueue, 0, sizeof(*virtqueue));
            return KB2_TEST_VQ_MALFORMED;
        }
        if (regions[index].transport_base < virtqueue->mapping_transport_base) {
            virtqueue->mapping_transport_base = regions[index].transport_base;
        }
    }
    descriptor_length = (uint64_t)queue->queue_size * KB2_TEST_VQ_DESCRIPTOR_SIZE;
    available_length = 6u + (uint64_t)queue->queue_size * 2u;
    used_length = 6u + (uint64_t)queue->queue_size * KB2_TEST_VQ_USED_ELEMENT_SIZE;
    virtqueue->descriptor_table = kb2_test_resolve(
        virtqueue, queue->descriptor_address, descriptor_length, KB2_PROTOCOL_REGION_RIGHT_WRITE);
    virtqueue->available_ring = kb2_test_resolve(
        virtqueue, queue->available_address, available_length, KB2_PROTOCOL_REGION_RIGHT_WRITE);
    virtqueue->used_ring = kb2_test_resolve(
        virtqueue, queue->used_address, used_length, KB2_PROTOCOL_REGION_RIGHT_WRITE);
    if (virtqueue->descriptor_table == NULL || virtqueue->available_ring == NULL ||
        virtqueue->used_ring == NULL ||
        ((uintptr_t)(virtqueue->available_ring + KB2_TEST_VQ_AVAILABLE_INDEX_OFFSET) & 1u) != 0 ||
        ((uintptr_t)(virtqueue->used_ring + KB2_TEST_VQ_USED_INDEX_OFFSET) & 1u) != 0 ||
        !__atomic_always_lock_free(sizeof(uint16_t), NULL)) {
        memset(virtqueue, 0, sizeof(*virtqueue));
        return KB2_TEST_VQ_MALFORMED;
    }
    if (initialize) {
        memset(virtqueue->descriptor_table, 0, (size_t)descriptor_length);
        memset(virtqueue->available_ring, 0, (size_t)available_length);
        memset(virtqueue->used_ring, 0, (size_t)used_length);
    }
    return KB2_TEST_VQ_OK;
}

kb2_test_vq_status_t kb2_test_vq_set_descriptor(kb2_test_vq_t *virtqueue,
                                                uint16_t index,
                                                uint64_t address,
                                                uint32_t length,
                                                uint16_t flags,
                                                uint16_t next) {
    const uint16_t known_flags = KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT |
                                 KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE |
                                 KB2_TEST_VQ_DESCRIPTOR_FLAG_INDIRECT;
    uint32_t rights = (flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) != 0
                          ? KB2_PROTOCOL_REGION_RIGHT_WRITE
                          : KB2_PROTOCOL_REGION_RIGHT_READ;
    uint8_t *descriptor;

    if (virtqueue == NULL || index >= virtqueue->queue.queue_size || length == 0 ||
        (flags & ~known_flags) != 0 ||
        ((flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT) != 0 &&
         next >= virtqueue->queue.queue_size) ||
        ((flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_INDIRECT) != 0 &&
         ((flags & (KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT | KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE)) != 0 ||
          length % KB2_TEST_VQ_DESCRIPTOR_SIZE != 0 ||
          length / KB2_TEST_VQ_DESCRIPTOR_SIZE > virtqueue->queue.max_indirect_length)) ||
        kb2_test_resolve(virtqueue, address, length, rights) == NULL) {
        return KB2_TEST_VQ_MALFORMED;
    }
    descriptor = virtqueue->descriptor_table + (size_t)index * KB2_TEST_VQ_DESCRIPTOR_SIZE;
    kb2_test_store_u64(descriptor + KB2_TEST_VQ_DESCRIPTOR_ADDRESS_OFFSET, address);
    kb2_test_store_u32(descriptor + KB2_TEST_VQ_DESCRIPTOR_LENGTH_OFFSET, length);
    kb2_test_store_u16(descriptor + KB2_TEST_VQ_DESCRIPTOR_FLAGS_OFFSET, flags);
    kb2_test_store_u16(descriptor + KB2_TEST_VQ_DESCRIPTOR_NEXT_OFFSET, next);
    return KB2_TEST_VQ_OK;
}

kb2_test_vq_status_t kb2_test_vq_set_indirect_descriptor(kb2_test_vq_t *virtqueue,
                                                         uint64_t table_address,
                                                         uint16_t table_index,
                                                         uint16_t table_count,
                                                         uint64_t address,
                                                         uint32_t length,
                                                         uint16_t flags,
                                                         uint16_t next) {
    const uint16_t known_flags =
        KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT | KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE;
    uint64_t table_length = (uint64_t)table_count * KB2_TEST_VQ_DESCRIPTOR_SIZE;
    uint32_t rights = (flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) != 0
                          ? KB2_PROTOCOL_REGION_RIGHT_WRITE
                          : KB2_PROTOCOL_REGION_RIGHT_READ;
    uint8_t *table;
    uint8_t *descriptor;

    if (virtqueue == NULL || table_count == 0 ||
        table_count > virtqueue->queue.max_indirect_length || table_index >= table_count ||
        length == 0 || (flags & ~known_flags) != 0 ||
        ((flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT) != 0 && next >= table_count) ||
        kb2_test_resolve(virtqueue, address, length, rights) == NULL) {
        return KB2_TEST_VQ_MALFORMED;
    }
    table = kb2_test_resolve(virtqueue,
                             table_address,
                             table_length,
                             KB2_PROTOCOL_REGION_RIGHT_READ);
    if (table == NULL) {
        return KB2_TEST_VQ_MALFORMED;
    }
    descriptor = table + (size_t)table_index * KB2_TEST_VQ_DESCRIPTOR_SIZE;
    kb2_test_store_u64(descriptor + KB2_TEST_VQ_DESCRIPTOR_ADDRESS_OFFSET, address);
    kb2_test_store_u32(descriptor + KB2_TEST_VQ_DESCRIPTOR_LENGTH_OFFSET, length);
    kb2_test_store_u16(descriptor + KB2_TEST_VQ_DESCRIPTOR_FLAGS_OFFSET, flags);
    kb2_test_store_u16(descriptor + KB2_TEST_VQ_DESCRIPTOR_NEXT_OFFSET, next);
    return KB2_TEST_VQ_OK;
}

kb2_test_vq_status_t kb2_test_vq_publish(kb2_test_vq_t *virtqueue,
                                         uint16_t head,
                                         int *notify_out) {
    uint16_t old_index;
    uint16_t new_index;
    uint16_t event_index;
    size_t slot;

    if (virtqueue == NULL || notify_out == NULL || head >= virtqueue->queue.queue_size) {
        return KB2_TEST_VQ_INVALID_ARGUMENT;
    }
    if ((uint16_t)(virtqueue->available_producer_index -
                   virtqueue->used_consumer_index) >= virtqueue->queue.max_outstanding) {
        return KB2_TEST_VQ_FULL;
    }
    if (kb2_test_outstanding(virtqueue, head)) {
        return KB2_TEST_VQ_MALFORMED;
    }
    old_index = virtqueue->available_producer_index;
    new_index = (uint16_t)(old_index + 1u);
    slot = old_index & (virtqueue->queue.queue_size - 1u);
    kb2_test_store_u16(virtqueue->available_ring + KB2_TEST_VQ_AVAILABLE_RING_OFFSET + slot * 2u,
                       head);
    kb2_test_set_outstanding(virtqueue, head, 1);
    kb2_test_atomic_store_release(
        virtqueue->available_ring + KB2_TEST_VQ_AVAILABLE_INDEX_OFFSET, new_index);
    virtqueue->available_producer_index = new_index;
    event_index = kb2_test_atomic_load_acquire(
        virtqueue->used_ring + KB2_TEST_VQ_USED_RING_OFFSET +
        (size_t)virtqueue->queue.queue_size * KB2_TEST_VQ_USED_ELEMENT_SIZE);
    *notify_out = kb2_test_notification_needed(event_index, new_index, old_index);
    return KB2_TEST_VQ_OK;
}

kb2_test_vq_status_t kb2_test_vq_take_available(kb2_test_vq_t *virtqueue,
                                                uint16_t *head_out) {
    uint16_t producer_index;
    uint16_t distance;
    size_t slot;

    if (virtqueue == NULL || head_out == NULL) {
        return KB2_TEST_VQ_INVALID_ARGUMENT;
    }
    producer_index = kb2_test_atomic_load_acquire(
        virtqueue->available_ring + KB2_TEST_VQ_AVAILABLE_INDEX_OFFSET);
    distance = (uint16_t)(producer_index - virtqueue->available_consumer_index);
    if (distance == 0) {
        return KB2_TEST_VQ_EMPTY;
    }
    if (distance > virtqueue->queue.queue_size) {
        return KB2_TEST_VQ_MALFORMED;
    }
    slot = virtqueue->available_consumer_index & (virtqueue->queue.queue_size - 1u);
    *head_out = kb2_test_load_u16(virtqueue->available_ring +
                                  KB2_TEST_VQ_AVAILABLE_RING_OFFSET + slot * 2u);
    ++virtqueue->available_consumer_index;
    kb2_test_atomic_store_release(
        virtqueue->used_ring + KB2_TEST_VQ_USED_RING_OFFSET +
            (size_t)virtqueue->queue.queue_size * KB2_TEST_VQ_USED_ELEMENT_SIZE,
        virtqueue->available_consumer_index);
    if (*head_out >= virtqueue->queue.queue_size) {
        return KB2_TEST_VQ_MALFORMED;
    }
    return KB2_TEST_VQ_OK;
}

static kb2_test_vq_status_t kb2_test_vq_read_descriptor_chain(
    kb2_test_vq_t *virtqueue,
    const uint8_t *descriptor_table,
    uint16_t descriptor_count,
    uint16_t head,
    int indirect,
    kb2_test_vq_segment_t *segments_out,
    size_t segment_capacity,
    size_t *segment_count_out) {
    const uint16_t known_flags = KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT |
                                 KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE |
                                 KB2_TEST_VQ_DESCRIPTOR_FLAG_INDIRECT;
    uint16_t current = head;
    size_t count = 0;

    while (count < descriptor_count && count < virtqueue->queue.max_chain_length) {
        const uint8_t *descriptor;
        uint64_t address;
        uint32_t length;
        uint32_t rights;
        uint16_t flags;
        uint16_t next;

        if (current >= descriptor_count || count >= segment_capacity) {
            return KB2_TEST_VQ_MALFORMED;
        }
        descriptor = descriptor_table + (size_t)current * KB2_TEST_VQ_DESCRIPTOR_SIZE;
        address = kb2_test_load_u64(descriptor + KB2_TEST_VQ_DESCRIPTOR_ADDRESS_OFFSET);
        length = kb2_test_load_u32(descriptor + KB2_TEST_VQ_DESCRIPTOR_LENGTH_OFFSET);
        flags = kb2_test_load_u16(descriptor + KB2_TEST_VQ_DESCRIPTOR_FLAGS_OFFSET);
        next = kb2_test_load_u16(descriptor + KB2_TEST_VQ_DESCRIPTOR_NEXT_OFFSET);
        rights = (flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) != 0
                     ? KB2_PROTOCOL_REGION_RIGHT_WRITE
                     : KB2_PROTOCOL_REGION_RIGHT_READ;
        if (length == 0 || (flags & ~known_flags) != 0 ||
            ((indirect && (flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_INDIRECT) != 0) ||
             ((flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT) != 0 && next >= descriptor_count)) ||
            kb2_test_resolve(virtqueue, address, length, rights) == NULL) {
            return KB2_TEST_VQ_MALFORMED;
        }
        if (!indirect && count == 0 &&
            (flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_INDIRECT) != 0) {
            const uint8_t *indirect_table;
            uint16_t indirect_count;

            if ((flags & (KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT |
                          KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE)) != 0 ||
                length % KB2_TEST_VQ_DESCRIPTOR_SIZE != 0 ||
                length / KB2_TEST_VQ_DESCRIPTOR_SIZE > virtqueue->queue.max_indirect_length ||
                length / KB2_TEST_VQ_DESCRIPTOR_SIZE > UINT16_MAX) {
                return KB2_TEST_VQ_MALFORMED;
            }
            indirect_table = kb2_test_resolve(
                virtqueue, address, length, KB2_PROTOCOL_REGION_RIGHT_READ);
            if (indirect_table == NULL) {
                return KB2_TEST_VQ_MALFORMED;
            }
            indirect_count = (uint16_t)(length / KB2_TEST_VQ_DESCRIPTOR_SIZE);
            return kb2_test_vq_read_descriptor_chain(virtqueue,
                                                     indirect_table,
                                                     indirect_count,
                                                     0,
                                                     1,
                                                     segments_out,
                                                     segment_capacity,
                                                     segment_count_out);
        }
        if ((flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_INDIRECT) != 0) {
            return KB2_TEST_VQ_MALFORMED;
        }
        segments_out[count].data = kb2_test_resolve(virtqueue, address, length, rights);
        segments_out[count].address = address;
        segments_out[count].length = length;
        segments_out[count].flags = flags;
        ++count;
        if ((flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_NEXT) == 0) {
            *segment_count_out = count;
            return KB2_TEST_VQ_OK;
        }
        current = next;
    }
    return KB2_TEST_VQ_MALFORMED;
}

kb2_test_vq_status_t kb2_test_vq_read_chain(kb2_test_vq_t *virtqueue,
                                            uint16_t head,
                                            kb2_test_vq_segment_t *segments_out,
                                            size_t segment_capacity,
                                            size_t *segment_count_out) {
    if (virtqueue == NULL || segments_out == NULL || segment_count_out == NULL ||
        segment_capacity == 0 || head >= virtqueue->queue.queue_size) {
        return KB2_TEST_VQ_INVALID_ARGUMENT;
    }
    *segment_count_out = 0;
    return kb2_test_vq_read_descriptor_chain(virtqueue,
                                             virtqueue->descriptor_table,
                                             (uint16_t)virtqueue->queue.queue_size,
                                             head,
                                             0,
                                             segments_out,
                                             segment_capacity,
                                             segment_count_out);
}

kb2_test_vq_status_t kb2_test_vq_complete(kb2_test_vq_t *virtqueue,
                                          uint16_t head,
                                          uint32_t length,
                                          int *notify_out) {
    uint16_t old_index;
    uint16_t new_index;
    uint16_t event_index;
    size_t slot;
    uint8_t *element;

    if (virtqueue == NULL || notify_out == NULL || head >= virtqueue->queue.queue_size ||
        virtqueue->used_producer_index == virtqueue->available_consumer_index) {
        return KB2_TEST_VQ_INVALID_ARGUMENT;
    }
    old_index = virtqueue->used_producer_index;
    new_index = (uint16_t)(old_index + 1u);
    slot = old_index & (virtqueue->queue.queue_size - 1u);
    element = virtqueue->used_ring + KB2_TEST_VQ_USED_RING_OFFSET +
              slot * KB2_TEST_VQ_USED_ELEMENT_SIZE;
    kb2_test_store_u32(element, head);
    kb2_test_store_u32(element + 4, length);
    kb2_test_atomic_store_release(virtqueue->used_ring + KB2_TEST_VQ_USED_INDEX_OFFSET,
                                  new_index);
    virtqueue->used_producer_index = new_index;
    event_index = kb2_test_atomic_load_acquire(
        virtqueue->available_ring + KB2_TEST_VQ_AVAILABLE_RING_OFFSET +
        (size_t)virtqueue->queue.queue_size * 2u);
    *notify_out = kb2_test_notification_needed(event_index, new_index, old_index);
    return KB2_TEST_VQ_OK;
}

kb2_test_vq_status_t kb2_test_vq_inject_used_id(kb2_test_vq_t *virtqueue,
                                                uint32_t head,
                                                uint32_t length,
                                                int *notify_out) {
    uint16_t old_index;
    uint16_t new_index;
    uint16_t event_index;
    size_t slot;
    uint8_t *element;

    if (virtqueue == NULL || notify_out == NULL ||
        virtqueue->used_producer_index == virtqueue->available_consumer_index) {
        return KB2_TEST_VQ_INVALID_ARGUMENT;
    }
    old_index = virtqueue->used_producer_index;
    new_index = (uint16_t)(old_index + 1u);
    slot = old_index & (virtqueue->queue.queue_size - 1u);
    element = virtqueue->used_ring + KB2_TEST_VQ_USED_RING_OFFSET +
              slot * KB2_TEST_VQ_USED_ELEMENT_SIZE;
    kb2_test_store_u32(element, head);
    kb2_test_store_u32(element + 4, length);
    kb2_test_atomic_store_release(virtqueue->used_ring + KB2_TEST_VQ_USED_INDEX_OFFSET,
                                  new_index);
    virtqueue->used_producer_index = new_index;
    event_index = kb2_test_atomic_load_acquire(
        virtqueue->available_ring + KB2_TEST_VQ_AVAILABLE_RING_OFFSET +
        (size_t)virtqueue->queue.queue_size * 2u);
    *notify_out = kb2_test_notification_needed(event_index, new_index, old_index);
    return KB2_TEST_VQ_OK;
}

kb2_test_vq_status_t kb2_test_vq_take_used(kb2_test_vq_t *virtqueue,
                                           uint16_t *head_out,
                                           uint32_t *length_out) {
    uint16_t producer_index;
    uint16_t distance;
    uint32_t head;
    size_t slot;
    const uint8_t *element;

    if (virtqueue == NULL || head_out == NULL || length_out == NULL) {
        return KB2_TEST_VQ_INVALID_ARGUMENT;
    }
    producer_index =
        kb2_test_atomic_load_acquire(virtqueue->used_ring + KB2_TEST_VQ_USED_INDEX_OFFSET);
    distance = (uint16_t)(producer_index - virtqueue->used_consumer_index);
    if (distance == 0) {
        return KB2_TEST_VQ_EMPTY;
    }
    if (distance > virtqueue->queue.queue_size) {
        return KB2_TEST_VQ_MALFORMED;
    }
    slot = virtqueue->used_consumer_index & (virtqueue->queue.queue_size - 1u);
    element = virtqueue->used_ring + KB2_TEST_VQ_USED_RING_OFFSET +
              slot * KB2_TEST_VQ_USED_ELEMENT_SIZE;
    head = kb2_test_load_u32(element);
    *length_out = kb2_test_load_u32(element + 4);
    ++virtqueue->used_consumer_index;
    kb2_test_atomic_store_release(
        virtqueue->available_ring + KB2_TEST_VQ_AVAILABLE_RING_OFFSET +
            (size_t)virtqueue->queue.queue_size * 2u,
        virtqueue->used_consumer_index);
    if (head >= virtqueue->queue.queue_size ||
        !kb2_test_outstanding(virtqueue, (uint16_t)head)) {
        return KB2_TEST_VQ_MALFORMED;
    }
    kb2_test_set_outstanding(virtqueue, (uint16_t)head, 0);
    *head_out = (uint16_t)head;
    return KB2_TEST_VQ_OK;
}
