/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/controller.h>
#include <kobox2/closure.h>

#include "test_closure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static void *test_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void test_deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static int configure_controller(kb2_controller_t *controller) {
    uint8_t digest[KB2_DIGEST_SIZE];
    kb2_digest_kind_t kind;

    CHECK(kb2_test_configure_closure(controller));
    for (kind = KB2_DIGEST_PROFILE; kind <= KB2_DIGEST_CHANNEL_SET; ++kind) {
        memset(digest, (int)kind + 1, sizeof(digest));
        CHECK(kb2_controller_set_digest(controller, kind, digest, sizeof(digest)) ==
              KB2_STATUS_OK);
    }
    CHECK(kb2_controller_set_limit(controller, KB2_LIMIT_SHARED_MEMORY_BYTES, 1u << 20) ==
          KB2_STATUS_OK);
    CHECK(kb2_controller_set_limit(controller, KB2_LIMIT_CHANNEL_COUNT, 2) == KB2_STATUS_OK);
    CHECK(kb2_controller_set_limit(controller, KB2_LIMIT_QUEUE_COUNT, 4) == KB2_STATUS_OK);
    CHECK(kb2_controller_set_limit(controller, KB2_LIMIT_OUTSTANDING_REQUEST_COUNT, 64) ==
          KB2_STATUS_OK);
    return 0;
}

static int complete_pending(kb2_controller_t *controller,
                            kb2_action_type_t expected,
                            uint64_t resource_set_id,
                            uint64_t sandbox_id) {
    const kb2_action_t *action = kb2_controller_pending_action(controller);

    CHECK(action != NULL);
    CHECK(kb2_action_type(action) == expected);
    CHECK(kb2_action_generation(action) == kb2_controller_generation(controller));
    CHECK(kb2_controller_complete_action(controller,
                                         kb2_action_generation(action),
                                         kb2_action_token(action),
                                         KB2_STATUS_OK,
                                         resource_set_id,
                                         sandbox_id) == KB2_STATUS_OK);
    return 0;
}

static int test_configuration_rejection(void) {
    kb2_controller_t *controller = NULL;
    uint8_t digest[KB2_DIGEST_SIZE];

    CHECK(kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) ==
          KB2_STATUS_OK);
    memset(digest, 1, sizeof(digest));
    CHECK(kb2_controller_set_digest(controller, KB2_DIGEST_MANIFEST, digest, sizeof(digest)) ==
          KB2_STATUS_INVALID_ARGUMENT);
    CHECK(kb2_controller_start(controller) == KB2_STATUS_INVALID_CONFIGURATION);
    CHECK(kb2_controller_state(controller) == KB2_STATE_IDLE);
    CHECK(kb2_controller_generation(controller) == 0);
    CHECK(kb2_controller_pending_action(controller) == NULL);
    kb2_controller_destroy(controller);
    return 0;
}

static int test_start_and_restart(void) {
    const kb2_action_t *action;
    uint8_t digest[KB2_DIGEST_SIZE];
    uint64_t first_generation;
    uint64_t limit;
    uint64_t token;
    kb2_controller_t *controller = NULL;

    CHECK(kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) ==
          KB2_STATUS_OK);
    CHECK(configure_controller(controller) == 0);
    CHECK(kb2_controller_start(controller) == KB2_STATUS_OK);

    action = kb2_controller_pending_action(controller);
    CHECK(action != NULL);
    CHECK(kb2_action_type(action) == KB2_ACTION_ALLOCATE_RESOURCES);
    CHECK(kb2_action_resource_set_id(action) == 0);
    CHECK(kb2_action_sandbox_id(action) == 0);
    CHECK(kb2_action_launch_flags(action) == KB2_LAUNCH_RESET_REQUIRED);
    CHECK(kb2_action_closure(action) != NULL);
    CHECK(kb2_closure_artifact_count(kb2_action_closure(action)) == 1);
    CHECK(kb2_action_copy_digest(action, KB2_DIGEST_MANIFEST, digest, sizeof(digest)) ==
          KB2_STATUS_OK);
    CHECK(digest[0] == 0x41 && digest[KB2_DIGEST_SIZE - 1] == 0x41);
    CHECK(kb2_action_limit(action, KB2_LIMIT_QUEUE_COUNT, &limit) == KB2_STATUS_OK);
    CHECK(limit == 4);
    CHECK(kb2_action_copy_digest(action, KB2_DIGEST_PROFILE, digest, sizeof(digest)) ==
          KB2_STATUS_OK);
    CHECK(digest[0] == 2 && digest[KB2_DIGEST_SIZE - 1] == 2);
    first_generation = kb2_action_generation(action);
    token = kb2_action_token(action);
    CHECK(first_generation == 1);
    CHECK(kb2_controller_complete_action(controller,
                                         first_generation + 1,
                                         token,
                                         KB2_STATUS_OK,
                                         7,
                                         0) == KB2_STATUS_STALE_GENERATION);
    CHECK(kb2_controller_complete_action(controller,
                                         first_generation,
                                         token + 1,
                                         KB2_STATUS_OK,
                                         7,
                                         0) == KB2_STATUS_STALE_ACTION);
    CHECK(kb2_action_token(kb2_controller_pending_action(controller)) == token);

    CHECK(complete_pending(controller, KB2_ACTION_ALLOCATE_RESOURCES, 7, 0) == 0);
    CHECK(kb2_action_resource_set_id(kb2_controller_pending_action(controller)) == 7);
    CHECK(kb2_action_sandbox_id(kb2_controller_pending_action(controller)) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_LAUNCH_SANDBOX, 0, 11) == 0);
    CHECK(kb2_action_resource_set_id(kb2_controller_pending_action(controller)) == 7);
    CHECK(kb2_action_sandbox_id(kb2_controller_pending_action(controller)) == 11);
    CHECK(complete_pending(controller, KB2_ACTION_TRANSFER_RESOURCES, 0, 0) == 0);
    CHECK(kb2_controller_report_ready(controller, first_generation) == KB2_STATUS_OK);

    CHECK(kb2_controller_restart(controller) == KB2_STATUS_OK);
    CHECK(kb2_action_resource_set_id(kb2_controller_pending_action(controller)) == 7);
    CHECK(kb2_action_sandbox_id(kb2_controller_pending_action(controller)) == 11);
    CHECK(complete_pending(controller, KB2_ACTION_QUIESCE_SANDBOX, 0, 0) == 0);
    CHECK(kb2_action_resource_set_id(kb2_controller_pending_action(controller)) == 7);
    CHECK(kb2_action_sandbox_id(kb2_controller_pending_action(controller)) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_REVOKE_RESOURCES, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_RESET_RESOURCES, 0, 0) == 0);
    CHECK(kb2_action_resource_set_id(kb2_controller_pending_action(controller)) == 0);
    CHECK(kb2_action_sandbox_id(kb2_controller_pending_action(controller)) == 11);
    CHECK(complete_pending(controller, KB2_ACTION_REAP_SANDBOX, 0, 0) == 0);
    CHECK(kb2_action_resource_set_id(kb2_controller_pending_action(controller)) == 7);
    CHECK(kb2_action_sandbox_id(kb2_controller_pending_action(controller)) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_RELEASE_RESOURCES, 0, 0) == 0);

    CHECK(kb2_controller_generation(controller) == first_generation + 1);
    CHECK(kb2_action_type(kb2_controller_pending_action(controller)) ==
          KB2_ACTION_ALLOCATE_RESOURCES);
    CHECK(kb2_controller_report_ready(controller, first_generation) ==
          KB2_STATUS_STALE_GENERATION);

    CHECK(complete_pending(controller, KB2_ACTION_ALLOCATE_RESOURCES, 8, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_LAUNCH_SANDBOX, 0, 12) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_TRANSFER_RESOURCES, 0, 0) == 0);
    CHECK(kb2_controller_report_ready(controller, first_generation + 1) == KB2_STATUS_OK);
    CHECK(kb2_controller_state(controller) == KB2_STATE_RUNNING);

    CHECK(kb2_controller_report_fault(controller,
                                      first_generation + 1,
                                      KB2_FAULT_PROCESS_EXIT,
                                      23) == KB2_STATUS_OK);
    CHECK(kb2_controller_state(controller) == KB2_STATE_FAULTED);
    CHECK(kb2_controller_fault_kind(controller) == KB2_FAULT_PROCESS_EXIT);
    CHECK(kb2_controller_fault_code(controller) == 23);

    CHECK(kb2_controller_stop(controller) == KB2_STATUS_OK);
    CHECK(kb2_action_resource_set_id(kb2_controller_pending_action(controller)) == 8);
    CHECK(kb2_action_sandbox_id(kb2_controller_pending_action(controller)) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_REVOKE_RESOURCES, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_RESET_RESOURCES, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_REAP_SANDBOX, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_RELEASE_RESOURCES, 0, 0) == 0);
    CHECK(kb2_controller_state(controller) == KB2_STATE_IDLE);
    CHECK(kb2_controller_pending_action(controller) == NULL);

    kb2_controller_destroy(controller);
    return 0;
}

static int test_host_action_failure(void) {
    const kb2_action_t *action;
    kb2_controller_t *controller = NULL;

    CHECK(kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) ==
          KB2_STATUS_OK);
    CHECK(configure_controller(controller) == 0);
    CHECK(kb2_controller_start(controller) == KB2_STATUS_OK);
    action = kb2_controller_pending_action(controller);
    CHECK(action != NULL);
    CHECK(kb2_controller_complete_action(controller,
                                         kb2_action_generation(action),
                                         kb2_action_token(action),
                                         KB2_STATUS_RESOURCE_DENIED,
                                         0,
                                         0) == KB2_STATUS_RESOURCE_DENIED);
    CHECK(kb2_controller_state(controller) == KB2_STATE_FAULTED);
    CHECK(kb2_controller_fault_kind(controller) == KB2_FAULT_HOST_ACTION);
    CHECK(kb2_controller_pending_action(controller) == NULL);
    CHECK(kb2_controller_stop(controller) == KB2_STATUS_OK);
    CHECK(kb2_controller_state(controller) == KB2_STATE_IDLE);
    kb2_controller_destroy(controller);
    return 0;
}

static int test_action_completion_validation(void) {
    const kb2_action_t *action;
    kb2_controller_t *controller = NULL;

    CHECK(kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) ==
          KB2_STATUS_OK);
    CHECK(configure_controller(controller) == 0);
    CHECK(kb2_controller_start(controller) == KB2_STATUS_OK);
    action = kb2_controller_pending_action(controller);
    CHECK(action != NULL);
    CHECK(kb2_controller_complete_action(controller,
                                         kb2_action_generation(action),
                                         kb2_action_token(action),
                                         KB2_STATUS_OK,
                                         0,
                                         0) == KB2_STATUS_INVALID_ARGUMENT);
    CHECK(kb2_controller_complete_action(controller,
                                         kb2_action_generation(action),
                                         kb2_action_token(action),
                                         KB2_STATUS_HOST_FAILURE,
                                         1,
                                         0) == KB2_STATUS_INVALID_ARGUMENT);
    CHECK(complete_pending(controller, KB2_ACTION_ALLOCATE_RESOURCES, 7, 0) == 0);
    action = kb2_controller_pending_action(controller);
    CHECK(kb2_controller_complete_action(controller,
                                         kb2_action_generation(action),
                                         kb2_action_token(action),
                                         KB2_STATUS_OK,
                                         7,
                                         11) == KB2_STATUS_INVALID_ARGUMENT);
    CHECK(complete_pending(controller, KB2_ACTION_LAUNCH_SANDBOX, 0, 11) == 0);
    action = kb2_controller_pending_action(controller);
    CHECK(kb2_controller_complete_action(controller,
                                         kb2_action_generation(action),
                                         kb2_action_token(action),
                                         KB2_STATUS_OK,
                                         7,
                                         0) == KB2_STATUS_INVALID_ARGUMENT);
    CHECK(kb2_controller_complete_action(controller,
                                         kb2_action_generation(action),
                                         kb2_action_token(action),
                                         KB2_STATUS_HOST_FAILURE,
                                         0,
                                         0) == KB2_STATUS_HOST_FAILURE);
    CHECK(kb2_controller_stop(controller) == KB2_STATUS_OK);
    CHECK(complete_pending(controller, KB2_ACTION_TERMINATE_SANDBOX, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_REVOKE_RESOURCES, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_RESET_RESOURCES, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_REAP_SANDBOX, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_RELEASE_RESOURCES, 0, 0) == 0);

    kb2_controller_destroy(controller);
    return 0;
}

static int test_stop_without_resource_reset(void) {
    uint64_t generation;
    kb2_controller_t *controller = NULL;

    CHECK(kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) ==
          KB2_STATUS_OK);
    CHECK(configure_controller(controller) == 0);
    CHECK(kb2_test_configure_closure_with_reset(controller, 0));
    CHECK(kb2_controller_start(controller) == KB2_STATUS_OK);
    CHECK(kb2_action_launch_flags(kb2_controller_pending_action(controller)) == 0);
    generation = kb2_controller_generation(controller);
    CHECK(complete_pending(controller, KB2_ACTION_ALLOCATE_RESOURCES, 9, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_LAUNCH_SANDBOX, 0, 13) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_TRANSFER_RESOURCES, 0, 0) == 0);
    CHECK(kb2_controller_report_ready(controller, generation) == KB2_STATUS_OK);

    CHECK(kb2_controller_stop(controller) == KB2_STATUS_OK);
    CHECK(complete_pending(controller, KB2_ACTION_QUIESCE_SANDBOX, 0, 0) == 0);
    CHECK(kb2_action_type(kb2_controller_pending_action(controller)) ==
          KB2_ACTION_REVOKE_RESOURCES);
    CHECK(complete_pending(controller, KB2_ACTION_REVOKE_RESOURCES, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_REAP_SANDBOX, 0, 0) == 0);
    CHECK(complete_pending(controller, KB2_ACTION_RELEASE_RESOURCES, 0, 0) == 0);
    CHECK(kb2_controller_state(controller) == KB2_STATE_IDLE);

    kb2_controller_destroy(controller);
    return 0;
}

int main(void) {
    CHECK(test_configuration_rejection() == 0);
    CHECK(test_start_and_restart() == 0);
    CHECK(test_host_action_failure() == 0);
    CHECK(test_action_completion_validation() == 0);
    CHECK(test_stop_without_resource_reset() == 0);
    return 0;
}
