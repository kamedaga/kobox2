/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <kobox2/virtqueue_x86_64.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr,                                                                        \
                    "%s:%d pid=%ld: %s (errno=%d)\n",                                              \
                    __FILE__,                                                                      \
                    __LINE__,                                                                      \
                    (long)getpid(),                                                                \
                    #expression,                                                                   \
                    errno);                                                                        \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

enum { QUEUE_SIZE = 16, BATCH = 4, ROUNDS = 17000, MEMORY_SIZE = 16384, BYTES = 64 };

struct endpoint {
    kb2_protocol_channel_t identity;
    kb2_protocol_queue_t queues[2];
    kb2_protocol_region_t region;
    kb2_vq_channel_t channel;
    kb2_vq_mapping_t mapping;
    kb2_vq_arena_t arenas[2];
    kb2_vq_mapped_memory_t mapped;
    kb2_vq_t queue;
    kb2_vq_chain_t *slots[QUEUE_SIZE];
    uint16_t owners[QUEUE_SIZE];
};

static void
bind_endpoint(struct endpoint *endpoint, void *mapping, uint64_t generation, kb2_vq_side_t side) {
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->identity =
        (kb2_protocol_channel_t){.feature_bits = KB2_PROTOCOL_TRANSPORT_FEATURES_REQUIRED,
                                 .channel_id = generation * 10,
                                 .generation = generation,
                                 .protocol_id = 1,
                                 .flags = KB2_PROTOCOL_CHANNEL_FLAG_MANAGEMENT};
    for (unsigned int i = 0; i < 2; ++i) {
        endpoint->queues[i] = (kb2_protocol_queue_t){.queue_id = i + 1,
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
                                                     .max_outstanding = BATCH};
        endpoint->arenas[i] = (kb2_vq_arena_t){
            .queue_id = i + 1, .rights = 3, .transport_base = 4096 + i * 4096, .length = 4096};
    }
    endpoint->region = (kb2_protocol_region_t){.region_id = 1, .rights = 3, .length = MEMORY_SIZE};
    endpoint->mapping =
        (kb2_vq_mapping_t){.address = mapping, .length = MEMORY_SIZE, .local_rights = 3};
    CHECK(kb2_vq_mapped_memory_init(&endpoint->mapped,
                                    &endpoint->mapping,
                                    1,
                                    endpoint->arenas,
                                    2,
                                    &kb2_vq_x86_64_atomics) == KB2_VQ_OK);
    kb2_vq_memory_t memory = kb2_vq_mapped_memory_ops(&endpoint->mapped);
    CHECK(kb2_vq_channel_init(
              &endpoint->channel, &endpoint->identity, endpoint->queues, 2, &endpoint->region, 1) ==
          KB2_VQ_OK);
    CHECK(kb2_vq_bind(&endpoint->queue,
                      &endpoint->channel,
                      2,
                      side,
                      &memory,
                      endpoint->slots,
                      endpoint->owners,
                      QUEUE_SIZE) == KB2_VQ_OK);
}

static void kick(int fd) {
    uint64_t count = 1;
    CHECK(write(fd, &count, sizeof(count)) == sizeof(count));
}

static void await_progress(struct endpoint *endpoint, int fd) {
    int ready;
    CHECK(kb2_vq_arm(&endpoint->queue, endpoint->identity.generation, &ready) == KB2_VQ_OK);
    if (ready)
        return;
    struct pollfd event = {.fd = fd, .events = POLLIN};
    CHECK(poll(&event, 1, 5000) == 1 && (event.revents & POLLIN));
    uint64_t count;
    CHECK(read(fd, &count, sizeof(count)) == sizeof(count) && count);
    /* A wake is not a work item: the caller rechecks the ring. */
}

static unsigned char pattern(unsigned int round, unsigned int slot, unsigned int byte) {
    return (unsigned char)(round * 17 + slot * 37 + byte * 13);
}

static void driver(void *mapping, uint64_t generation, int available, int used) {
    struct endpoint endpoint;
    bind_endpoint(&endpoint, mapping, generation, KB2_VQ_DRIVER);
    for (unsigned int round = 0; round < ROUNDS; ++round) {
        kb2_vq_segment_t segments[BATCH][2];
        kb2_vq_chain_t chains[BATCH];
        int notify = 0;
        for (unsigned int i = 0; i < BATCH; ++i) {
            uint64_t address = 8192 + i * 256;
            for (unsigned int byte = 0; byte < BYTES; ++byte)
                ((unsigned char *)mapping)[address + byte] = pattern(round, i, byte);
            segments[i][0] = (kb2_vq_segment_t){address, BYTES, (uint16_t)(3 * i), 0};
            segments[i][1] = (kb2_vq_segment_t){address + 128, BYTES, (uint16_t)(3 * i + 1), 1};
            chains[i] = (kb2_vq_chain_t){
                .segments = segments[i], .capacity = 2, .count = 2, .head = (uint16_t)(3 * i)};
            if (i == 1 || i == 2) {
                chains[i].indirect = 10240 + i * 128;
                chains[i].indirect_length = 64;
                chains[i].indirect_first = i == 2;
                chains[i].indirect_descriptor = (uint16_t)(3 * i + (i == 2));
                if (i == 1)
                    segments[i][0].descriptor = 0;
                segments[i][1].descriptor = i == 1 ? 3 : 0;
            }
            int current;
            CHECK(kb2_vq_publish(&endpoint.queue, generation, &chains[i], &current) == KB2_VQ_OK);
            notify |= current;
        }
        if (notify)
            kick(available); /* One kick for a whole batch. */
        if (!(round % 7))
            kick(available); /* Deliberate redundant notification. */
        for (unsigned int i = 0; i < BATCH;) {
            kb2_vq_chain_t *done = NULL;
            kb2_vq_status_t status = kb2_vq_take_used(&endpoint.queue, generation, &done);
            if (status == KB2_VQ_EMPTY) {
                await_progress(&endpoint, used);
                continue;
            }
            CHECK(status == KB2_VQ_OK && done == &chains[BATCH - 1 - i] &&
                  done->used_length == BYTES);
            unsigned char response[BYTES];
            CHECK(kb2_vq_copy_response(&endpoint.queue, generation, done, 0, response, BYTES) ==
                  KB2_VQ_OK);
            for (unsigned int byte = 0; byte < BYTES; ++byte)
                CHECK(response[byte] ==
                      (unsigned char)(pattern(round, BATCH - 1 - i, BYTES - 1 - byte) ^ 0xa5));
            CHECK(kb2_vq_release(&endpoint.queue, generation, done) == KB2_VQ_OK);
            ++i;
        }
    }
    CHECK(!endpoint.queue.owned && endpoint.queue.produced == (uint16_t)(ROUNDS * BATCH));
}

static void device(void *mapping, uint64_t generation, int available, int used) {
    struct endpoint endpoint;
    bind_endpoint(&endpoint, mapping, generation, KB2_VQ_DEVICE);
    for (unsigned int round = 0; round < ROUNDS; ++round) {
        kb2_vq_segment_t segments[BATCH][QUEUE_SIZE];
        kb2_vq_chain_t chains[BATCH];
        for (unsigned int i = 0; i < BATCH;) {
            chains[i] = (kb2_vq_chain_t){.segments = segments[i], .capacity = QUEUE_SIZE};
            kb2_vq_status_t status = kb2_vq_take_available(&endpoint.queue, generation, &chains[i]);
            if (status == KB2_VQ_EMPTY) {
                await_progress(&endpoint, available);
                continue;
            }
            CHECK(status == KB2_VQ_OK && chains[i].head == 3 * i);
            unsigned char request[BYTES], response[BYTES];
            CHECK(kb2_vq_copy_request(&endpoint.queue, generation, &chains[i], 0, request, BYTES) ==
                  KB2_VQ_OK);
            for (unsigned int byte = 0; byte < BYTES; ++byte) {
                CHECK(request[byte] == pattern(round, i, byte));
                response[BYTES - 1 - byte] = request[byte] ^ 0xa5;
            }
            CHECK(kb2_vq_write_response(&endpoint.queue, generation, &chains[i], response, BYTES) ==
                  KB2_VQ_OK);
            ++i;
        }
        int notify = 0;
        for (int i = BATCH - 1; i >= 0; --i) {
            int current;
            CHECK(kb2_vq_complete(&endpoint.queue, generation, &chains[i], &current) == KB2_VQ_OK);
            notify |= current;
        }
        if (notify)
            kick(used);
        if (!(round % 11))
            kick(used);
    }
    CHECK(!endpoint.queue.owned && endpoint.queue.produced == (uint16_t)(ROUNDS * BATCH));
}

static void generation_run(uint64_t generation) {
    int fd = memfd_create("kobox2-virtqueue-process-test", MFD_CLOEXEC);
    CHECK(fd >= 0 && !ftruncate(fd, MEMORY_SIZE));
    void *mapping = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(mapping != MAP_FAILED);
    int available = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int used = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(available >= 0 && used >= 0);
    pid_t parent = getpid();
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(!prctl(PR_SET_PDEATHSIG, SIGKILL) && getppid() == parent);
        /* Retain the inherited mapping until the replacement is established:
         * the endpoints must resolve the same transport address differently. */
        void *child_mapping = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        CHECK(child_mapping != MAP_FAILED && child_mapping != mapping);
        CHECK(!munmap(mapping, MEMORY_SIZE) && !close(fd));
        device(child_mapping, generation, available, used);
        CHECK(!munmap(child_mapping, MEMORY_SIZE));
        CHECK(!close(available) && !close(used));
        _exit(0);
    }
    CHECK(!close(fd));
    driver(mapping, generation, available, used);
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    CHECK(!munmap(mapping, MEMORY_SIZE));
    CHECK(!close(available) && !close(used));
}

int main(void) {
    alarm(30);
    generation_run(9);
    generation_run(10);
    puts("virtqueue process: 68000 requests x 2 fresh generations, SG/mixed, coalesced wakes, wrap "
         "PASS");
    return 0;
}
