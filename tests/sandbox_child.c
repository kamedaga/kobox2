/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE

#include <kobox2/closure_manifest.h>
#include <kobox2/protocol.h>
#include <kobox2/sha256.h>

#include <kobox2_test/bootstrap.h>
#include <kobox2_test/management.h>
#include <kobox2_test/split_virtqueue.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void close_descriptors(int *file_descriptors, size_t count) {
    size_t index;

    for (index = 0; index < count; ++index) {
        if (file_descriptors[index] >= 0) {
            close(file_descriptors[index]);
            file_descriptors[index] = -1;
        }
    }
}

static int notification_index(const kb2_test_bootstrap_t *bootstrap, uint32_t notification_id) {
    size_t index;

    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        if (bootstrap->notification_ids[index] == notification_id) {
            return (int)index;
        }
    }
    return -1;
}

static int eventfd_write_one(int file_descriptor) {
    uint64_t value = 1;
    ssize_t bytes;

    do {
        bytes = write(file_descriptor, &value, sizeof(value));
    } while (bytes < 0 && errno == EINTR);
    return bytes == (ssize_t)sizeof(value);
}

static int eventfd_wait(int file_descriptor) {
    struct pollfd descriptor = {
        .fd = file_descriptor,
        .events = POLLIN,
    };
    uint64_t value;
    ssize_t bytes;
    int result;

    do {
        result = poll(&descriptor, 1, -1);
    } while (result < 0 && errno == EINTR);
    if (result != 1 || (descriptor.revents & POLLIN) == 0) {
        return 0;
    }
    do {
        bytes = read(file_descriptor, &value, sizeof(value));
    } while (bytes < 0 && errno == EINTR);
    return bytes == (ssize_t)sizeof(value) && value != 0;
}

static int send_event(kb2_test_vq_t *event_queue,
                      int notification_fd,
                      uint64_t generation,
                      uint32_t opcode,
                      uint64_t value) {
    kb2_test_vq_segment_t segment;
    size_t segment_count;
    uint16_t head;
    int notify;

    if (kb2_test_vq_take_available(event_queue, &head) != KB2_TEST_VQ_OK ||
        kb2_test_vq_read_chain(event_queue, head, &segment, 1, &segment_count) !=
            KB2_TEST_VQ_OK ||
        segment_count != 1 ||
        (segment.flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) == 0 ||
        segment.length < KB2_TEST_MESSAGE_SIZE ||
        !kb2_test_message_encode(segment.data,
                                 KB2_TEST_MESSAGE_SIZE,
                                 opcode,
                                 KB2_TEST_MESSAGE_FLAG_EVENT,
                                 generation,
                                 0,
                                 value) ||
        kb2_test_vq_complete(event_queue, head, KB2_TEST_MESSAGE_SIZE, &notify) !=
            KB2_TEST_VQ_OK ||
        (notify && !eventfd_write_one(notification_fd))) {
        return 0;
    }
    return 1;
}

static int validate_request_chain(kb2_test_vq_t *request_queue,
                                  uint16_t head,
                                  kb2_test_vq_segment_t segments[2]) {
    size_t segment_count;

    return kb2_test_vq_read_chain(request_queue, head, segments, 2, &segment_count) ==
               KB2_TEST_VQ_OK &&
           segment_count == 2 &&
           (segments[0].flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) == 0 &&
           (segments[1].flags & KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE) != 0 &&
           segments[0].length == KB2_TEST_MESSAGE_SIZE &&
           segments[1].length == KB2_TEST_MESSAGE_SIZE;
}

static void wait_for_kill(void) {
    for (;;) {
        pause();
    }
}

static int validate_closure_package(const kb2_test_bootstrap_t *bootstrap,
                                    const int *file_descriptors) {
    kb2_closure_manifest_t manifest;
    struct stat status;
    uint8_t digest[KB2_SHA256_DIGEST_SIZE];
    void *manifest_bytes = MAP_FAILED;
    size_t index;
    int result = 0;

    if (bootstrap->manifest_size > SIZE_MAX ||
        fstat(file_descriptors[1], &status) != 0 || status.st_size < 0 ||
        (uint64_t)status.st_size != bootstrap->manifest_size) {
        return 0;
    }
    manifest_bytes = mmap(NULL,
                          (size_t)bootstrap->manifest_size,
                          PROT_READ,
                          MAP_PRIVATE,
                          file_descriptors[1],
                          0);
    if (manifest_bytes == MAP_FAILED) {
        return 0;
    }
    kb2_sha256(manifest_bytes, (size_t)bootstrap->manifest_size, digest);
    if (memcmp(digest, bootstrap->manifest_digest, sizeof(digest)) != 0 ||
        kb2_closure_manifest_decode(manifest_bytes,
                                    (size_t)bootstrap->manifest_size,
                                    &manifest) != KB2_PROTOCOL_OK ||
        kb2_closure_manifest_artifact_count(&manifest) != bootstrap->artifact_count) {
        goto finish;
    }
    for (index = 0; index < bootstrap->artifact_count; ++index) {
        kb2_closure_manifest_artifact_t artifact;
        void *artifact_bytes = MAP_FAILED;

        if (kb2_closure_manifest_artifact(&manifest, index, &artifact) != KB2_PROTOCOL_OK ||
            artifact.content_size > SIZE_MAX ||
            fstat(file_descriptors[KB2_TEST_BASE_TRANSFER_FD_COUNT + index], &status) != 0 ||
            status.st_size < 0 || (uint64_t)status.st_size != artifact.content_size) {
            goto finish;
        }
        artifact_bytes = mmap(NULL,
                              (size_t)artifact.content_size,
                              PROT_READ,
                              MAP_PRIVATE,
                              file_descriptors[KB2_TEST_BASE_TRANSFER_FD_COUNT + index],
                              0);
        if (artifact_bytes == MAP_FAILED) {
            goto finish;
        }
        kb2_sha256(artifact_bytes, (size_t)artifact.content_size, digest);
        munmap(artifact_bytes, (size_t)artifact.content_size);
        if (memcmp(digest, artifact.content_digest, sizeof(digest)) != 0) {
            goto finish;
        }
    }
    result = 1;

finish:
    munmap(manifest_bytes, (size_t)bootstrap->manifest_size);
    return result;
}

int main(void) {
    kb2_test_bootstrap_t bootstrap = {0};
    kb2_protocol_channel_t channel;
    kb2_protocol_queue_t queues[2];
    kb2_protocol_region_t regions[KB2_TEST_REGION_COUNT];
    kb2_test_vq_t event_queue;
    kb2_test_vq_t request_queue;
    int file_descriptors[KB2_TEST_MAX_TRANSFER_FD_COUNT];
    struct stat shared_memory_status;
    void *shared_memory = MAP_FAILED;
    size_t file_descriptor_count;
    size_t index;
    size_t queue_count;
    size_t region_count;
    int descriptor_flags;
    int event_used_index;
    int request_available_index;
    int request_used_index;
    size_t notification_base;
    int result = 1;
    int stage = 1;

    memset(file_descriptors, -1, sizeof(file_descriptors));
    if (!kb2_test_receive_bootstrap(KB2_TEST_BOOTSTRAP_FD,
                                    &bootstrap,
                                    file_descriptors,
                                    KB2_TEST_MAX_TRANSFER_FD_COUNT,
                                    &file_descriptor_count) ||
        file_descriptor_count != kb2_test_bootstrap_descriptor_count(&bootstrap) ||
        bootstrap.generation == 0 ||
        bootstrap.shared_memory_size > (uint64_t)SIZE_MAX ||
        bootstrap.channel_descriptor_size > bootstrap.shared_memory_size ||
        fstat(file_descriptors[0], &shared_memory_status) != 0 ||
        shared_memory_status.st_size < 0 ||
        (uint64_t)shared_memory_status.st_size != bootstrap.shared_memory_size) {
        goto finish;
    }
    if (!validate_closure_package(&bootstrap, file_descriptors)) {
        goto finish;
    }
    stage = 2;
    notification_base = KB2_TEST_BASE_TRANSFER_FD_COUNT + bootstrap.artifact_count;
    close(KB2_TEST_BOOTSTRAP_FD);
    for (index = 0; index < file_descriptor_count; ++index) {
        descriptor_flags = fcntl(file_descriptors[index], F_GETFD);
        if (descriptor_flags < 0 || (descriptor_flags & FD_CLOEXEC) == 0) {
            goto finish;
        }
    }

    shared_memory = mmap(NULL,
                         (size_t)bootstrap.shared_memory_size,
                         PROT_READ | PROT_WRITE,
                         MAP_SHARED,
                         file_descriptors[0],
                         0);
    if (shared_memory == MAP_FAILED ||
        kb2_protocol_channel_decode(shared_memory,
                                    bootstrap.channel_descriptor_size,
                                    &channel,
                                    queues,
                                    2,
                                    &queue_count,
                                    regions,
                                    KB2_TEST_REGION_COUNT,
                                    &region_count) != KB2_PROTOCOL_OK ||
        queue_count != 2 || region_count != KB2_TEST_REGION_COUNT ||
        channel.generation != bootstrap.generation ||
        channel.protocol_id != KB2_TEST_PROTOCOL_ID ||
        channel.flags != KB2_PROTOCOL_CHANNEL_FLAG_MANAGEMENT ||
        queues[0].role != KB2_PROTOCOL_QUEUE_ROLE_EVENT ||
        queues[1].role != KB2_PROTOCOL_QUEUE_ROLE_REQUEST ||
        kb2_test_vq_bind(&event_queue,
                         shared_memory,
                         (size_t)bootstrap.shared_memory_size,
                         &queues[0],
                         regions,
                         region_count,
                         0) != KB2_TEST_VQ_OK ||
        kb2_test_vq_bind(&request_queue,
                         shared_memory,
                         (size_t)bootstrap.shared_memory_size,
                         &queues[1],
                         regions,
                         region_count,
                         0) != KB2_TEST_VQ_OK) {
        goto finish;
    }
    stage = 3;
    event_used_index = notification_index(&bootstrap, queues[0].used_notification_id);
    request_available_index =
        notification_index(&bootstrap, queues[1].available_notification_id);
    request_used_index = notification_index(&bootstrap, queues[1].used_notification_id);
    if (event_used_index < 0 || request_available_index < 0 || request_used_index < 0 ||
        !send_event(&event_queue,
                    file_descriptors[notification_base + (size_t)event_used_index],
                    bootstrap.generation,
                    KB2_TEST_EVENT_READY,
                    bootstrap.generation)) {
        goto finish;
    }

    for (;;) {
        int notify_used = 0;

        if (!eventfd_wait(
                file_descriptors[notification_base + (size_t)request_available_index])) {
            goto finish;
        }
        for (;;) {
            kb2_test_vq_segment_t segments[2];
            kb2_test_vq_status_t queue_status;
            uint64_t correlation;
            uint64_t value;
            uint32_t opcode;
            uint16_t head;
            int notify;

            queue_status = kb2_test_vq_take_available(&request_queue, &head);
            if (queue_status == KB2_TEST_VQ_EMPTY) {
                break;
            }
            if (queue_status != KB2_TEST_VQ_OK ||
                !validate_request_chain(&request_queue, head, segments) ||
                !kb2_test_message_decode(segments[0].data,
                                         segments[0].length,
                                         0,
                                         bootstrap.generation,
                                         &opcode,
                                         &correlation,
                                         &value)) {
                if (!send_event(&event_queue,
                                file_descriptors[notification_base + (size_t)event_used_index],
                                bootstrap.generation,
                                KB2_TEST_EVENT_FAULT,
                                KB2_TEST_PROTOCOL_FAULT_MALFORMED_DESCRIPTOR)) {
                    goto finish;
                }
                wait_for_kill();
            }
            if (opcode == KB2_TEST_REQUEST_PAUSE_AFTER_ACQUIRE) {
                if (!send_event(&event_queue,
                                file_descriptors[notification_base + (size_t)event_used_index],
                                bootstrap.generation,
                                KB2_TEST_EVENT_ACQUIRED,
                                correlation)) {
                    goto finish;
                }
                wait_for_kill();
            }
            if (opcode == KB2_TEST_REQUEST_BAD_USED_ID) {
                if (kb2_test_vq_inject_used_id(&request_queue,
                                               request_queue.queue.queue_size,
                                               0,
                                               &notify) != KB2_TEST_VQ_OK ||
                    (notify &&
                     !eventfd_write_one(
                         file_descriptors[notification_base + (size_t)request_used_index]))) {
                    goto finish;
                }
                continue;
            }
            if (!kb2_test_message_encode(segments[1].data,
                                         segments[1].length,
                                         opcode,
                                         KB2_TEST_MESSAGE_FLAG_RESPONSE,
                                         opcode == KB2_TEST_REQUEST_BAD_GENERATION
                                             ? bootstrap.generation + 1u
                                             : bootstrap.generation,
                                         correlation,
                                         value)) {
                goto finish;
            }
            if (opcode == KB2_TEST_REQUEST_BAD_ENVELOPE) {
                segments[1].data[KB2_PROTOCOL_MESSAGE_ENVELOPE_RESERVED_OFFSET] = 1;
            }
            if (kb2_test_vq_complete(
                    &request_queue, head, KB2_TEST_MESSAGE_SIZE, &notify) != KB2_TEST_VQ_OK) {
                goto finish;
            }
            notify_used |= notify;
            if (opcode == KB2_TEST_REQUEST_PAUSE_AFTER_USED) {
                if (!send_event(&event_queue,
                                file_descriptors[notification_base + (size_t)event_used_index],
                                bootstrap.generation,
                                KB2_TEST_EVENT_USED_PUBLISHED,
                                correlation)) {
                    goto finish;
                }
                wait_for_kill();
            }
            if (opcode == KB2_TEST_REQUEST_QUIESCE) {
                if (notify_used &&
                    !eventfd_write_one(
                        file_descriptors[notification_base + (size_t)request_used_index])) {
                    goto finish;
                }
                if (!send_event(&event_queue,
                                file_descriptors[notification_base + (size_t)event_used_index],
                                bootstrap.generation,
                                KB2_TEST_EVENT_STOPPED,
                                0)) {
                    goto finish;
                }
                result = 0;
                goto finish;
            }
        }
        if (notify_used &&
            !eventfd_write_one(
                file_descriptors[notification_base + (size_t)request_used_index])) {
            goto finish;
        }
    }

finish:
    if (result != 0) {
        fprintf(stderr, "sandbox child failed at stage %d\n", stage);
    }
    if (shared_memory != MAP_FAILED) {
        munmap(shared_memory, (size_t)bootstrap.shared_memory_size);
    }
    close_descriptors(file_descriptors, KB2_TEST_MAX_TRANSFER_FD_COUNT);
    close(KB2_TEST_BOOTSTRAP_FD);
    return result;
}
