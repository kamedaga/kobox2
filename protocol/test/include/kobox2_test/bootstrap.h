/* SPDX-License-Identifier: MIT */

#ifndef KOBOX2_TEST_BOOTSTRAP_H
#define KOBOX2_TEST_BOOTSTRAP_H

#include <stddef.h>
#include <stdint.h>

#define KB2_TEST_BOOTSTRAP_FD 3
#define KB2_TEST_NOTIFICATION_COUNT 4u
#define KB2_TEST_TRANSFER_FD_COUNT (1u + KB2_TEST_NOTIFICATION_COUNT)

typedef struct kb2_test_bootstrap {
    uint64_t generation;
    uint64_t shared_memory_size;
    uint32_t channel_descriptor_size;
    uint32_t notification_ids[KB2_TEST_NOTIFICATION_COUNT];
} kb2_test_bootstrap_t;

int kb2_test_send_bootstrap(int socket_fd,
                            const kb2_test_bootstrap_t *bootstrap,
                            const int *file_descriptors,
                            size_t file_descriptor_count);
int kb2_test_receive_bootstrap(int socket_fd,
                               kb2_test_bootstrap_t *bootstrap_out,
                               int *file_descriptors_out,
                               size_t file_descriptor_capacity,
                               size_t *file_descriptor_count_out);

#endif
