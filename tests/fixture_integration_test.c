/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/controller.h>
#include <kobox2/protocol.h>

#include "host/linux_host_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *test_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void test_deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static int configure(kb2_controller_t *controller) {
    uint8_t digest[KB2_DIGEST_SIZE];
    kb2_digest_kind_t kind;

    for (kind = KB2_DIGEST_MANIFEST; kind < KB2_DIGEST_CHANNEL_SET; ++kind) {
        memset(digest, (int)kind + 1, sizeof(digest));
        if (kb2_controller_set_digest(controller, kind, digest, sizeof(digest)) !=
            KB2_STATUS_OK) {
            return 0;
        }
    }
    return kb2_protocol_copy_schema_digest(digest, sizeof(digest)) == KB2_PROTOCOL_OK &&
           kb2_controller_set_digest(
               controller, KB2_DIGEST_CHANNEL_SET, digest, sizeof(digest)) == KB2_STATUS_OK &&
           kb2_controller_set_limit(controller, KB2_LIMIT_SHARED_MEMORY_BYTES, 65536) ==
               KB2_STATUS_OK &&
           kb2_controller_set_limit(controller, KB2_LIMIT_CHANNEL_COUNT, 1) ==
               KB2_STATUS_OK &&
           kb2_controller_set_limit(controller, KB2_LIMIT_QUEUE_COUNT, 2) ==
               KB2_STATUS_OK &&
           kb2_controller_set_limit(controller, KB2_LIMIT_OUTSTANDING_REQUEST_COUNT, 16) ==
               KB2_STATUS_OK &&
           kb2_controller_set_launch_flags(controller, KB2_LAUNCH_RESET_REQUIRED) ==
               KB2_STATUS_OK;
}

static int drive(kb2_controller_t *controller, kb2_test_host_t *host) {
    const kb2_action_t *action;

    while ((action = kb2_controller_pending_action(controller)) != NULL) {
        uint64_t resource_set_id;
        uint64_t sandbox_id;
        uint64_t generation = kb2_action_generation(action);
        uint64_t token = kb2_action_token(action);
        kb2_status_t result =
            kb2_test_host_execute(host, action, &resource_set_id, &sandbox_id);

        if (result != KB2_STATUS_OK ||
            kb2_controller_complete_action(controller,
                                           generation,
                                           token,
                                           result,
                                           resource_set_id,
                                           sandbox_id) != KB2_STATUS_OK) {
            return 0;
        }
    }
    return 1;
}

int main(int argument_count, char **arguments) {
    kb2_controller_t *controller = NULL;
    kb2_test_host_t host;
    uint64_t generation;
    int host_initialized = 0;
    int result = 1;

    if (argument_count != 2) {
        fprintf(stderr, "usage: %s FIXTURE_SANDBOX\n", arguments[0]);
        return 2;
    }
    if (!kb2_test_host_initialize(&host, arguments[1])) {
        return 1;
    }
    host_initialized = 1;
    if (kb2_controller_create(test_allocate, test_deallocate, NULL, &controller) !=
            KB2_STATUS_OK ||
        !configure(controller) || kb2_controller_start(controller) != KB2_STATUS_OK ||
        !drive(controller, &host) ||
        kb2_controller_state(controller) != KB2_STATE_HANDSHAKING) {
        goto finish;
    }
    generation = kb2_controller_generation(controller);
    if (kb2_controller_report_ready(controller, generation) != KB2_STATUS_OK ||
        !kb2_test_host_run_fixture(&host) ||
        !kb2_test_host_run_fixture(&host) ||
        kb2_controller_restart(controller) != KB2_STATUS_OK || !drive(controller, &host) ||
        kb2_controller_generation(controller) != generation + 1u ||
        kb2_controller_report_ready(controller, generation) != KB2_STATUS_STALE_GENERATION ||
        kb2_controller_report_ready(controller, generation + 1u) != KB2_STATUS_OK ||
        !kb2_test_host_run_fixture(&host) ||
        kb2_controller_stop(controller) != KB2_STATUS_OK || !drive(controller, &host) ||
        kb2_controller_state(controller) != KB2_STATE_IDLE ||
        !kb2_test_host_is_released(&host)) {
        goto finish;
    }
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
