/* SPDX-License-Identifier: Apache-2.0 */

#ifndef KOBOX2_TEST_LINUX_HOST_ADAPTER_H
#define KOBOX2_TEST_LINUX_HOST_ADAPTER_H

#include <kobox2/controller.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "bootstrap.h"

typedef struct kb2_test_host {
    const char *sandbox_path;
    void *shared_memory;
    size_t shared_memory_size;
    size_t channel_descriptor_size;
    pid_t process_id;
    int bootstrap_socket;
    int shared_memory_fd;
    int notification_fds[KB2_TEST_NOTIFICATION_COUNT];
    uint32_t notification_ids[KB2_TEST_NOTIFICATION_COUNT];
    uint64_t generation;
    uint64_t resource_set_id;
    uint64_t sandbox_id;
    uint64_t next_resource_set_id;
    uint64_t next_sandbox_id;
    uint64_t next_channel_id;
    uint32_t next_notification_id;
    int resources_transferred;
    int resources_revoked;
} kb2_test_host_t;

int kb2_test_host_initialize(kb2_test_host_t *host, const char *sandbox_path);
void kb2_test_host_destroy(kb2_test_host_t *host);
kb2_status_t kb2_test_host_execute(kb2_test_host_t *host,
                                   const kb2_action_t *action,
                                   uint64_t *resource_set_id_out,
                                   uint64_t *sandbox_id_out);
uint64_t kb2_test_host_resource_set_id(const kb2_test_host_t *host);
uint64_t kb2_test_host_sandbox_id(const kb2_test_host_t *host);
int kb2_test_host_is_released(const kb2_test_host_t *host);

#endif
