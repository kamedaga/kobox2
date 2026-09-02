/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "linux_host_adapter.h"

#include <kobox2/protocol.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define KB2_TEST_MINIMUM_SHARED_MEMORY_SIZE 16384u
#define KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS 5000

static void kb2_test_close(int *file_descriptor) {
    if (*file_descriptor >= 0) {
        close(*file_descriptor);
        *file_descriptor = -1;
    }
}

static void kb2_test_host_release_local_resources(kb2_test_host_t *host) {
    size_t index;

    if (host->shared_memory != MAP_FAILED && host->shared_memory != NULL) {
        munmap(host->shared_memory, host->shared_memory_size);
    }
    host->shared_memory = MAP_FAILED;
    host->shared_memory_size = 0;
    host->channel_descriptor_size = 0;
    kb2_test_close(&host->shared_memory_fd);
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        kb2_test_close(&host->notification_fds[index]);
        host->notification_ids[index] = 0;
    }
    kb2_test_close(&host->bootstrap_socket);
    kb2_test_close(&host->process_fd);
    host->generation = 0;
    host->resource_set_id = 0;
    host->resources_transferred = 0;
    host->resources_revoked = 0;
    host->abnormal_exit_allowed = 0;
    host->reap_after_reset = 0;
}

static int kb2_test_wait_process(pid_t process_id,
                                 int *status_out,
                                 int timeout_milliseconds) {
    pid_t result;
    int wait_milliseconds;

    while (timeout_milliseconds >= 0) {
        do {
            result = waitpid(process_id, status_out, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == process_id) {
            return 1;
        }
        if (result < 0 || timeout_milliseconds == 0) {
            return 0;
        }
        wait_milliseconds = timeout_milliseconds < 10 ? timeout_milliseconds : 10;
        do {
            result = poll(NULL, 0, wait_milliseconds);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            return 0;
        }
        timeout_milliseconds -= wait_milliseconds;
    }
    return 0;
}

static int kb2_test_wait_process_fd(int process_fd, int timeout_milliseconds) {
    struct pollfd descriptor = {
        .fd = process_fd,
        .events = POLLIN,
    };
    int result;

    do {
        result = poll(&descriptor, 1, timeout_milliseconds);
    } while (result < 0 && errno == EINTR);
    return result == 1 && (descriptor.revents & POLLIN) != 0;
}

int kb2_test_host_initialize(kb2_test_host_t *host, const char *sandbox_path) {
    size_t index;

    if (host == NULL || sandbox_path == NULL || sandbox_path[0] == '\0') {
        return 0;
    }
    memset(host, 0, sizeof(*host));
    host->sandbox_path = sandbox_path;
    host->shared_memory = MAP_FAILED;
    host->process_id = -1;
    host->process_fd = -1;
    host->bootstrap_socket = -1;
    host->shared_memory_fd = -1;
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        host->notification_fds[index] = -1;
    }
    return 1;
}

void kb2_test_host_destroy(kb2_test_host_t *host) {
    int status;

    if (host == NULL) {
        return;
    }
    if (host->process_id > 0) {
        kill(host->process_id, SIGKILL);
        kb2_test_wait_process(
            host->process_id, &status, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS);
        host->process_id = -1;
    }
    kb2_test_host_release_local_resources(host);
    memset(host, 0, sizeof(*host));
}

static kb2_status_t kb2_test_host_make_channel(kb2_test_host_t *host,
                                               const kb2_action_t *action) {
    kb2_protocol_channel_t channel = {
        .feature_bits = KB2_PROTOCOL_TRANSPORT_FEATURES_REQUIRED,
        .generation = kb2_action_generation(action),
        .protocol_id = KB2_TEST_PROTOCOL_ID,
        .flags = KB2_PROTOCOL_CHANNEL_FLAG_MANAGEMENT,
    };
    size_t event_index;
    uint64_t outstanding;
    size_t index;
    int notify;

    memset(host->queues, 0, sizeof(host->queues));
    host->regions[0] = (kb2_protocol_region_t){
        .region_id = 1,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .transport_base = KB2_TEST_TRANSPORT_BASE,
        .length = KB2_TEST_EVENT_BUFFER_OFFSET,
    };
    host->regions[1] = (kb2_protocol_region_t){
        .region_id = 2,
        .rights = KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .transport_base = KB2_TEST_TRANSPORT_BASE + KB2_TEST_EVENT_BUFFER_OFFSET,
        .length = 0x1000,
    };
    host->regions[2] = (kb2_protocol_region_t){
        .region_id = 3,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ,
        .transport_base = KB2_TEST_TRANSPORT_BASE + KB2_TEST_REQUEST_BUFFER_OFFSET,
        .length = 0x1000,
    };
    host->regions[3] = (kb2_protocol_region_t){
        .region_id = 4,
        .rights = KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .transport_base = KB2_TEST_TRANSPORT_BASE + KB2_TEST_RESPONSE_BUFFER_OFFSET,
        .length = 0x1000,
    };
    host->regions[4] = (kb2_protocol_region_t){
        .region_id = 5,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ,
        .transport_base = KB2_TEST_TRANSPORT_BASE + KB2_TEST_INDIRECT_TABLE_OFFSET,
        .length = 0x1000,
    };
    if (kb2_action_limit(action, KB2_LIMIT_OUTSTANDING_REQUEST_COUNT, &outstanding) !=
            KB2_STATUS_OK ||
        outstanding == 0 || outstanding > 16) {
        return KB2_STATUS_RESOURCE_DENIED;
    }
    if (host->next_channel_id == UINT64_MAX ||
        host->next_notification_id > UINT32_MAX - KB2_TEST_NOTIFICATION_COUNT) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }
    channel.channel_id = ++host->next_channel_id;
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        host->notification_ids[index] = ++host->next_notification_id;
    }

    host->queues[0].queue_id = 1;
    host->queues[0].role = KB2_PROTOCOL_QUEUE_ROLE_EVENT;
    host->queues[0].queue_size = KB2_TEST_QUEUE_SIZE;
    host->queues[0].descriptor_address = KB2_TEST_TRANSPORT_BASE + 0x1000;
    host->queues[0].available_address = KB2_TEST_TRANSPORT_BASE + 0x1100;
    host->queues[0].used_address = KB2_TEST_TRANSPORT_BASE + 0x1140;
    host->queues[0].available_notification_id = host->notification_ids[0];
    host->queues[0].used_notification_id = host->notification_ids[1];
    host->queues[0].max_chain_length = KB2_TEST_QUEUE_SIZE;
    host->queues[0].max_indirect_length = 128;
    host->queues[0].max_outstanding = (uint32_t)outstanding;

    host->queues[1].queue_id = 2;
    host->queues[1].role = KB2_PROTOCOL_QUEUE_ROLE_REQUEST;
    host->queues[1].queue_size = KB2_TEST_QUEUE_SIZE;
    host->queues[1].descriptor_address = KB2_TEST_TRANSPORT_BASE + 0x2000;
    host->queues[1].available_address = KB2_TEST_TRANSPORT_BASE + 0x2100;
    host->queues[1].used_address = KB2_TEST_TRANSPORT_BASE + 0x2140;
    host->queues[1].available_notification_id = host->notification_ids[2];
    host->queues[1].used_notification_id = host->notification_ids[3];
    host->queues[1].max_chain_length = KB2_TEST_QUEUE_SIZE;
    host->queues[1].max_indirect_length = 128;
    host->queues[1].max_outstanding = (uint32_t)outstanding;

    if (kb2_protocol_channel_encode(host->shared_memory,
                                    host->shared_memory_size,
                                    &host->channel_descriptor_size,
                                    &channel,
                                    host->queues,
                                    2,
                                    host->regions,
                                    KB2_TEST_REGION_COUNT) != KB2_PROTOCOL_OK ||
        kb2_test_vq_bind(&host->event_queue,
                         host->shared_memory,
                         host->shared_memory_size,
                         &host->queues[0],
                         host->regions,
                         KB2_TEST_REGION_COUNT,
                         1) != KB2_TEST_VQ_OK ||
        kb2_test_vq_bind(&host->request_queue,
                         host->shared_memory,
                         host->shared_memory_size,
                         &host->queues[1],
                         host->regions,
                         KB2_TEST_REGION_COUNT,
                         1) != KB2_TEST_VQ_OK) {
        return KB2_STATUS_HOST_FAILURE;
    }
    for (event_index = 0; event_index < KB2_TEST_QUEUE_SIZE; ++event_index) {
        uint64_t address = KB2_TEST_TRANSPORT_BASE + KB2_TEST_EVENT_BUFFER_OFFSET +
                           event_index * KB2_TEST_EVENT_BUFFER_STRIDE;

        if (kb2_test_vq_set_descriptor(&host->event_queue,
                                       (uint16_t)event_index,
                                       address,
                                       KB2_TEST_EVENT_BUFFER_STRIDE,
                                       KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE,
                                       0) != KB2_TEST_VQ_OK ||
            kb2_test_vq_publish(&host->event_queue, (uint16_t)event_index, &notify) !=
                KB2_TEST_VQ_OK) {
            return KB2_STATUS_HOST_FAILURE;
        }
    }
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_allocate(kb2_test_host_t *host,
                                           const kb2_action_t *action,
                                           uint64_t *resource_set_id_out) {
    uint8_t configured_digest[KB2_DIGEST_SIZE];
    uint8_t protocol_digest[KB2_DIGEST_SIZE];
    uint64_t channel_count;
    uint64_t queue_count;
    uint64_t shared_memory_size;
    size_t index;
    int failure_error;
    kb2_status_t status;

    if (host->resource_set_id != 0 || host->shared_memory != MAP_FAILED ||
        kb2_action_resource_set_id(action) != 0 || kb2_action_sandbox_id(action) != 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    if (kb2_action_limit(action, KB2_LIMIT_SHARED_MEMORY_BYTES, &shared_memory_size) !=
            KB2_STATUS_OK ||
        kb2_action_limit(action, KB2_LIMIT_CHANNEL_COUNT, &channel_count) != KB2_STATUS_OK ||
        kb2_action_limit(action, KB2_LIMIT_QUEUE_COUNT, &queue_count) != KB2_STATUS_OK ||
        shared_memory_size < KB2_TEST_MINIMUM_SHARED_MEMORY_SIZE ||
        shared_memory_size > (uint64_t)SIZE_MAX || shared_memory_size > (uint64_t)INT64_MAX ||
        channel_count != 1 || queue_count != 2) {
        return KB2_STATUS_RESOURCE_DENIED;
    }
    if (kb2_action_copy_digest(action,
                               KB2_DIGEST_CHANNEL_SET,
                               configured_digest,
                               sizeof(configured_digest)) != KB2_STATUS_OK ||
        kb2_protocol_copy_schema_digest(protocol_digest, sizeof(protocol_digest)) !=
            KB2_PROTOCOL_OK ||
        memcmp(configured_digest, protocol_digest, sizeof(configured_digest)) != 0) {
        return KB2_STATUS_RESOURCE_DENIED;
    }
    if (host->next_resource_set_id == UINT64_MAX) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }

    host->shared_memory_fd = memfd_create("kobox2-test-channel", MFD_CLOEXEC);
    if (host->shared_memory_fd < 0) {
        return errno == EMFILE || errno == ENFILE ? KB2_STATUS_RESOURCE_EXHAUSTED
                                                  : KB2_STATUS_HOST_FAILURE;
    }
    if (ftruncate(host->shared_memory_fd, (off_t)shared_memory_size) != 0) {
        failure_error = errno;
        kb2_test_host_release_local_resources(host);
        return failure_error == ENOSPC ? KB2_STATUS_RESOURCE_EXHAUSTED
                                       : KB2_STATUS_HOST_FAILURE;
    }
    host->shared_memory = mmap(NULL,
                               (size_t)shared_memory_size,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               host->shared_memory_fd,
                               0);
    if (host->shared_memory == MAP_FAILED) {
        failure_error = errno;
        kb2_test_host_release_local_resources(host);
        return failure_error == ENOMEM ? KB2_STATUS_RESOURCE_EXHAUSTED
                                       : KB2_STATUS_HOST_FAILURE;
    }
    host->shared_memory_size = (size_t)shared_memory_size;
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        host->notification_fds[index] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (host->notification_fds[index] < 0) {
            failure_error = errno;
            kb2_test_host_release_local_resources(host);
            return failure_error == EMFILE || failure_error == ENFILE
                       ? KB2_STATUS_RESOURCE_EXHAUSTED
                       : KB2_STATUS_HOST_FAILURE;
        }
    }

    status = kb2_test_host_make_channel(host, action);
    if (status != KB2_STATUS_OK) {
        kb2_test_host_release_local_resources(host);
        return status;
    }
    host->generation = kb2_action_generation(action);
    host->resource_set_id = ++host->next_resource_set_id;
    *resource_set_id_out = host->resource_set_id;
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_launch(kb2_test_host_t *host,
                                         const kb2_action_t *action,
                                         uint64_t *sandbox_id_out) {
    posix_spawn_file_actions_t file_actions;
    char *arguments[2];
    int sockets[2] = {-1, -1};
    int spawn_status;
    int actions_status;

    if (host->next_sandbox_id == UINT64_MAX) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }
    if (host->resource_set_id == 0 || host->process_id > 0 ||
        kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        return errno == EMFILE || errno == ENFILE ? KB2_STATUS_RESOURCE_EXHAUSTED
                                                  : KB2_STATUS_HOST_FAILURE;
    }
    actions_status = posix_spawn_file_actions_init(&file_actions);
    if (actions_status != 0) {
        close(sockets[0]);
        close(sockets[1]);
        return KB2_STATUS_HOST_FAILURE;
    }
    actions_status = posix_spawn_file_actions_addclose(&file_actions, sockets[0]);
    if (actions_status == 0) {
        actions_status =
            posix_spawn_file_actions_adddup2(&file_actions, sockets[1], KB2_TEST_BOOTSTRAP_FD);
    }
    if (actions_status == 0 && sockets[1] != KB2_TEST_BOOTSTRAP_FD) {
        actions_status = posix_spawn_file_actions_addclose(&file_actions, sockets[1]);
    }
    if (actions_status != 0) {
        posix_spawn_file_actions_destroy(&file_actions);
        close(sockets[0]);
        close(sockets[1]);
        return KB2_STATUS_HOST_FAILURE;
    }

    arguments[0] = (char *)host->sandbox_path;
    arguments[1] = NULL;
    spawn_status =
        posix_spawn(&host->process_id, host->sandbox_path, &file_actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&file_actions);
    close(sockets[1]);
    if (spawn_status != 0) {
        close(sockets[0]);
        host->process_id = -1;
        return spawn_status == EAGAIN ? KB2_STATUS_RESOURCE_EXHAUSTED
                                      : KB2_STATUS_HOST_FAILURE;
    }

    host->process_fd = pidfd_open(host->process_id, 0);
    if (host->process_fd < 0) {
        int process_status;

        kill(host->process_id, SIGKILL);
        kb2_test_wait_process(
            host->process_id, &process_status, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS);
        close(sockets[0]);
        host->process_id = -1;
        return KB2_STATUS_HOST_FAILURE;
    }

    host->bootstrap_socket = sockets[0];
    host->sandbox_id = ++host->next_sandbox_id;
    *sandbox_id_out = host->sandbox_id;
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_transfer(kb2_test_host_t *host,
                                           const kb2_action_t *action) {
    kb2_test_bootstrap_t bootstrap = {
        .generation = host->generation,
        .shared_memory_size = host->shared_memory_size,
        .channel_descriptor_size = host->channel_descriptor_size,
    };
    int file_descriptors[KB2_TEST_TRANSFER_FD_COUNT];
    size_t index;

    if (host->bootstrap_socket < 0 || host->process_id <= 0 || host->resources_transferred ||
        kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != host->sandbox_id) {
        return KB2_STATUS_HOST_FAILURE;
    }
    file_descriptors[0] = host->shared_memory_fd;
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        bootstrap.notification_ids[index] = host->notification_ids[index];
        file_descriptors[index + 1] = host->notification_fds[index];
    }
    if (!kb2_test_send_bootstrap(host->bootstrap_socket,
                                 &bootstrap,
                                 file_descriptors,
                                 KB2_TEST_TRANSFER_FD_COUNT)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->resources_transferred = 1;
    if (!kb2_test_host_receive_ready(host)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    kb2_test_close(&host->bootstrap_socket);
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_revoke(kb2_test_host_t *host, const kb2_action_t *action) {
    uint64_t action_sandbox_id = kb2_action_sandbox_id(action);

    if (!host->resources_transferred || host->resources_revoked ||
        kb2_action_resource_set_id(action) != host->resource_set_id ||
        (action_sandbox_id != host->sandbox_id &&
         !(action_sandbox_id == 0 && host->abnormal_exit_allowed)) ||
        host->process_id <= 0 || host->process_fd < 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    if (!host->abnormal_exit_allowed) {
        if (!kb2_test_host_request_quiesce(host)) {
            return KB2_STATUS_HOST_FAILURE;
        }
    } else if (kill(host->process_id, SIGKILL) != 0 && errno != ESRCH) {
        return KB2_STATUS_HOST_FAILURE;
    }
    if (!kb2_test_wait_process_fd(host->process_fd, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->resources_revoked = 1;
    host->reap_after_reset = action_sandbox_id == 0;
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_reset(kb2_test_host_t *host, const kb2_action_t *action) {
    int process_status;
    uint64_t value;
    size_t index;
    ssize_t bytes;

    if (!host->resources_revoked || kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    memset((uint8_t *)host->shared_memory + host->channel_descriptor_size,
           0,
           host->shared_memory_size - host->channel_descriptor_size);
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        do {
            bytes = read(host->notification_fds[index], &value, sizeof(value));
        } while (bytes == (ssize_t)sizeof(value) || (bytes < 0 && errno == EINTR));
        if (bytes < 0 && errno != EAGAIN) {
            return KB2_STATUS_HOST_FAILURE;
        }
    }
    if (host->reap_after_reset) {
        if (!kb2_test_wait_process(
                host->process_id, &process_status, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS)) {
            return KB2_STATUS_HOST_FAILURE;
        }
        host->process_id = -1;
        host->sandbox_id = 0;
        kb2_test_close(&host->process_fd);
    }
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_terminate(kb2_test_host_t *host,
                                            const kb2_action_t *action) {
    int process_status;

    if (!host->resources_revoked || host->process_id <= 0 ||
        kb2_action_resource_set_id(action) != 0 ||
        kb2_action_sandbox_id(action) != host->sandbox_id ||
        !kb2_test_wait_process(host->process_id,
                               &process_status,
                               KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->process_id = -1;
    if (!host->abnormal_exit_allowed &&
        (!WIFEXITED(process_status) || WEXITSTATUS(process_status) != 0)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->sandbox_id = 0;
    kb2_test_close(&host->process_fd);
    kb2_test_close(&host->bootstrap_socket);
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_release(kb2_test_host_t *host,
                                          const kb2_action_t *action) {
    if (host->process_id > 0 || kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    kb2_test_host_release_local_resources(host);
    return KB2_STATUS_OK;
}

kb2_status_t kb2_test_host_execute(kb2_test_host_t *host,
                                   const kb2_action_t *action,
                                   uint64_t *resource_set_id_out,
                                   uint64_t *sandbox_id_out) {
    if (host == NULL || action == NULL || resource_set_id_out == NULL || sandbox_id_out == NULL ||
        kb2_action_generation(action) == 0) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    *resource_set_id_out = 0;
    *sandbox_id_out = 0;
    if (kb2_action_type(action) != KB2_ACTION_ALLOCATE_RESOURCES &&
        kb2_action_generation(action) != host->generation) {
        return KB2_STATUS_HOST_FAILURE;
    }

    switch (kb2_action_type(action)) {
    case KB2_ACTION_ALLOCATE_RESOURCES:
        return kb2_test_host_allocate(host, action, resource_set_id_out);
    case KB2_ACTION_LAUNCH_SANDBOX:
        return kb2_test_host_launch(host, action, sandbox_id_out);
    case KB2_ACTION_TRANSFER_RESOURCES:
        return kb2_test_host_transfer(host, action);
    case KB2_ACTION_REVOKE_RESOURCES:
        return kb2_test_host_revoke(host, action);
    case KB2_ACTION_RESET_RESOURCES:
        return kb2_test_host_reset(host, action);
    case KB2_ACTION_TERMINATE_SANDBOX:
        return kb2_test_host_terminate(host, action);
    case KB2_ACTION_RELEASE_RESOURCES:
        return kb2_test_host_release(host, action);
    case KB2_ACTION_NONE:
    default:
        return KB2_STATUS_INVALID_ARGUMENT;
    }
}

uint64_t kb2_test_host_resource_set_id(const kb2_test_host_t *host) {
    return host == NULL ? 0 : host->resource_set_id;
}

uint64_t kb2_test_host_sandbox_id(const kb2_test_host_t *host) {
    return host == NULL ? 0 : host->sandbox_id;
}

int kb2_test_host_is_released(const kb2_test_host_t *host) {
    return host != NULL && host->resource_set_id == 0 && host->sandbox_id == 0 &&
           host->process_id <= 0 && host->shared_memory == MAP_FAILED;
}
