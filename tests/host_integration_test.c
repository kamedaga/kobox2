/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/controller.h>
#include <kobox2/protocol.h>

#include "host/linux_host_adapter.h"
#include "test_closure.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define KB2_TEST_BATCH_COUNT 256u

static void *test_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void test_deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static int configure_controller(kb2_controller_t *controller, const kb2_test_host_t *host) {
    uint8_t digest[KB2_DIGEST_SIZE];
    kb2_digest_kind_t kind;

    if (!kb2_test_configure_fixture_closure(controller,
                                            host->manifest_digest,
                                            host->artifact_digests[0],
                                            host->artifact_digests[1],
                                            host->artifact_digests[2])) {
        return 0;
    }
    for (kind = KB2_DIGEST_PROFILE; kind < KB2_DIGEST_CHANNEL_SET; ++kind) {
        memset(digest, (int)kind + 1, sizeof(digest));
        if (kb2_controller_set_digest(controller, kind, digest, sizeof(digest)) !=
            KB2_STATUS_OK) {
            return 0;
        }
    }
    if (kb2_protocol_copy_schema_digest(digest, sizeof(digest)) != KB2_PROTOCOL_OK ||
        kb2_controller_set_digest(
            controller, KB2_DIGEST_CHANNEL_SET, digest, sizeof(digest)) != KB2_STATUS_OK ||
        kb2_controller_set_limit(controller, KB2_LIMIT_SHARED_MEMORY_BYTES, 65536) !=
            KB2_STATUS_OK ||
        kb2_controller_set_limit(controller, KB2_LIMIT_CHANNEL_COUNT, 1) != KB2_STATUS_OK ||
        kb2_controller_set_limit(controller, KB2_LIMIT_QUEUE_COUNT, 2) != KB2_STATUS_OK ||
        kb2_controller_set_limit(controller, KB2_LIMIT_OUTSTANDING_REQUEST_COUNT, 16) !=
            KB2_STATUS_OK) {
        return 0;
    }
    return 1;
}

static int drive_actions(kb2_controller_t *controller, kb2_test_host_t *host) {
    const kb2_action_t *action;

    while ((action = kb2_controller_pending_action(controller)) != NULL) {
        uint64_t generation = kb2_action_generation(action);
        uint64_t token = kb2_action_token(action);
        uint64_t resource_set_id;
        uint64_t sandbox_id;
        kb2_status_t result =
            kb2_test_host_execute(host, action, &resource_set_id, &sandbox_id);
        kb2_status_t completion = kb2_controller_complete_action(controller,
                                                                  generation,
                                                                  token,
                                                                  result,
                                                                  resource_set_id,
                                                                  sandbox_id);

        if (completion != result || result != KB2_STATUS_OK) {
            fprintf(stderr, "host action %u failed: %u/%u\n",
                    (unsigned)kb2_action_type(action),
                    (unsigned)result,
                    (unsigned)completion);
            return 0;
        }
    }
    return 1;
}

static int start_running(kb2_controller_t *controller,
                         kb2_test_host_t *host,
                         uint64_t *generation_out) {
    if (kb2_controller_start(controller) != KB2_STATUS_OK ||
        !drive_actions(controller, host) ||
        kb2_controller_state(controller) != KB2_STATE_HANDSHAKING) {
        return 0;
    }
    *generation_out = kb2_controller_generation(controller);
    return *generation_out != 0 &&
           kb2_controller_report_ready(controller, *generation_out) == KB2_STATUS_OK &&
           kb2_controller_state(controller) == KB2_STATE_RUNNING;
}

static int identifiers_are_fresh(const kb2_test_host_t *host,
                                 uint64_t resource_set_id,
                                 uint64_t sandbox_id,
                                 const uint32_t notification_ids[KB2_TEST_NOTIFICATION_COUNT]) {
    size_t current;
    size_t previous;

    if (kb2_test_host_resource_set_id(host) == resource_set_id ||
        kb2_test_host_sandbox_id(host) == sandbox_id) {
        return 0;
    }
    for (current = 0; current < KB2_TEST_NOTIFICATION_COUNT; ++current) {
        for (previous = 0; previous < KB2_TEST_NOTIFICATION_COUNT; ++previous) {
            if (host->notification_ids[current] == notification_ids[previous]) {
                return 0;
            }
        }
    }
    return 1;
}

static int stop_and_release(kb2_controller_t *controller, kb2_test_host_t *host) {
    return kb2_controller_stop(controller) == KB2_STATUS_OK &&
           drive_actions(controller, host) &&
           kb2_controller_state(controller) == KB2_STATE_IDLE &&
           kb2_test_host_is_released(host);
}

static int run_startup_failure(const char *sandbox_path, const char *core_path,
                              const char *provider_path, const char *consumer_path,
                              int transfer_failure) {
    kb2_controller_t *controller = NULL;
    kb2_test_host_t host;
    const kb2_action_t *action;
    kb2_action_type_t failure = transfer_failure ? KB2_ACTION_TRANSFER_RESOURCES :
                                                  KB2_ACTION_LAUNCH_SANDBOX;
    uint64_t resource_id, sandbox_id, generation, token;
    kb2_status_t result;
    int success = 0;

    if (!kb2_test_host_initialize(&host, sandbox_path, core_path, provider_path, consumer_path)) {
        return 0;
    }
    if (kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) != KB2_STATUS_OK ||
        !configure_controller(controller, &host) ||
        kb2_controller_start(controller) != KB2_STATUS_OK) {
        goto finish;
    }
    for (;;) {
        action = kb2_controller_pending_action(controller);
        if (!action) {
            goto finish;
        }
        generation = kb2_action_generation(action);
        token = kb2_action_token(action);
        if (kb2_action_type(action) == failure) {
            if (transfer_failure) {
                /* Force sendmsg failure before any rights are transferred. */
                close(host.bootstrap_socket);
                host.bootstrap_socket = -1;
            } else {
                /* A real posix_spawn failure, not a synthetic completion. */
                host.sandbox_path = "/dev/null/kobox-no-executable";
            }
            result = kb2_test_host_execute(&host, action, &resource_id, &sandbox_id);
            if (result != KB2_STATUS_HOST_FAILURE || resource_id || sandbox_id ||
                kb2_controller_complete_action(controller, generation, token, result, 0, 0) !=
                    result) {
                goto finish;
            }
            break;
        }
        result = kb2_test_host_execute(&host, action, &resource_id, &sandbox_id);
        if (result != KB2_STATUS_OK ||
            kb2_controller_complete_action(controller, generation, token, result,
                                           resource_id, sandbox_id) != KB2_STATUS_OK) {
            goto finish;
        }
    }
    if (!stop_and_release(controller, &host)) {
        goto finish;
    }
    /* Reuse the same owner/controller after rollback; no stale generation
     * or old process may contaminate the next allocation.
     */
    host.sandbox_path = sandbox_path;
    if (!start_running(controller, &host, &generation) || generation != 2 ||
        !kb2_test_host_echo(&host, UINT64_C(0x61667465722d6661), 0) ||
        !stop_and_release(controller, &host)) {
        goto finish;
    }
    success = 1;
finish:
    if (!success) {
        fprintf(stderr, "startup rollback failed: %s\n", transfer_failure ? "transfer" : "spawn");
    }
    kb2_test_host_destroy(&host);
    kb2_controller_destroy(controller);
    return success;
}

static int run_transport_and_normal_restart(const char *sandbox_path,
                                            const char *core_path,
                                            const char *provider_path,
                                            const char *consumer_path) {
    kb2_controller_t *controller = NULL;
    kb2_test_host_t host;
    uint64_t first_generation;
    uint64_t first_resource_set_id;
    uint64_t first_sandbox_id;
    uint32_t first_notification_ids[KB2_TEST_NOTIFICATION_COUNT];
    struct stat old_memory_status;
    struct stat new_memory_status;
    void *old_memory = MAP_FAILED;
    size_t old_memory_size = 0;
    int old_memory_fd = -1;
    int old_notification_fd = -1;
    size_t batch;
    int success = 0;

    if (!kb2_test_host_initialize(
            &host, sandbox_path, core_path, provider_path, consumer_path)) {
        return 0;
    }
    if (kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) !=
            KB2_STATUS_OK ||
        !configure_controller(controller, &host) ||
        !start_running(controller, &host, &first_generation) ||
        !kb2_test_host_echo(&host, UINT64_C(0x1122334455667788), 0) ||
        !kb2_test_host_echo(&host, UINT64_C(0x8877665544332211), 1)) {
        goto finish;
    }
    for (batch = 0; batch < KB2_TEST_BATCH_COUNT; ++batch) {
        if (!kb2_test_host_echo_batch(
                &host, (uint64_t)batch * KB2_TEST_QUEUE_SIZE, KB2_TEST_QUEUE_SIZE)) {
            goto finish;
        }
    }

    first_resource_set_id = kb2_test_host_resource_set_id(&host);
    first_sandbox_id = kb2_test_host_sandbox_id(&host);
    memcpy(first_notification_ids, host.notification_ids, sizeof(first_notification_ids));
    old_memory_size = host.shared_memory_size;
    old_memory_fd = dup(host.shared_memory_fd);
    old_notification_fd = dup(host.notification_fds[3]);
    if (old_memory_fd < 0 || old_notification_fd < 0 ||
        fstat(old_memory_fd, &old_memory_status) != 0) {
        goto finish;
    }
    old_memory = mmap(NULL, old_memory_size, PROT_READ | PROT_WRITE, MAP_SHARED, old_memory_fd, 0);
    if (old_memory == MAP_FAILED) {
        goto finish;
    }
    if (kb2_controller_restart(controller) != KB2_STATUS_OK ||
        !drive_actions(controller, &host) ||
        kb2_controller_state(controller) != KB2_STATE_HANDSHAKING ||
        kb2_controller_generation(controller) != first_generation + 1u ||
        !identifiers_are_fresh(
            &host, first_resource_set_id, first_sandbox_id, first_notification_ids) ||
        fstat(host.shared_memory_fd, &new_memory_status) != 0 ||
        (new_memory_status.st_dev == old_memory_status.st_dev &&
         new_memory_status.st_ino == old_memory_status.st_ino)) {
        goto finish;
    }
    {
        uint64_t notification = 1;
        uint64_t received;
        ssize_t bytes;

        memset(old_memory, 0xa5, old_memory_size);
        do {
            bytes = write(old_notification_fd, &notification, sizeof(notification));
        } while (bytes < 0 && errno == EINTR);
        if (bytes != (ssize_t)sizeof(notification)) {
            goto finish;
        }
        do {
            bytes = read(host.notification_fds[3], &received, sizeof(received));
        } while (bytes < 0 && errno == EINTR);
        if (bytes >= 0 || errno != EAGAIN) {
            goto finish;
        }
    }
    if (kb2_controller_report_ready(controller, first_generation) !=
            KB2_STATUS_STALE_GENERATION ||
        kb2_controller_report_ready(controller, first_generation + 1u) != KB2_STATUS_OK ||
        !kb2_test_host_echo(&host, UINT64_C(0xa5a55a5adeadbeef), 1) ||
        !stop_and_release(controller, &host)) {
        goto finish;
    }
    success = 1;

finish:
    if (!success) {
        fprintf(stderr, "transport/normal restart scenario failed\n");
    }
    if (old_memory != MAP_FAILED) {
        munmap(old_memory, old_memory_size);
    }
    if (old_memory_fd >= 0) {
        close(old_memory_fd);
    }
    if (old_notification_fd >= 0) {
        close(old_notification_fd);
    }
    kb2_test_host_destroy(&host);
    if (controller != NULL) {
        kb2_controller_destroy(controller);
    }
    return success;
}

static int run_fault_restart(const char *sandbox_path,
                             const char *core_path,
                             const char *provider_path,
                             const char *consumer_path,
                             kb2_test_fault_scenario_t scenario,
                             kb2_fault_kind_t expected_fault_kind) {
    kb2_controller_t *controller = NULL;
    kb2_test_host_t host;
    kb2_fault_kind_t fault_kind;
    uint64_t fault_code;
    uint64_t first_generation;
    uint64_t first_resource_set_id;
    uint64_t first_sandbox_id;
    uint32_t first_notification_ids[KB2_TEST_NOTIFICATION_COUNT];
    int success = 0;

    if (!kb2_test_host_initialize(
            &host, sandbox_path, core_path, provider_path, consumer_path)) {
        return 0;
    }
    if (kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) !=
            KB2_STATUS_OK ||
        !configure_controller(controller, &host) ||
        !start_running(controller, &host, &first_generation)) {
        goto finish;
    }
    first_resource_set_id = kb2_test_host_resource_set_id(&host);
    first_sandbox_id = kb2_test_host_sandbox_id(&host);
    memcpy(first_notification_ids, host.notification_ids, sizeof(first_notification_ids));

    if (!kb2_test_host_inject_fault(&host, scenario, &fault_kind, &fault_code) ||
        fault_kind != expected_fault_kind || fault_code != (uint64_t)scenario ||
        kb2_controller_report_fault(controller, first_generation, fault_kind, fault_code) !=
            KB2_STATUS_OK ||
        kb2_controller_state(controller) != KB2_STATE_FAULTED ||
        kb2_controller_restart(controller) != KB2_STATUS_OK ||
        !drive_actions(controller, &host) ||
        kb2_controller_state(controller) != KB2_STATE_HANDSHAKING ||
        kb2_controller_generation(controller) != first_generation + 1u ||
        !identifiers_are_fresh(
            &host, first_resource_set_id, first_sandbox_id, first_notification_ids) ||
        kb2_controller_report_ready(controller, first_generation) !=
            KB2_STATUS_STALE_GENERATION ||
        kb2_controller_report_ready(controller, first_generation + 1u) != KB2_STATUS_OK ||
        !kb2_test_host_echo(&host, UINT64_C(0xf00d0000) + (uint64_t)scenario, 1) ||
        !stop_and_release(controller, &host)) {
        goto finish;
    }
    success = 1;

finish:
    if (!success) {
        fprintf(stderr, "fault/restart scenario %u failed\n", (unsigned int)scenario);
    }
    kb2_test_host_destroy(&host);
    if (controller != NULL) {
        kb2_controller_destroy(controller);
    }
    return success;
}

int main(int argument_count, char **arguments) {
    static const struct {
        kb2_test_fault_scenario_t scenario;
        kb2_fault_kind_t kind;
    } fault_scenarios[] = {
        {KB2_TEST_FAULT_KILL_BEFORE_ACQUIRE, KB2_FAULT_PROCESS_EXIT},
        {KB2_TEST_FAULT_KILL_AFTER_ACQUIRE, KB2_FAULT_PROCESS_EXIT},
        {KB2_TEST_FAULT_KILL_AFTER_USED, KB2_FAULT_PROCESS_EXIT},
        {KB2_TEST_FAULT_BAD_USED_ID, KB2_FAULT_PROTOCOL},
        {KB2_TEST_FAULT_BAD_GENERATION, KB2_FAULT_PROTOCOL},
        {KB2_TEST_FAULT_BAD_ENVELOPE, KB2_FAULT_PROTOCOL},
        {KB2_TEST_FAULT_BAD_CHAIN, KB2_FAULT_PROTOCOL},
        {KB2_TEST_FAULT_BAD_LENGTH, KB2_FAULT_PROTOCOL},
        {KB2_TEST_FAULT_BAD_RIGHTS, KB2_FAULT_PROTOCOL},
    };
    size_t index;

    if (argument_count != 5) {
        fprintf(stderr,
                "usage: %s SANDBOX_CHILD CORE_SO PROVIDER_KO CONSUMER_KO\n",
                arguments[0]);
        return 2;
    }
    if (!run_transport_and_normal_restart(
            arguments[1], arguments[2], arguments[3], arguments[4])) {
        return 1;
    }
    if (!run_startup_failure(arguments[1], arguments[2], arguments[3], arguments[4], 0) ||
        !run_startup_failure(arguments[1], arguments[2], arguments[3], arguments[4], 1)) {
        return 1;
    }
    for (index = 0; index < sizeof(fault_scenarios) / sizeof(fault_scenarios[0]); ++index) {
        if (!run_fault_restart(arguments[1],
                               arguments[2],
                               arguments[3],
                               arguments[4],
                               fault_scenarios[index].scenario,
                               fault_scenarios[index].kind)) {
            return 1;
        }
    }
    return 0;
}
