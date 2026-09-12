/* SPDX-License-Identifier: Apache-2.0 */
#include <kobox2/virtqueue_memory.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                       \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

enum { QUEUE_SIZE = 16, MEMORY_SIZE = 16384, GENERATION = 9, READ = 1, WRITE = 2 };

struct endpoint {
    kb2_vq_channel_t channel;
    kb2_vq_t queue;
    kb2_vq_chain_t *slots[QUEUE_SIZE];
    uint16_t owners[QUEUE_SIZE];
};

struct fixture {
    _Alignas(16) unsigned char bytes[MEMORY_SIZE];
    kb2_protocol_channel_t identity;
    kb2_protocol_queue_t queues[2];
    kb2_protocol_region_t region;
    kb2_vq_mapping_t mapping;
    kb2_vq_arena_t arenas[2];
    kb2_vq_mapped_memory_t mapped;
    kb2_vq_memory_t base, memory;
    struct endpoint driver, device;
    unsigned int io_count, fail_after;
    void (*on_fence)(struct fixture *);
    kb2_vq_chain_t *race_chain;
};

static uint16_t load16(const void *address) {
    const unsigned char *p = address;
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static void store16(void *address, uint16_t value) {
    unsigned char *p = address;
    p[0] = (unsigned char)value;
    p[1] = (unsigned char)(value >> 8);
}

static void store32(void *address, uint32_t value) {
    unsigned char *p = address;
    store16(p, (uint16_t)value);
    store16(p + 2, (uint16_t)(value >> 16));
}

static void store64(void *address, uint64_t value) {
    unsigned char *p = address;
    store32(p, (uint32_t)value);
    store32(p + 4, (uint32_t)(value >> 32));
}

/* Single-thread deterministic tests use byte operations. Interprocess atomic
 * ordering is tested separately with the actual architecture backend. */
static void no_fence(void) {
}

static int fail_io(struct fixture *f) {
    ++f->io_count;
    return f->fail_after && !--f->fail_after;
}

static int read_bytes(void *context, uint64_t address, void *out, size_t size) {
    struct fixture *f = context;
    return fail_io(f) ? -1 : f->base.read(f->base.context, address, out, size);
}

static int write_bytes(void *context, uint64_t address, const void *bytes, size_t size) {
    struct fixture *f = context;
    return fail_io(f) ? -1 : f->base.write(f->base.context, address, bytes, size);
}

static int load_index(void *context, uint64_t address, uint16_t *value) {
    struct fixture *f = context;
    return fail_io(f) ? -1 : f->base.load_acquire(f->base.context, address, value);
}

static int store_index(void *context, uint64_t address, uint16_t value) {
    struct fixture *f = context;
    return fail_io(f) ? -1 : f->base.store_release(f->base.context, address, value);
}

static int
payload_allowed(void *context, uint32_t queue, uint64_t address, uint64_t size, uint32_t rights) {
    struct fixture *f = context;
    return f->base.payload_allowed(f->base.context, queue, address, size, rights);
}

static void full_fence(void *context) {
    struct fixture *f = context;
    if (f->on_fence) {
        void (*hook)(struct fixture *) = f->on_fence;
        f->on_fence = NULL;
        hook(f);
    }
}

static void
bind_endpoint(struct fixture *f, struct endpoint *endpoint, kb2_vq_side_t side, uint32_t queue_id) {
    CHECK(kb2_vq_channel_init(&endpoint->channel, &f->identity, f->queues, 2, &f->region, 1) ==
          KB2_VQ_OK);
    CHECK(kb2_vq_bind(&endpoint->queue,
                      &endpoint->channel,
                      queue_id,
                      side,
                      &f->memory,
                      endpoint->slots,
                      endpoint->owners,
                      QUEUE_SIZE) == KB2_VQ_OK);
}

static void init(struct fixture *f, int event, uint32_t max_outstanding) {
    memset(f, 0, sizeof(*f));
    f->identity = (kb2_protocol_channel_t){.feature_bits = KB2_PROTOCOL_TRANSPORT_FEATURES_REQUIRED,
                                           .channel_id = 5,
                                           .generation = GENERATION,
                                           .protocol_id = 1,
                                           .flags = KB2_PROTOCOL_CHANNEL_FLAG_MANAGEMENT};
    for (unsigned int i = 0; i < 2; ++i) {
        f->queues[i] = (kb2_protocol_queue_t){.queue_id = i + 1,
                                              .role = i ? KB2_PROTOCOL_QUEUE_ROLE_REQUEST
                                                        : KB2_PROTOCOL_QUEUE_ROLE_EVENT,
                                              .queue_size = QUEUE_SIZE,
                                              .descriptor_address = 512 + i * 512,
                                              .available_address = 768 + i * 512,
                                              .used_address = 808 + i * 512,
                                              .available_notification_id = i * 2 + 1,
                                              .used_notification_id = i * 2 + 2,
                                              .max_chain_length = QUEUE_SIZE,
                                              .max_indirect_length = 32,
                                              .max_outstanding = max_outstanding};
        f->arenas[i] = (kb2_vq_arena_t){.queue_id = i + 1,
                                        .rights = READ | WRITE,
                                        .transport_base = 4096 + i * 4096,
                                        .length = 4096};
    }
    f->region = (kb2_protocol_region_t){
        .region_id = 1, .rights = READ | WRITE, .transport_base = 0, .length = MEMORY_SIZE};
    f->mapping = (kb2_vq_mapping_t){.transport_base = 0,
                                    .length = MEMORY_SIZE,
                                    .address = f->bytes,
                                    .local_rights = READ | WRITE};
    const kb2_vq_atomic_ops_t atomics = {load16, store16, no_fence};
    CHECK(kb2_vq_mapped_memory_init(&f->mapped, &f->mapping, 1, f->arenas, 2, &atomics) ==
          KB2_VQ_OK);
    f->base = kb2_vq_mapped_memory_ops(&f->mapped);
    f->memory = (kb2_vq_memory_t){.context = f,
                                  .read = read_bytes,
                                  .write = write_bytes,
                                  .load_acquire = load_index,
                                  .store_release = store_index,
                                  .fence = full_fence,
                                  .payload_allowed = payload_allowed};
    bind_endpoint(f, &f->driver, KB2_VQ_DRIVER, event ? 1 : 2);
    bind_endpoint(f, &f->device, KB2_VQ_DEVICE, event ? 1 : 2);
}

static kb2_vq_chain_t request(kb2_vq_segment_t segments[2], uint16_t head, uint64_t address) {
    segments[0] = (kb2_vq_segment_t){.address = address, .length = 8, .descriptor = head};
    segments[1] = (kb2_vq_segment_t){
        .address = address + 32, .length = 8, .descriptor = (uint16_t)(head + 1), .writable = 1};
    return (kb2_vq_chain_t){.segments = segments, .capacity = 2, .count = 2, .head = head};
}

static void round_trip(void) {
    struct fixture f;
    init(&f, 0, QUEUE_SIZE);
    kb2_vq_segment_t segments[] = {
        {8192, 3, 0, 0}, {8200, 5, 4, 0}, {8224, 2, 8, 1}, {8232, 6, 12, 1}};
    kb2_vq_segment_t snapshot[QUEUE_SIZE];
    kb2_vq_chain_t send = {.segments = segments, .capacity = 4, .count = 4};
    kb2_vq_chain_t receive = {.segments = snapshot, .capacity = QUEUE_SIZE};
    kb2_vq_chain_t *done = NULL;
    int notify, ready;
    unsigned char bytes[8];
    memcpy(f.bytes + 8192, "abc", 3);
    memcpy(f.bytes + 8200, "defgh", 5);
    CHECK(kb2_vq_arm(&f.device.queue, GENERATION, &ready) == KB2_VQ_OK && !ready);
    CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK && notify);
    CHECK(kb2_vq_arm(&f.device.queue, GENERATION, &ready) == KB2_VQ_OK && ready);
    CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive) == KB2_VQ_OK);
    /* The peer can edit descriptors after publication; dispatch uses only the snapshot. */
    memset(f.bytes + f.queues[1].descriptor_address, 0xff, QUEUE_SIZE * 16);
    CHECK(kb2_vq_copy_request(&f.device.queue, GENERATION, &receive, 1, bytes, 6) == KB2_VQ_OK);
    CHECK(!memcmp(bytes, "bcdefg", 6));
    CHECK(kb2_vq_write_response(&f.device.queue, GENERATION, &receive, "12", 2) == KB2_VQ_OK);
    CHECK(kb2_vq_write_response(&f.device.queue, GENERATION, &receive, "3456789", 7) ==
          KB2_VQ_INVALID);
    CHECK(kb2_vq_write_response(&f.device.queue, GENERATION, &receive, "345678", 6) == KB2_VQ_OK);
    CHECK(kb2_vq_complete(&f.device.queue, GENERATION, &receive, &notify) == KB2_VQ_OK && notify);
    CHECK(kb2_vq_complete(&f.device.queue, GENERATION, &receive, &notify) == KB2_VQ_INVALID);
    CHECK(kb2_vq_take_used(&f.driver.queue, GENERATION, &done) == KB2_VQ_OK && done == &send);
    CHECK(done->used_length == 8 && f.driver.queue.owned == 1);
    CHECK(kb2_vq_copy_response(&f.driver.queue, GENERATION, done, 0, bytes, 8) == KB2_VQ_OK);
    CHECK(!memcmp(bytes, "12345678", 8));
    CHECK(kb2_vq_copy_response(&f.driver.queue, GENERATION, done, 1, bytes, 8) == KB2_VQ_INVALID);
    CHECK(kb2_vq_release(&f.driver.queue, GENERATION, done) == KB2_VQ_OK);
    CHECK(kb2_vq_release(&f.driver.queue, GENERATION, done) == KB2_VQ_INVALID);
    CHECK(!f.driver.queue.owned && !f.device.queue.owned);
}

static void indirect_chains(void) {
    for (size_t prefix = 0; prefix <= 1; ++prefix) {
        struct fixture f;
        init(&f, 0, QUEUE_SIZE);
        kb2_vq_segment_t segments[] = {{8192, 8, 0, 0}, {8224, 8, 3, 1}};
        /* With a direct prefix, both tables may legitimately use index 0. */
        if (prefix)
            segments[1].descriptor = 0;
        kb2_vq_chain_t send = {.segments = segments,
                               .capacity = 2,
                               .count = 2,
                               .head = prefix ? 0 : 7,
                               .indirect = 8448,
                               .indirect_length = 128,
                               .indirect_first = prefix,
                               .indirect_descriptor = 7};
        kb2_vq_segment_t snapshot[QUEUE_SIZE];
        kb2_vq_chain_t receive = {.segments = snapshot, .capacity = QUEUE_SIZE};
        int notify;
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK);
        unsigned char *root = f.bytes + f.queues[1].descriptor_address + 7 * 16;
        store16(root + 12, 6);     /* INDIRECT | WRITE: WRITE is ignored. */
        store16(root + 14, 65535); /* NEXT is clear. */
        store16(f.bytes + send.indirect + segments[1].descriptor * 16 + 14, 65535);
        CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive) == KB2_VQ_OK);
        CHECK(receive.count == 2 && receive.indirect_length == 128 &&
              receive.indirect_first == prefix);
        CHECK(f.device.owners[7] == send.head + 1);
        if (prefix)
            CHECK(f.device.owners[0] == 1);
        CHECK(kb2_vq_complete(&f.device.queue, GENERATION, &receive, &notify) == KB2_VQ_OK);
        CHECK(!f.device.owners[7] && !f.device.owners[0]);
    }
}

static void local_rejection(void) {
    for (unsigned int defect = 0; defect < 9; ++defect) {
        struct fixture f;
        init(&f, 0, QUEUE_SIZE);
        kb2_vq_segment_t segments[2];
        kb2_vq_chain_t send = request(segments, 0, 8192);
        switch (defect) {
        case 0:
            segments[1].descriptor = 0;
            break;
        case 1:
            segments[1].address = segments[0].address;
            break;
        case 2:
            segments[1].address = f.queues[0].used_address;
            break;
        case 3:
            segments[1].address = 0;
            break; /* reserved channel header */
        case 4:
            segments[1].address = 4096;
            break; /* another lane's arena */
        case 5:
            segments[1].address = UINT64_MAX - 2;
            break;
        case 6:
            segments[1].writable = 2;
            break;
        case 7:
            segments[0].writable = 1;
            break;
        case 8:
            send.count = 17;
            break;
        }
        unsigned int before = f.io_count;
        int notify = 77;
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_INVALID);
        CHECK(f.io_count == before && notify == 77 && !send.state && !f.driver.queue.owned);
        CHECK(!atomic_load(&f.driver.channel.faulted));
    }
}

static void peer_rejection(void) {
    for (unsigned int defect = 0; defect < 14; ++defect) {
        struct fixture f;
        init(&f, 0, QUEUE_SIZE);
        kb2_vq_segment_t segments[2], snapshot[QUEUE_SIZE];
        kb2_vq_chain_t send = request(segments, 0, 8192);
        kb2_vq_chain_t receive = {.segments = snapshot, .capacity = QUEUE_SIZE};
        int notify;
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK);
        unsigned char *desc = f.bytes + f.queues[1].descriptor_address;
        unsigned char *avail = f.bytes + f.queues[1].available_address;
        switch (defect) {
        case 0:
            store16(avail + 4, 16);
            break;
        case 1:
            store16(avail + 2, 17);
            break;
        case 2:
            store16(desc + 14, 0);
            break; /* cycle */
        case 3:
            store16(desc + 14, 16);
            break;
        case 4:
            store16(desc + 12, 8);
            break;
        case 5:
            store64(desc, UINT64_MAX - 2);
            break;
        case 6:
            store64(desc + 16, 8192);
            break; /* writable alias */
        case 7:
            store64(desc + 16, f.queues[0].descriptor_address);
            break;
        case 8:
            store64(desc + 16, 4096);
            break;
        case 9:
            store16(desc + 12, 5);
            break; /* INDIRECT | NEXT */
        case 10:
            store16(desc + 12, 4);
            store32(desc + 8, 15);
            break;
        case 11:
            store16(desc + 12, 3);
            store16(desc + 28, 0);
            break; /* WRITE then READ */
        case 12:
            store64(desc + 16, 0);
            break;
        case 13:
            store64(desc + 16, 12284);
            break; /* crosses authorized arena */
        }
        CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive) == KB2_VQ_FAULTED);
        unsigned int before = f.io_count;
        CHECK(kb2_vq_arm(&f.device.queue, GENERATION, &notify) == KB2_VQ_FAULTED);
        CHECK(f.io_count == before);
    }
}

static void ownership_and_completion(void) {
    struct fixture f;
    init(&f, 0, 2);
    kb2_vq_segment_t segments[3][2], snapshots[2][QUEUE_SIZE];
    kb2_vq_chain_t send[3], receive[2];
    int notify;
    for (unsigned int i = 0; i < 3; ++i)
        send[i] = request(segments[i], (uint16_t)(i * 2), 8192 + 64 * i);
    for (unsigned int i = 0; i < 2; ++i) {
        receive[i] = (kb2_vq_chain_t){.segments = snapshots[i], .capacity = QUEUE_SIZE};
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send[i], &notify) == KB2_VQ_OK);
        CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive[i]) == KB2_VQ_OK);
    }
    CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send[2], &notify) == KB2_VQ_FULL);
    for (int i = 1; i >= 0; --i) {
        kb2_vq_chain_t *done = NULL;
        CHECK(kb2_vq_complete(&f.device.queue, GENERATION, &receive[i], &notify) == KB2_VQ_OK);
        CHECK(kb2_vq_take_used(&f.driver.queue, GENERATION, &done) == KB2_VQ_OK &&
              done == &send[i]);
        CHECK(!done->used_length);
        if (i == 1)
            CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send[2], &notify) == KB2_VQ_FULL);
        CHECK(kb2_vq_release(&f.driver.queue, GENERATION, done) == KB2_VQ_OK);
    }
    CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send[2], &notify) == KB2_VQ_OK);
    /* Repeated available head while still owned is a channel fault. */
    CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive[0]) == KB2_VQ_OK);
    store16(f.bytes + f.queues[1].available_address + 4 + 3 * 2, send[2].head);
    store16(f.bytes + f.queues[1].available_address + 2, 4);
    CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive[1]) == KB2_VQ_FAULTED);
}

static void malformed_used(void) {
    for (unsigned int defect = 0; defect < 4; ++defect) {
        struct fixture f;
        init(&f, 0, QUEUE_SIZE);
        kb2_vq_segment_t segments[2];
        kb2_vq_chain_t send = request(segments, 0, 8192), *done = NULL;
        int notify;
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK);
        unsigned char *used = f.bytes + f.queues[1].used_address;
        store16(used + 2, 1);
        switch (defect) {
        case 0:
            store32(used + 4, 16);
            break;
        case 1:
            store32(used + 4, 1);
            break; /* tail descriptor, not owned head */
        case 2:
            store32(used + 8, 9);
            break;
        case 3:
            store16(used + 2, 2);
            break; /* more completions than published requests */
        }
        CHECK(kb2_vq_take_used(&f.driver.queue, GENERATION, &done) == KB2_VQ_FAULTED && !done);
        CHECK(f.driver.queue.owned == 1 && !f.driver.queue.consumed);
    }
}

static void publish_in_arm(struct fixture *f) {
    int notify;
    CHECK(kb2_vq_publish(&f->driver.queue, GENERATION, f->race_chain, &notify) == KB2_VQ_OK &&
          notify);
}

static void notifications_and_wrap(void) {
    struct fixture f;
    init(&f, 0, QUEUE_SIZE);
    kb2_vq_segment_t segments[2], snapshot[QUEUE_SIZE];
    kb2_vq_chain_t send = request(segments, 0, 8192);
    kb2_vq_chain_t receive = {.segments = snapshot, .capacity = QUEUE_SIZE};
    kb2_vq_chain_t *done;
    int ready, notify;
    f.race_chain = &send;
    f.on_fence = publish_in_arm;
    CHECK(kb2_vq_arm(&f.device.queue, GENERATION, &ready) == KB2_VQ_OK && ready);
    for (unsigned int round = 0; round < 65540; ++round) {
        if (round) {
            CHECK(kb2_vq_arm(&f.device.queue, GENERATION, &ready) == KB2_VQ_OK && !ready);
            CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK &&
                  notify);
        }
        CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive) == KB2_VQ_OK);
        CHECK(kb2_vq_arm(&f.driver.queue, GENERATION, &ready) == KB2_VQ_OK && !ready);
        CHECK(kb2_vq_complete(&f.device.queue, GENERATION, &receive, &notify) == KB2_VQ_OK &&
              notify);
        CHECK(kb2_vq_take_used(&f.driver.queue, GENERATION, &done) == KB2_VQ_OK);
        CHECK(kb2_vq_release(&f.driver.queue, GENERATION, done) == KB2_VQ_OK);
    }
    CHECK(f.driver.queue.produced == 4 && f.device.queue.produced == 4);
    /* A consumer that has not rearmed can suppress a redundant kick. */
    CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK && !notify);
    CHECK(kb2_vq_arm(&f.device.queue, GENERATION, &ready) == KB2_VQ_OK && ready);
}

static void event_and_generation(void) {
    struct fixture f;
    init(&f, 1, QUEUE_SIZE);
    kb2_vq_segment_t segment = {4096, 16, 0, 1}, snapshot[QUEUE_SIZE];
    kb2_vq_chain_t send = {.segments = &segment, .capacity = 1, .count = 1};
    kb2_vq_chain_t receive = {.segments = snapshot, .capacity = QUEUE_SIZE};
    kb2_vq_chain_t *done;
    int notify;
    unsigned int before = f.io_count;
    CHECK(kb2_vq_publish(&f.driver.queue, GENERATION + 1, &send, &notify) == KB2_VQ_STALE);
    CHECK(f.io_count == before);
    CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK);
    CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive) == KB2_VQ_OK);
    CHECK(!receive.readable);
    CHECK(kb2_vq_write_response(&f.device.queue, GENERATION, &receive, "event", 5) == KB2_VQ_OK);
    CHECK(kb2_vq_complete(&f.device.queue, GENERATION, &receive, &notify) == KB2_VQ_OK);
    CHECK(kb2_vq_take_used(&f.driver.queue, GENERATION, &done) == KB2_VQ_OK &&
          done->used_length == 5);
    kb2_vq_t other;
    kb2_vq_chain_t *slots[QUEUE_SIZE];
    uint16_t owners[QUEUE_SIZE];
    CHECK(kb2_vq_bind(
              &other, &f.driver.channel, 2, KB2_VQ_DRIVER, &f.memory, slots, owners, QUEUE_SIZE) ==
          KB2_VQ_OK);
    kb2_vq_channel_fault(&f.driver.channel);
    before = f.io_count;
    CHECK(kb2_vq_arm(&other, GENERATION, &notify) == KB2_VQ_FAULTED);
    CHECK(kb2_vq_release(&f.driver.queue, GENERATION, done) == KB2_VQ_FAULTED);
    CHECK(f.io_count == before && f.driver.queue.owned == 1);
}

static void io_failures(void) {
    /* Every publication callback can fail, including the event-index read
     * after publication. Such a request is never retryable or released. */
    for (unsigned int failure = 1; failure <= 5; ++failure) {
        struct fixture f;
        init(&f, 0, QUEUE_SIZE);
        kb2_vq_segment_t segments[2];
        kb2_vq_chain_t send = request(segments, 0, 8192);
        int notify = 77;
        f.fail_after = failure;
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_IO);
        CHECK(notify == 77 && f.driver.queue.owned == 1 && send.state);
        unsigned int before = f.io_count;
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_FAULTED);
        CHECK(before == f.io_count);
    }
    for (unsigned int failure = 1; failure <= 4; ++failure) {
        struct fixture f;
        init(&f, 0, QUEUE_SIZE);
        kb2_vq_segment_t segments[2], snapshot[QUEUE_SIZE];
        kb2_vq_chain_t send = request(segments, 0, 8192);
        kb2_vq_chain_t receive = {.segments = snapshot, .capacity = QUEUE_SIZE};
        int notify;
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK);
        f.fail_after = failure;
        CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive) == KB2_VQ_IO);
        CHECK(!receive.state && !f.device.queue.consumed);
    }
}

static void mapping_validation(void) {
    struct fixture f;
    init(&f, 0, QUEUE_SIZE);
    kb2_vq_mapped_memory_t out;
    const kb2_vq_atomic_ops_t atomics = {load16, store16, no_fence};
    kb2_vq_mapping_t mappings[2] = {f.mapping, f.mapping};
    CHECK(kb2_vq_mapped_memory_init(&out, mappings, 2, f.arenas, 2, &atomics) == KB2_VQ_INVALID);
    mappings[1].transport_base = MEMORY_SIZE;
    CHECK(kb2_vq_mapped_memory_init(&out, mappings, 2, f.arenas, 2, &atomics) == KB2_VQ_INVALID);
    f.arenas[1].transport_base = f.arenas[0].transport_base;
    CHECK(kb2_vq_mapped_memory_init(&out, &f.mapping, 1, f.arenas, 2, &atomics) == KB2_VQ_INVALID);
    uint16_t value = 77;
    CHECK(f.base.load_acquire(f.base.context, 1, &value) && value == 77);
    CHECK(f.base.load_acquire(f.base.context, MEMORY_SIZE, &value) && value == 77);
    CHECK(f.base.write(f.base.context, UINT64_MAX, "x", 1));
}

static void indirect_rejection_and_leases(void) {
    for (unsigned int defect = 0; defect < 9; ++defect) {
        struct fixture f;
        init(&f, 0, QUEUE_SIZE);
        kb2_vq_segment_t segments[] = {{8192, 8, 0, 0}, {8224, 8, 3, 1}};
        kb2_vq_chain_t send = {.segments = segments,
                               .capacity = 2,
                               .count = 2,
                               .head = 7,
                               .indirect = 8448,
                               .indirect_length = 128,
                               .indirect_descriptor = 7};
        kb2_vq_segment_t snapshot[QUEUE_SIZE];
        kb2_vq_chain_t receive = {.segments = snapshot, .capacity = QUEUE_SIZE};
        int notify;
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK);
        unsigned char *root = f.bytes + f.queues[1].descriptor_address + 7 * 16;
        unsigned char *table = f.bytes + 8448;
        switch (defect) {
        case 0:
            store32(root + 8, 0);
            break;
        case 1:
            store32(root + 8, 129);
            break;
        case 2:
            store32(root + 8, 1024);
            break;
        case 3:
            store16(table + 12, 4);
            break; /* Nested indirect table. */
        case 4:
            store16(table + 14, 0);
            break; /* Cycle. */
        case 5:
            store16(table + 14, 8);
            break; /* Past the full table lease. */
        case 6:
            store64(table + 3 * 16, 8512);
            break; /* Payload aliases an unused table entry. */
        case 7:
            store16(root + 12, 5);
            break;
        case 8:
            store64(root, 4096);
            break; /* Other lane. */
        }
        CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive) == KB2_VQ_FAULTED);
    }
    struct fixture f;
    init(&f, 0, QUEUE_SIZE);
    kb2_vq_segment_t segments[2][2], snapshots[2][QUEUE_SIZE];
    kb2_vq_chain_t first = request(segments[0], 0, 8192);
    first.indirect = 8448;
    first.indirect_length = 128;
    first.indirect_descriptor = 0;
    kb2_vq_chain_t second = request(segments[1], 2, 8704);
    kb2_vq_chain_t received[2] = {{.segments = snapshots[0], .capacity = QUEUE_SIZE},
                                  {.segments = snapshots[1], .capacity = QUEUE_SIZE}};
    int notify;
    CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &first, &notify) == KB2_VQ_OK);
    CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &received[0]) == KB2_VQ_OK);
    segments[1][0].address = 8512;
    CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &second, &notify) == KB2_VQ_INVALID);
    segments[1][0].address = 8704;
    CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &second, &notify) == KB2_VQ_OK);
    store64(f.bytes + f.queues[1].descriptor_address + 2 * 16, 8512);
    CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &received[1]) == KB2_VQ_FAULTED);
    CHECK(f.device.queue.owned == 1); /* First lease survives the channel fault. */
}

static void completion_io_failures(void) {
    for (unsigned int stage = 0; stage < 10; ++stage) {
        struct fixture f;
        init(&f, 0, QUEUE_SIZE);
        kb2_vq_segment_t segments[2], snapshot[QUEUE_SIZE];
        kb2_vq_chain_t send = request(segments, 0, 8192), *done = NULL;
        kb2_vq_chain_t receive = {.segments = snapshot, .capacity = QUEUE_SIZE};
        int notify;
        unsigned char bytes[8] = {0};
        CHECK(kb2_vq_publish(&f.driver.queue, GENERATION, &send, &notify) == KB2_VQ_OK);
        CHECK(kb2_vq_take_available(&f.device.queue, GENERATION, &receive) == KB2_VQ_OK);
        kb2_vq_status_t status;
        if (stage == 0) {
            f.fail_after = 1;
            status = kb2_vq_copy_request(&f.device.queue, GENERATION, &receive, 0, bytes, 8);
        } else if (stage == 1) {
            f.fail_after = 1;
            status = kb2_vq_write_response(&f.device.queue, GENERATION, &receive, bytes, 8);
        } else {
            CHECK(kb2_vq_write_response(&f.device.queue, GENERATION, &receive, bytes, 8) ==
                  KB2_VQ_OK);
            if (stage <= 4) {
                f.fail_after = stage - 1;
                status = kb2_vq_complete(&f.device.queue, GENERATION, &receive, &notify);
            } else {
                CHECK(kb2_vq_complete(&f.device.queue, GENERATION, &receive, &notify) == KB2_VQ_OK);
                if (stage <= 6) {
                    f.fail_after = stage - 4;
                    status = kb2_vq_take_used(&f.driver.queue, GENERATION, &done);
                    CHECK(!done);
                } else if (stage == 7) {
                    CHECK(kb2_vq_take_used(&f.driver.queue, GENERATION, &done) == KB2_VQ_OK);
                    f.fail_after = 1;
                    status = kb2_vq_copy_response(&f.driver.queue, GENERATION, done, 0, bytes, 8);
                } else {
                    f.fail_after = stage - 7;
                    status = kb2_vq_arm(&f.driver.queue, GENERATION, &notify);
                }
            }
        }
        CHECK(status == KB2_VQ_IO);
        CHECK(f.driver.queue.owned == 1);
        CHECK(stage > 4 || f.device.queue.owned == 1);
    }
}

int main(void) {
    round_trip();
    indirect_chains();
    local_rejection();
    peer_rejection();
    ownership_and_completion();
    malformed_used();
    notifications_and_wrap();
    event_and_generation();
    io_failures();
    mapping_validation();
    indirect_rejection_and_leases();
    completion_io_failures();
    puts("virtqueue engine: ownership, indirect/mixed, bounds, faults, EVENT_IDX and wrap PASS");
    return 0;
}
