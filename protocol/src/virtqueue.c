/* SPDX-License-Identifier: MIT */
#include <kobox2/virtqueue.h>

#include <limits.h>
#include <string.h>

enum { FREE, SUBMITTED, ACCEPTED, RETURNED };
enum { DESC_SIZE = 16, DESC_NEXT = 1, DESC_WRITE = 2, DESC_INDIRECT = 4 };

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)read_u16(p) | (uint32_t)read_u16(p + 2) << 16;
}

static uint64_t read_u64(const uint8_t *p) {
    return read_u32(p) | (uint64_t)read_u32(p + 4) << 32;
}

static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void write_u32(uint8_t *p, uint32_t v) {
    write_u16(p, (uint16_t)v);
    write_u16(p + 2, (uint16_t)(v >> 16));
}

static void write_u64(uint8_t *p, uint64_t v) {
    write_u32(p, (uint32_t)v);
    write_u32(p + 4, (uint32_t)(v >> 32));
}

static int range_valid(uint64_t address, uint64_t size) {
    return size && address <= UINT64_MAX - size;
}

static int overlaps(uint64_t a, uint64_t n, uint64_t b, uint64_t m) {
    return n && m && a < b + m && b < a + n;
}

static uint64_t ring_base(const kb2_protocol_queue_t *q, unsigned int ring) {
    return ring == 0 ? q->descriptor_address : ring == 1 ? q->available_address : q->used_address;
}

static uint64_t ring_size(const kb2_protocol_queue_t *q, unsigned int ring) {
    return ring == 0 ? (uint64_t)q->queue_size * 16
                     : 6 + (uint64_t)q->queue_size * (ring == 1 ? 2 : 8);
}

static int payload_range(const kb2_vq_t *vq, uint64_t address, uint64_t size, uint32_t rights) {
    const kb2_vq_channel_t *channel = vq->channel;
    int found = 0;
    if (!size)
        return 1; /* Empty descriptors do not authorize any memory access. */
    if (!range_valid(address, size))
        return 0;
    for (size_t i = 0; i < channel->region_count; ++i) {
        const kb2_protocol_region_t *r = &channel->regions[i];
        if (address >= r->transport_base && address + size <= r->transport_base + r->length &&
            (r->rights & rights) == rights)
            found = 1;
    }
    if (!found)
        return 0;
    /* A payload may not alias any queue's metadata, including another lane. */
    for (size_t i = 0; i < channel->queue_count; ++i)
        for (unsigned int ring = 0; ring < 3; ++ring)
            if (overlaps(address,
                         size,
                         ring_base(&channel->queues[i], ring),
                         ring_size(&channel->queues[i], ring)))
                return 0;
    return vq->memory.payload_allowed(
               vq->memory.context, vq->queue.queue_id, address, size, rights) != 0;
}

void kb2_vq_channel_fault(kb2_vq_channel_t *channel) {
    if (channel)
        atomic_store_explicit(&channel->faulted, 1, memory_order_release);
}

static kb2_vq_status_t fault(kb2_vq_t *vq, int io) {
    kb2_vq_channel_fault(vq->channel);
    return io ? KB2_VQ_IO : KB2_VQ_FAULTED;
}

static kb2_vq_status_t check(kb2_vq_t *vq, uint64_t generation) {
    if (!vq || !vq->channel || !generation)
        return KB2_VQ_INVALID;
    if (vq->channel->identity.generation != generation)
        return KB2_VQ_STALE;
    return atomic_load_explicit(&vq->channel->faulted, memory_order_acquire) ? KB2_VQ_FAULTED
                                                                             : KB2_VQ_OK;
}

kb2_vq_status_t kb2_vq_channel_init(kb2_vq_channel_t *out,
                                    const kb2_protocol_channel_t *identity,
                                    const kb2_protocol_queue_t *queues,
                                    size_t queue_count,
                                    const kb2_protocol_region_t *regions,
                                    size_t region_count) {
    if (!out || kb2_protocol_channel_validate(identity, queues, queue_count, regions, region_count))
        return KB2_VQ_INVALID;
    out->identity = *identity;
    out->queues = queues;
    out->queue_count = queue_count;
    out->regions = regions;
    out->region_count = region_count;
    atomic_init(&out->faulted, 0);
    return KB2_VQ_OK;
}

kb2_vq_status_t kb2_vq_bind(kb2_vq_t *out,
                            kb2_vq_channel_t *channel,
                            uint32_t queue_id,
                            kb2_vq_side_t side,
                            const kb2_vq_memory_t *memory,
                            kb2_vq_chain_t **slots,
                            uint16_t *descriptor_owner,
                            size_t capacity) {
    const kb2_protocol_queue_t *queue = NULL;
    uint16_t index;
    if (!out || !channel || !memory || !memory->read || !memory->write || !memory->load_acquire ||
        !memory->store_release || !memory->fence || !memory->payload_allowed || !slots ||
        !descriptor_owner || (side != KB2_VQ_DRIVER && side != KB2_VQ_DEVICE))
        return KB2_VQ_INVALID;
    if (atomic_load_explicit(&channel->faulted, memory_order_acquire))
        return KB2_VQ_FAULTED;
    for (size_t i = 0; i < channel->queue_count; ++i)
        if (channel->queues[i].queue_id == queue_id)
            queue = &channel->queues[i];
    if (!queue || capacity < queue->queue_size)
        return KB2_VQ_INVALID;
    kb2_vq_t candidate = {.channel = channel,
                          .queue = *queue,
                          .memory = *memory,
                          .side = side,
                          .slots = slots,
                          .descriptor_owner = descriptor_owner};
    uint64_t owned_ring = side == KB2_VQ_DRIVER ? queue->available_address : queue->used_address;
    if (memory->load_acquire(memory->context, owned_ring + 2, &index))
        return fault(&candidate, 1);
    if (index)
        return fault(&candidate, 0); /* Fresh-generation binding, never in-place recovery. */
    memset(slots, 0, queue->queue_size * sizeof(*slots));
    memset(descriptor_owner, 0, queue->queue_size * sizeof(*descriptor_owner));
    *out = candidate;
    return KB2_VQ_OK;
}

static int owned(const kb2_vq_t *vq, const kb2_vq_chain_t *chain, unsigned int state) {
    return chain && chain->head < vq->queue.queue_size && chain->state == state &&
           vq->slots[chain->head] == chain;
}

static int chains_overlap(const kb2_vq_chain_t *a, const kb2_vq_chain_t *b) {
    uint64_t at = a->indirect_length;
    uint64_t bt = b->indirect_length;
    if (overlaps(a->indirect, at, b->indirect, bt))
        return 1;
    for (size_t i = 0; i < a->count; ++i) {
        const kb2_vq_segment_t *left = &a->segments[i];
        if (overlaps(left->address, left->length, b->indirect, bt))
            return 1;
        for (size_t j = 0; j < b->count; ++j) {
            const kb2_vq_segment_t *right = &b->segments[j];
            if ((left->writable || right->writable) &&
                overlaps(left->address, left->length, right->address, right->length))
                return 1;
        }
    }
    for (size_t i = 0; i < b->count; ++i)
        if (overlaps(a->indirect, at, b->segments[i].address, b->segments[i].length))
            return 1;
    return 0;
}

static int validate_chain(kb2_vq_t *vq, kb2_vq_chain_t *chain) {
    uint64_t readable = 0, writable = 0;
    int seen_writable = 0;
    if (!chain->segments || !chain->count || chain->count > chain->capacity ||
        chain->count > vq->queue.max_chain_length || chain->head >= vq->queue.queue_size ||
        vq->slots[chain->head] || vq->descriptor_owner[chain->head])
        return 0;
    size_t direct_count = chain->indirect_length ? chain->indirect_first : chain->count;
    if (chain->indirect_length) {
        if ((chain->indirect & 15) || chain->indirect_length % DESC_SIZE ||
            chain->indirect_length / DESC_SIZE > vq->queue.max_indirect_length ||
            direct_count >= chain->count || chain->indirect_descriptor >= vq->queue.queue_size ||
            vq->descriptor_owner[chain->indirect_descriptor] ||
            chain->segments[direct_count].descriptor != 0 ||
            !payload_range(
                vq, chain->indirect, chain->indirect_length, KB2_PROTOCOL_REGION_RIGHT_READ))
            return 0;
    } else if (chain->indirect || chain->indirect_first || chain->indirect_descriptor) {
        return 0;
    }
    uint16_t first = direct_count ? chain->segments[0].descriptor : chain->indirect_descriptor;
    if (first != chain->head)
        return 0;
    for (size_t i = 0; i < chain->count; ++i) {
        const kb2_vq_segment_t *s = &chain->segments[i];
        int in_table = i >= direct_count;
        if (s->writable > 1 || (!s->writable && seen_writable) ||
            !payload_range(vq,
                           s->address,
                           s->length,
                           s->writable ? KB2_PROTOCOL_REGION_RIGHT_WRITE
                                       : KB2_PROTOCOL_REGION_RIGHT_READ) ||
            overlaps(s->address, s->length, chain->indirect, chain->indirect_length))
            return 0;
        if (in_table) {
            if (s->descriptor >= chain->indirect_length / DESC_SIZE)
                return 0;
        } else if (s->descriptor >= vq->queue.queue_size || vq->descriptor_owner[s->descriptor] ||
                   (chain->indirect_length && s->descriptor == chain->indirect_descriptor)) {
            return 0;
        }
        for (size_t j = 0; j < i; ++j) {
            const kb2_vq_segment_t *previous = &chain->segments[j];
            if (((j >= direct_count) == in_table && s->descriptor == previous->descriptor) ||
                ((s->writable || previous->writable) &&
                 overlaps(s->address, s->length, previous->address, previous->length)))
                return 0;
        }
        seen_writable |= s->writable;
        if (s->writable)
            writable += s->length;
        else
            readable += s->length;
    }
    if (!writable ||
        (vq->queue.role == KB2_PROTOCOL_QUEUE_ROLE_REQUEST ? !readable : readable != 0))
        return 0;
    for (size_t head = 0; head < vq->queue.queue_size; ++head)
        if (vq->slots[head] && chains_overlap(chain, vq->slots[head]))
            return 0;
    chain->readable = readable;
    chain->writable = writable;
    chain->written = chain->used_length = 0;
    return 1;
}

static void claim(kb2_vq_t *vq, kb2_vq_chain_t *chain, unsigned int state) {
    vq->slots[chain->head] = chain;
    ++vq->owned;
    size_t direct_count = chain->indirect_length ? chain->indirect_first : chain->count;
    if (chain->indirect_length)
        vq->descriptor_owner[chain->indirect_descriptor] = (uint16_t)(chain->head + 1);
    for (size_t i = 0; i < direct_count; ++i)
        vq->descriptor_owner[chain->segments[i].descriptor] = (uint16_t)(chain->head + 1);
    chain->state = state;
}

static void drop(kb2_vq_t *vq, kb2_vq_chain_t *chain) {
    size_t direct_count = chain->indirect_length ? chain->indirect_first : chain->count;
    if (chain->indirect_length)
        vq->descriptor_owner[chain->indirect_descriptor] = 0;
    for (size_t i = 0; i < direct_count; ++i)
        vq->descriptor_owner[chain->segments[i].descriptor] = 0;
    vq->slots[chain->head] = NULL;
    --vq->owned;
    chain->state = FREE;
}

static kb2_vq_status_t publish_index(kb2_vq_t *vq, uint64_t ring, uint64_t event, int *notify) {
    uint16_t old = vq->produced, next = (uint16_t)(old + 1), threshold;
    if (vq->memory.store_release(vq->memory.context, ring + 2, next))
        return fault(vq, 1);
    vq->produced = next;
    vq->memory.fence(vq->memory.context);
    if (vq->memory.load_acquire(vq->memory.context, event, &threshold))
        return fault(vq, 1);
    *notify = (uint16_t)(next - threshold - 1) < (uint16_t)(next - old);
    return KB2_VQ_OK;
}

kb2_vq_status_t
kb2_vq_publish(kb2_vq_t *vq, uint64_t generation, kb2_vq_chain_t *chain, int *notify) {
    kb2_vq_status_t status = check(vq, generation);
    if (status)
        return status;
    if (vq->side != KB2_VQ_DRIVER || !chain || chain->state != FREE || !notify)
        return KB2_VQ_INVALID;
    if (vq->owned == vq->queue.max_outstanding)
        return KB2_VQ_FULL;
    if (!validate_chain(vq, chain))
        return KB2_VQ_INVALID;
    /* Validation is complete before the first shared write or ownership move. */
    claim(vq, chain, SUBMITTED);
    size_t direct_count = chain->indirect_length ? chain->indirect_first : chain->count;
    for (size_t i = 0; i < chain->count; ++i) {
        const kb2_vq_segment_t *s = &chain->segments[i];
        uint8_t bytes[DESC_SIZE] = {0};
        uint16_t flags = s->writable ? DESC_WRITE : 0;
        uint64_t table = i >= direct_count ? chain->indirect : vq->queue.descriptor_address;
        uint16_t index = s->descriptor;
        uint16_t next = 0;
        if (i + 1 < chain->count) {
            flags |= DESC_NEXT;
            next = i + 1 == direct_count ? chain->indirect_descriptor
                                         : chain->segments[i + 1].descriptor;
        }
        write_u64(bytes, s->address);
        write_u32(bytes + 8, s->length);
        write_u16(bytes + 12, flags);
        write_u16(bytes + 14, next);
        if (vq->memory.write(
                vq->memory.context, table + (uint64_t)index * DESC_SIZE, bytes, sizeof(bytes)))
            return fault(vq, 1);
    }
    if (chain->indirect_length) {
        uint8_t bytes[DESC_SIZE] = {0};
        write_u64(bytes, chain->indirect);
        write_u32(bytes + 8, chain->indirect_length);
        write_u16(bytes + 12, DESC_INDIRECT);
        if (vq->memory.write(vq->memory.context,
                             vq->queue.descriptor_address +
                                 (uint64_t)chain->indirect_descriptor * DESC_SIZE,
                             bytes,
                             sizeof(bytes)))
            return fault(vq, 1);
    }
    uint8_t head[2];
    write_u16(head, chain->head);
    uint64_t slot = vq->produced & (vq->queue.queue_size - 1);
    if (vq->memory.write(vq->memory.context, vq->queue.available_address + 4 + 2 * slot, head, 2))
        return fault(vq, 1);
    return publish_index(vq,
                         vq->queue.available_address,
                         vq->queue.used_address + 4 + (uint64_t)vq->queue.queue_size * 8,
                         notify);
}

static kb2_vq_status_t read_chain(kb2_vq_t *vq, kb2_vq_chain_t *chain) {
    uint64_t table = vq->queue.descriptor_address;
    uint32_t limit = vq->queue.queue_size;
    uint16_t index = chain->head;
    int indirect = 0;
    chain->count = 0;
    chain->indirect = 0;
    chain->indirect_length = 0;
    chain->indirect_first = 0;
    chain->indirect_descriptor = 0;
    for (;;) {
        uint8_t bytes[DESC_SIZE];
        if (index >= limit || chain->count >= vq->queue.max_chain_length ||
            chain->count >= chain->capacity)
            return fault(vq, 0);
        if (vq->memory.read(
                vq->memory.context, table + (uint64_t)index * DESC_SIZE, bytes, sizeof(bytes)))
            return fault(vq, 1);
        uint64_t address = read_u64(bytes);
        uint32_t length = read_u32(bytes + 8);
        uint16_t flags = read_u16(bytes + 12), next = read_u16(bytes + 14);
        if (flags & ~(DESC_NEXT | DESC_WRITE | DESC_INDIRECT))
            return fault(vq, 0);
        size_t first = indirect ? chain->indirect_first : 0;
        for (size_t i = first; i < chain->count; ++i)
            if (chain->segments[i].descriptor == index)
                return fault(vq, 0);
        if (flags & DESC_INDIRECT) {
            /* WRITE and a NEXT-clear next value are ignored on an indirect
             * reference, as required by Virtio. A direct prefix is legal. */
            if (indirect || (flags & DESC_NEXT) || (address & 15) || !length ||
                length % DESC_SIZE || length / DESC_SIZE > vq->queue.max_indirect_length ||
                !payload_range(vq, address, length, KB2_PROTOCOL_REGION_RIGHT_READ))
                return fault(vq, 0);
            table = address;
            limit = length / DESC_SIZE;
            chain->indirect = address;
            chain->indirect_length = length;
            chain->indirect_first = chain->count;
            chain->indirect_descriptor = index;
            indirect = 1;
            index = 0;
            continue;
        }
        chain->segments[chain->count++] = (kb2_vq_segment_t){.address = address,
                                                             .length = length,
                                                             .descriptor = index,
                                                             .writable = !!(flags & DESC_WRITE)};
        if (!(flags & DESC_NEXT)) {
            break;
        }
        index = next;
    }
    return validate_chain(vq, chain) ? KB2_VQ_OK : fault(vq, 0);
}

static kb2_vq_status_t pending_index(kb2_vq_t *vq, uint16_t *distance) {
    uint64_t peer_ring =
        vq->side == KB2_VQ_DRIVER ? vq->queue.used_address : vq->queue.available_address;
    uint16_t index;
    if (vq->memory.load_acquire(vq->memory.context, peer_ring + 2, &index))
        return fault(vq, 1);
    *distance = (uint16_t)(index - vq->consumed);
    if (*distance > vq->queue.max_outstanding ||
        (vq->side == KB2_VQ_DRIVER && *distance > (uint16_t)(vq->produced - vq->consumed)))
        return fault(vq, 0);
    return *distance ? KB2_VQ_OK : KB2_VQ_EMPTY;
}

kb2_vq_status_t kb2_vq_take_available(kb2_vq_t *vq, uint64_t generation, kb2_vq_chain_t *chain) {
    kb2_vq_status_t status = check(vq, generation);
    uint16_t distance;
    uint8_t bytes[2];
    if (status)
        return status;
    if (vq->side != KB2_VQ_DEVICE || !chain || chain->state != FREE || !chain->segments ||
        chain->capacity < vq->queue.max_chain_length)
        return KB2_VQ_INVALID;
    status = pending_index(vq, &distance);
    if (status)
        return status;
    if (vq->owned + distance > vq->queue.max_outstanding)
        return fault(vq, 0);
    uint64_t slot = vq->consumed & (vq->queue.queue_size - 1);
    if (vq->memory.read(vq->memory.context, vq->queue.available_address + 4 + 2 * slot, bytes, 2))
        return fault(vq, 1);
    chain->head = read_u16(bytes);
    if (chain->head >= vq->queue.queue_size || vq->slots[chain->head] ||
        vq->descriptor_owner[chain->head])
        return fault(vq, 0);
    status = read_chain(vq, chain);
    if (status)
        return status;
    claim(vq, chain, ACCEPTED);
    ++vq->consumed;
    return KB2_VQ_OK;
}

static kb2_vq_status_t copy_data(kb2_vq_t *vq,
                                 kb2_vq_chain_t *chain,
                                 int writable,
                                 uint64_t offset,
                                 void *bytes,
                                 size_t size,
                                 int write) {
    uint8_t *cursor = bytes;
    for (size_t i = 0; i < chain->count && size; ++i) {
        const kb2_vq_segment_t *s = &chain->segments[i];
        if (s->writable != writable)
            continue;
        if (offset >= s->length) {
            offset -= s->length;
            continue;
        }
        size_t part = s->length - (size_t)offset;
        if (part > size)
            part = size;
        int result = write ? vq->memory.write(vq->memory.context, s->address + offset, cursor, part)
                           : vq->memory.read(vq->memory.context, s->address + offset, cursor, part);
        if (result)
            return fault(vq, 1);
        cursor += part;
        size -= part;
        offset = 0;
    }
    return size ? fault(vq, 0) : KB2_VQ_OK;
}

kb2_vq_status_t kb2_vq_copy_request(kb2_vq_t *vq,
                                    uint64_t generation,
                                    kb2_vq_chain_t *chain,
                                    uint64_t offset,
                                    void *out,
                                    size_t size) {
    kb2_vq_status_t status = check(vq, generation);
    if (status)
        return status;
    if (vq->side != KB2_VQ_DEVICE || !owned(vq, chain, ACCEPTED) || (!out && size) ||
        offset > chain->readable || size > chain->readable - offset)
        return KB2_VQ_INVALID;
    return copy_data(vq, chain, 0, offset, out, size, 0);
}

kb2_vq_status_t kb2_vq_write_response(
    kb2_vq_t *vq, uint64_t generation, kb2_vq_chain_t *chain, const void *bytes, size_t size) {
    kb2_vq_status_t status = check(vq, generation);
    if (status)
        return status;
    if (vq->side != KB2_VQ_DEVICE || !owned(vq, chain, ACCEPTED) || (!bytes && size) ||
        size > chain->writable - chain->written || size > UINT32_MAX - chain->written)
        return KB2_VQ_INVALID;
    status = copy_data(vq, chain, 1, chain->written, (void *)bytes, size, 1);
    if (!status)
        chain->written += size;
    return status;
}

kb2_vq_status_t
kb2_vq_complete(kb2_vq_t *vq, uint64_t generation, kb2_vq_chain_t *chain, int *notify) {
    kb2_vq_status_t status = check(vq, generation);
    if (status)
        return status;
    if (vq->side != KB2_VQ_DEVICE || !owned(vq, chain, ACCEPTED) || !notify)
        return KB2_VQ_INVALID;
    uint8_t bytes[8];
    uint64_t slot = vq->produced & (vq->queue.queue_size - 1);
    write_u32(bytes, chain->head);
    write_u32(bytes + 4, (uint32_t)chain->written);
    if (vq->memory.write(
            vq->memory.context, vq->queue.used_address + 4 + 8 * slot, bytes, sizeof(bytes)))
        return fault(vq, 1);
    status = publish_index(vq,
                           vq->queue.used_address,
                           vq->queue.available_address + 4 + (uint64_t)vq->queue.queue_size * 2,
                           notify);
    if (!status)
        drop(vq, chain);
    return status;
}

kb2_vq_status_t kb2_vq_take_used(kb2_vq_t *vq, uint64_t generation, kb2_vq_chain_t **chain_out) {
    kb2_vq_status_t status = check(vq, generation);
    uint16_t distance;
    uint8_t bytes[8];
    if (status)
        return status;
    if (vq->side != KB2_VQ_DRIVER || !chain_out)
        return KB2_VQ_INVALID;
    status = pending_index(vq, &distance);
    if (status)
        return status;
    uint64_t slot = vq->consumed & (vq->queue.queue_size - 1);
    if (vq->memory.read(
            vq->memory.context, vq->queue.used_address + 4 + 8 * slot, bytes, sizeof(bytes)))
        return fault(vq, 1);
    uint32_t head = read_u32(bytes), length = read_u32(bytes + 4);
    if (head >= vq->queue.queue_size || !owned(vq, vq->slots[head], SUBMITTED) ||
        length > vq->slots[head]->writable)
        return fault(vq, 0);
    kb2_vq_chain_t *chain = vq->slots[head];
    chain->used_length = length;
    chain->state = RETURNED;
    ++vq->consumed;
    *chain_out = chain;
    return KB2_VQ_OK;
}

kb2_vq_status_t kb2_vq_copy_response(kb2_vq_t *vq,
                                     uint64_t generation,
                                     kb2_vq_chain_t *chain,
                                     uint64_t offset,
                                     void *out,
                                     size_t size) {
    kb2_vq_status_t status = check(vq, generation);
    if (status)
        return status;
    if (vq->side != KB2_VQ_DRIVER || !owned(vq, chain, RETURNED) || (!out && size) ||
        offset > chain->used_length || size > chain->used_length - offset)
        return KB2_VQ_INVALID;
    return copy_data(vq, chain, 1, offset, out, size, 0);
}

kb2_vq_status_t kb2_vq_release(kb2_vq_t *vq, uint64_t generation, kb2_vq_chain_t *chain) {
    kb2_vq_status_t status = check(vq, generation);
    if (status)
        return status;
    if (vq->side != KB2_VQ_DRIVER || !owned(vq, chain, RETURNED))
        return KB2_VQ_INVALID;
    drop(vq, chain);
    return KB2_VQ_OK;
}

kb2_vq_status_t kb2_vq_arm(kb2_vq_t *vq, uint64_t generation, int *ready) {
    kb2_vq_status_t status = check(vq, generation);
    uint16_t distance;
    if (status)
        return status;
    if (!ready)
        return KB2_VQ_INVALID;
    uint64_t event = vq->side == KB2_VQ_DRIVER
                         ? vq->queue.available_address + 4 + (uint64_t)vq->queue.queue_size * 2
                         : vq->queue.used_address + 4 + (uint64_t)vq->queue.queue_size * 8;
    if (vq->memory.store_release(vq->memory.context, event, vq->consumed))
        return fault(vq, 1);
    vq->memory.fence(vq->memory.context);
    status = pending_index(vq, &distance);
    if (status != KB2_VQ_OK && status != KB2_VQ_EMPTY)
        return status;
    *ready = status == KB2_VQ_OK;
    return KB2_VQ_OK;
}
