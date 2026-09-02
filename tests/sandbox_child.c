/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE

#include <kobox2/protocol.h>

#include "host/bootstrap.h"

#include <errno.h>
#include <fcntl.h>
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

static int validate_notifications(const kb2_test_bootstrap_t *bootstrap,
                                  const kb2_protocol_queue_t *queues,
                                  size_t queue_count) {
    size_t index;

    for (index = 0; index < queue_count; ++index) {
        if (notification_index(bootstrap, queues[index].available_notification_id) < 0 ||
            notification_index(bootstrap, queues[index].used_notification_id) < 0) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    kb2_test_bootstrap_t bootstrap = {0};
    kb2_protocol_channel_t channel;
    kb2_protocol_queue_t queues[2];
    kb2_protocol_region_t regions[1];
    int file_descriptors[KB2_TEST_TRANSFER_FD_COUNT];
    struct stat shared_memory_status;
    void *shared_memory = MAP_FAILED;
    size_t file_descriptor_count;
    size_t index;
    size_t queue_count;
    size_t region_count;
    uint64_t notification_value = 1;
    uint32_t command;
    int descriptor_flags;
    int notify_index;
    int result = 1;
    ssize_t bytes;

    memset(file_descriptors, -1, sizeof(file_descriptors));
    if (!kb2_test_receive_bootstrap(KB2_TEST_BOOTSTRAP_FD,
                                    &bootstrap,
                                    file_descriptors,
                                    KB2_TEST_TRANSFER_FD_COUNT,
                                    &file_descriptor_count) ||
        file_descriptor_count != KB2_TEST_TRANSFER_FD_COUNT || bootstrap.generation == 0 ||
        bootstrap.shared_memory_size > (uint64_t)SIZE_MAX ||
        bootstrap.shared_memory_size < KB2_TEST_SHARED_ACK_OFFSET + sizeof(uint64_t) ||
        bootstrap.channel_descriptor_size > bootstrap.shared_memory_size ||
        fstat(file_descriptors[0], &shared_memory_status) != 0 ||
        shared_memory_status.st_size < 0 ||
        (uint64_t)shared_memory_status.st_size != bootstrap.shared_memory_size) {
        goto finish;
    }
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
                                    1,
                                    &region_count) != KB2_PROTOCOL_OK ||
        queue_count != 2 || region_count != 1 || channel.generation != bootstrap.generation ||
        regions[0].length != bootstrap.shared_memory_size ||
        !validate_notifications(&bootstrap, queues, queue_count)) {
        goto finish;
    }

    notify_index = notification_index(&bootstrap, queues[0].used_notification_id);
    if (notify_index < 0) {
        goto finish;
    }
    kb2_test_store_u64((uint8_t *)shared_memory + KB2_TEST_SHARED_ACK_OFFSET,
                       bootstrap.generation);
    do {
        bytes = write(file_descriptors[(size_t)notify_index + 1u],
                      &notification_value,
                      sizeof(notification_value));
    } while (bytes < 0 && errno == EINTR);
    if (bytes != (ssize_t)sizeof(notification_value) ||
        !kb2_test_receive_word(KB2_TEST_BOOTSTRAP_FD, &command, 5000) ||
        command != KB2_TEST_COMMAND_REVOKE) {
        goto finish;
    }

    munmap(shared_memory, (size_t)bootstrap.shared_memory_size);
    shared_memory = MAP_FAILED;
    close_descriptors(file_descriptors, file_descriptor_count);
    if (!kb2_test_send_word(KB2_TEST_BOOTSTRAP_FD, KB2_TEST_ACK_REVOKED)) {
        goto finish;
    }
    result = 0;

finish:
    if (shared_memory != MAP_FAILED) {
        munmap(shared_memory, (size_t)bootstrap.shared_memory_size);
    }
    close_descriptors(file_descriptors, KB2_TEST_TRANSFER_FD_COUNT);
    close(KB2_TEST_BOOTSTRAP_FD);
    return result;
}
