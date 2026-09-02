/* SPDX-License-Identifier: Apache-2.0 */

#ifndef KOBOX2_TEST_LINUX_HOST_ADAPTER_H
#define KOBOX2_TEST_LINUX_HOST_ADAPTER_H

#include <kobox2/controller.h>

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <kobox2_test/bootstrap.h>
#include <kobox2_test/management.h>
#include <kobox2_test/split_virtqueue.h>

typedef struct kb2_test_host {
    const char *sandbox_path;
    const char *artifact_paths[2];
    void *shared_memory;
    size_t shared_memory_size;
    size_t channel_descriptor_size;
    pid_t process_id;
    int process_fd;
    int bootstrap_socket;
    int shared_memory_fd;
    int manifest_fd;
    int artifact_fds[2];
    uint64_t artifact_sizes[2];
    uint8_t artifact_digests[2][32];
    uint64_t manifest_size;
    uint8_t manifest_digest[32];
    int notification_fds[KB2_TEST_NOTIFICATION_COUNT];
    uint32_t notification_ids[KB2_TEST_NOTIFICATION_COUNT];
    kb2_protocol_queue_t queues[2];
    kb2_protocol_region_t regions[KB2_TEST_REGION_COUNT];
    kb2_test_vq_t event_queue;
    kb2_test_vq_t request_queue;
    uint64_t generation;
    uint64_t resource_set_id;
    uint64_t sandbox_id;
    uint64_t next_resource_set_id;
    uint64_t next_sandbox_id;
    uint64_t next_channel_id;
    uint32_t next_notification_id;
    uint64_t next_correlation_id;
    int resources_transferred;
    int resources_revoked;
    int abnormal_exit_allowed;
    int process_exited;
} kb2_test_host_t;

int kb2_test_host_initialize(kb2_test_host_t *host,
                             const char *sandbox_path,
                             const char *core_path,
                             const char *module_path);
void kb2_test_host_destroy(kb2_test_host_t *host);
kb2_status_t kb2_test_host_execute(kb2_test_host_t *host,
                                   const kb2_action_t *action,
                                   uint64_t *resource_set_id_out,
                                   uint64_t *sandbox_id_out);
uint64_t kb2_test_host_resource_set_id(const kb2_test_host_t *host);
uint64_t kb2_test_host_sandbox_id(const kb2_test_host_t *host);
int kb2_test_host_is_released(const kb2_test_host_t *host);
int kb2_test_host_echo(kb2_test_host_t *host, uint64_t value, int indirect);
int kb2_test_host_run_fixture(kb2_test_host_t *host);
int kb2_test_host_echo_batch(kb2_test_host_t *host, uint64_t first_value, size_t count);
int kb2_test_host_request_quiesce(kb2_test_host_t *host);
int kb2_test_host_receive_ready(kb2_test_host_t *host);
int kb2_test_host_inject_fault(kb2_test_host_t *host,
                               kb2_test_fault_scenario_t scenario,
                               kb2_fault_kind_t *fault_kind_out,
                               uint64_t *fault_code_out);

#endif
