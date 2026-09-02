/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/controller.h>
#include <kobox2/protocol.h>

#include "host/linux_host_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);      \
            goto finish;                                                                           \
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

    for (kind = KB2_DIGEST_MANIFEST; kind < KB2_DIGEST_CHANNEL_SET; ++kind) {
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
            KB2_STATUS_OK ||
        kb2_controller_set_launch_flags(controller, KB2_LAUNCH_RESET_REQUIRED) !=
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
            return 0;
        }
    }
    return 1;
}

int main(int argument_count, char **arguments) {
    kb2_controller_t *controller = NULL;
    kb2_test_host_t host;
    uint64_t first_generation;
    uint64_t first_resource_set_id;
    uint64_t first_sandbox_id;
    uint32_t first_notification_ids[KB2_TEST_NOTIFICATION_COUNT];
    size_t current;
    size_t previous;
    int host_initialized = 0;
    int result = 1;

    if (argument_count != 2) {
        fprintf(stderr, "usage: %s SANDBOX_CHILD\n", arguments[0]);
        return 2;
    }
    CHECK(kb2_test_host_initialize(&host, arguments[1]));
    host_initialized = 1;
    CHECK(kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) ==
          KB2_STATUS_OK);
    CHECK(configure_controller(controller));

    CHECK(kb2_controller_start(controller) == KB2_STATUS_OK);
    CHECK(drive_actions(controller, &host));
    CHECK(kb2_controller_state(controller) == KB2_STATE_HANDSHAKING);
    first_generation = kb2_controller_generation(controller);
    first_resource_set_id = kb2_test_host_resource_set_id(&host);
    first_sandbox_id = kb2_test_host_sandbox_id(&host);
    CHECK(first_generation != 0 && first_resource_set_id != 0 && first_sandbox_id != 0);
    CHECK(host.process_id > 0);
    memcpy(first_notification_ids, host.notification_ids, sizeof(first_notification_ids));
    CHECK(kb2_controller_report_ready(controller, first_generation) == KB2_STATUS_OK);
    CHECK(kb2_controller_state(controller) == KB2_STATE_RUNNING);

    CHECK(kb2_controller_restart(controller) == KB2_STATUS_OK);
    CHECK(drive_actions(controller, &host));
    CHECK(kb2_controller_state(controller) == KB2_STATE_HANDSHAKING);
    CHECK(kb2_controller_generation(controller) == first_generation + 1);
    CHECK(kb2_test_host_resource_set_id(&host) != first_resource_set_id);
    CHECK(kb2_test_host_sandbox_id(&host) != first_sandbox_id);
    for (current = 0; current < KB2_TEST_NOTIFICATION_COUNT; ++current) {
        for (previous = 0; previous < KB2_TEST_NOTIFICATION_COUNT; ++previous) {
            CHECK(host.notification_ids[current] != first_notification_ids[previous]);
        }
    }
    CHECK(kb2_controller_report_ready(controller, first_generation) ==
          KB2_STATUS_STALE_GENERATION);
    CHECK(kb2_controller_report_ready(controller, first_generation + 1) == KB2_STATUS_OK);

    CHECK(kb2_controller_stop(controller) == KB2_STATUS_OK);
    CHECK(drive_actions(controller, &host));
    CHECK(kb2_controller_state(controller) == KB2_STATE_IDLE);
    CHECK(kb2_test_host_is_released(&host));
    result = 0;

finish:
    if (host_initialized) {
        kb2_test_host_destroy(&host);
    }
    if (controller != NULL) {
        kb2_controller_destroy(controller);
    }
    return result;
}
