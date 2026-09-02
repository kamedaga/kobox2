/* SPDX-License-Identifier: Apache-2.0 */

#ifndef KOBOX2_TEST_BOOTSTRAP_H
#define KOBOX2_TEST_BOOTSTRAP_H

#include <stddef.h>
#include <stdint.h>

#define KB2_TEST_BOOTSTRAP_FD 3
#define KB2_TEST_NOTIFICATION_COUNT 4u
#define KB2_TEST_TRANSFER_FD_COUNT (1u + KB2_TEST_NOTIFICATION_COUNT)
#define KB2_TEST_SHARED_ACK_OFFSET 512u
#define KB2_TEST_COMMAND_REVOKE 1u
#define KB2_TEST_ACK_REVOKED 1u

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
int kb2_test_send_word(int socket_fd, uint32_t value);
int kb2_test_receive_word(int socket_fd, uint32_t *value_out, int timeout_milliseconds);
void kb2_test_store_u64(uint8_t *destination, uint64_t value);
uint64_t kb2_test_load_u64(const uint8_t *source);

#endif
