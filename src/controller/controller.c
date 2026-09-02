/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/controller.h>

#include <limits.h>
#include <string.h>

#define KB2_DIGEST_KIND_COUNT 4u
#define KB2_LIMIT_KIND_COUNT 4u
#define KB2_KNOWN_LAUNCH_FLAGS ((uint32_t)KB2_LAUNCH_RESET_REQUIRED)

struct kb2_configuration {
    uint8_t digests[KB2_DIGEST_KIND_COUNT][KB2_DIGEST_SIZE];
    uint64_t limits[KB2_LIMIT_KIND_COUNT];
    uint32_t digest_mask;
    uint32_t flags;
};

struct kb2_action {
    kb2_action_type_t type;
    uint64_t token;
    uint64_t generation;
    uint64_t resource_set_id;
    struct kb2_configuration configuration;
};

struct kb2_controller {
    kb2_deallocate_fn deallocate;
    void *allocator_context;
    struct kb2_configuration configuration;
    struct kb2_action action;
    kb2_state_t state;
    kb2_fault_kind_t fault_kind;
    uint64_t fault_code;
    uint64_t generation;
    uint64_t next_token;
    uint64_t resource_set_id;
    int action_pending;
    int resources_allocated;
    int resources_revoked;
    int resources_reset;
    int process_started;
    int restart_requested;
};

static int kb2_digest_kind_valid(kb2_digest_kind_t kind) {
    return kind >= KB2_DIGEST_MANIFEST && kind <= KB2_DIGEST_CHANNEL_SET;
}

static int kb2_limit_kind_valid(kb2_limit_kind_t kind) {
    return kind >= KB2_LIMIT_SHARED_MEMORY_BYTES &&
           kind <= KB2_LIMIT_OUTSTANDING_REQUEST_COUNT;
}

static int kb2_digest_is_zero(const uint8_t digest[KB2_DIGEST_SIZE]) {
    uint8_t combined = 0;
    size_t index;

    for (index = 0; index < KB2_DIGEST_SIZE; ++index) {
        combined = (uint8_t)(combined | digest[index]);
    }
    return combined == 0;
}

static void kb2_clear_action(kb2_controller_t *controller) {
    memset(&controller->action, 0, sizeof(controller->action));
    controller->action.type = KB2_ACTION_NONE;
    controller->action_pending = 0;
}

static kb2_status_t kb2_emit_action(kb2_controller_t *controller, kb2_action_type_t type) {
    if (controller->next_token == UINT64_MAX) {
        controller->state = KB2_STATE_FAULTED;
        controller->fault_kind = KB2_FAULT_CONTROLLER;
        controller->fault_code = (uint64_t)KB2_STATUS_COUNTER_EXHAUSTED;
        return KB2_STATUS_COUNTER_EXHAUSTED;
    }

    ++controller->next_token;
    controller->action.type = type;
    controller->action.token = controller->next_token;
    controller->action.generation = controller->generation;
    controller->action.resource_set_id = controller->resource_set_id;
    controller->action.configuration = controller->configuration;
    controller->action_pending = 1;
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_validate_configuration(const kb2_controller_t *controller) {
    const uint32_t required_digest_mask = (1u << KB2_DIGEST_KIND_COUNT) - 1u;
    uint64_t channels;
    size_t index;

    if (controller->configuration.digest_mask != required_digest_mask) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    for (index = 0; index < KB2_DIGEST_KIND_COUNT; ++index) {
        if (kb2_digest_is_zero(controller->configuration.digests[index])) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    for (index = 0; index < KB2_LIMIT_KIND_COUNT; ++index) {
        if (controller->configuration.limits[index] == 0) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    if ((controller->configuration.flags & ~KB2_KNOWN_LAUNCH_FLAGS) != 0) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }

    channels = controller->configuration.limits[KB2_LIMIT_CHANNEL_COUNT];
    if (channels > UINT64_MAX / 2u ||
        controller->configuration.limits[KB2_LIMIT_QUEUE_COUNT] < channels * 2u) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    return KB2_STATUS_OK;
}

static int kb2_action_result_valid(kb2_status_t result) {
    return result == KB2_STATUS_OK || result == KB2_STATUS_RESOURCE_DENIED ||
           result == KB2_STATUS_RESOURCE_EXHAUSTED || result == KB2_STATUS_HOST_FAILURE;
}

static kb2_status_t kb2_begin_start(kb2_controller_t *controller) {
    kb2_status_t status;

    if (controller->generation == UINT64_MAX || controller->next_token == UINT64_MAX) {
        return KB2_STATUS_COUNTER_EXHAUSTED;
    }

    status = kb2_validate_configuration(controller);
    if (status != KB2_STATUS_OK) {
        return status;
    }

    ++controller->generation;
    controller->state = KB2_STATE_STARTING;
    controller->fault_kind = KB2_FAULT_NONE;
    controller->fault_code = 0;
    controller->resource_set_id = 0;
    controller->resources_allocated = 0;
    controller->resources_revoked = 0;
    controller->resources_reset = 0;
    controller->process_started = 0;
    return kb2_emit_action(controller, KB2_ACTION_ALLOCATE_RESOURCES);
}

static kb2_status_t kb2_finish_cleanup(kb2_controller_t *controller) {
    int restart = controller->restart_requested;
    kb2_status_t status;

    controller->restart_requested = 0;
    controller->state = KB2_STATE_IDLE;
    controller->fault_kind = KB2_FAULT_NONE;
    controller->fault_code = 0;
    if (restart) {
        status = kb2_begin_start(controller);
        if (status != KB2_STATUS_OK) {
            controller->state = KB2_STATE_FAULTED;
            controller->fault_kind = KB2_FAULT_CONTROLLER;
            controller->fault_code = (uint64_t)status;
        }
        return status;
    }
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_next_cleanup_action(kb2_controller_t *controller) {
    int reset_required = (controller->configuration.flags & KB2_LAUNCH_RESET_REQUIRED) != 0;

    if (controller->resources_allocated && !controller->resources_revoked) {
        return kb2_emit_action(controller, KB2_ACTION_REVOKE_RESOURCES);
    }
    if (controller->resources_allocated && reset_required && !controller->resources_reset) {
        return kb2_emit_action(controller, KB2_ACTION_RESET_RESOURCES);
    }
    if (controller->process_started) {
        return kb2_emit_action(controller, KB2_ACTION_TERMINATE_SANDBOX);
    }
    if (controller->resources_allocated) {
        return kb2_emit_action(controller, KB2_ACTION_RELEASE_RESOURCES);
    }
    return kb2_finish_cleanup(controller);
}

static kb2_status_t kb2_begin_cleanup(kb2_controller_t *controller, int restart) {
    controller->state = KB2_STATE_STOPPING;
    controller->restart_requested = restart;
    return kb2_next_cleanup_action(controller);
}

kb2_status_t kb2_controller_create(kb2_allocate_fn allocate,
                                   kb2_deallocate_fn deallocate,
                                   void *allocator_context,
                                   kb2_controller_t **controller_out) {
    kb2_controller_t *controller;

    if (allocate == NULL || deallocate == NULL || controller_out == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    *controller_out = NULL;
    controller = allocate(allocator_context, sizeof(*controller));
    if (controller == NULL) {
        return KB2_STATUS_NO_MEMORY;
    }

    memset(controller, 0, sizeof(*controller));
    controller->deallocate = deallocate;
    controller->allocator_context = allocator_context;
    controller->state = KB2_STATE_IDLE;
    controller->action.type = KB2_ACTION_NONE;
    *controller_out = controller;
    return KB2_STATUS_OK;
}

void kb2_controller_destroy(kb2_controller_t *controller) {
    kb2_deallocate_fn deallocate;
    void *allocator_context;

    if (controller == NULL) {
        return;
    }
    deallocate = controller->deallocate;
    allocator_context = controller->allocator_context;
    memset(controller, 0, sizeof(*controller));
    deallocate(allocator_context, controller, sizeof(*controller));
}

kb2_status_t kb2_controller_set_digest(kb2_controller_t *controller,
                                       kb2_digest_kind_t kind,
                                       const uint8_t *digest,
                                       size_t digest_size) {
    if (controller == NULL || digest == NULL || !kb2_digest_kind_valid(kind) ||
        digest_size != KB2_DIGEST_SIZE) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (controller->state != KB2_STATE_IDLE || controller->action_pending) {
        return KB2_STATUS_INVALID_STATE;
    }
    if (kb2_digest_is_zero(digest)) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }

    memcpy(controller->configuration.digests[kind], digest, KB2_DIGEST_SIZE);
    controller->configuration.digest_mask |= 1u << (uint32_t)kind;
    return KB2_STATUS_OK;
}

kb2_status_t kb2_controller_set_limit(kb2_controller_t *controller,
                                      kb2_limit_kind_t kind,
                                      uint64_t value) {
    if (controller == NULL || !kb2_limit_kind_valid(kind)) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (controller->state != KB2_STATE_IDLE || controller->action_pending) {
        return KB2_STATUS_INVALID_STATE;
    }
    if (value == 0) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }

    controller->configuration.limits[kind] = value;
    return KB2_STATUS_OK;
}

kb2_status_t kb2_controller_set_launch_flags(kb2_controller_t *controller, uint32_t flags) {
    if (controller == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (controller->state != KB2_STATE_IDLE || controller->action_pending) {
        return KB2_STATUS_INVALID_STATE;
    }
    if ((flags & ~KB2_KNOWN_LAUNCH_FLAGS) != 0) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }

    controller->configuration.flags = flags;
    return KB2_STATUS_OK;
}

kb2_state_t kb2_controller_state(const kb2_controller_t *controller) {
    return controller == NULL ? KB2_STATE_INVALID : controller->state;
}

uint64_t kb2_controller_generation(const kb2_controller_t *controller) {
    return controller == NULL ? 0 : controller->generation;
}

kb2_fault_kind_t kb2_controller_fault_kind(const kb2_controller_t *controller) {
    return controller == NULL ? KB2_FAULT_NONE : controller->fault_kind;
}

uint64_t kb2_controller_fault_code(const kb2_controller_t *controller) {
    return controller == NULL ? 0 : controller->fault_code;
}

const kb2_action_t *kb2_controller_pending_action(const kb2_controller_t *controller) {
    if (controller == NULL || !controller->action_pending) {
        return NULL;
    }
    return &controller->action;
}

kb2_action_type_t kb2_action_type(const kb2_action_t *action) {
    return action == NULL ? KB2_ACTION_NONE : action->type;
}

uint64_t kb2_action_token(const kb2_action_t *action) {
    return action == NULL ? 0 : action->token;
}

uint64_t kb2_action_generation(const kb2_action_t *action) {
    return action == NULL ? 0 : action->generation;
}

uint64_t kb2_action_resource_set_id(const kb2_action_t *action) {
    return action == NULL ? 0 : action->resource_set_id;
}

uint32_t kb2_action_launch_flags(const kb2_action_t *action) {
    return action == NULL ? 0 : action->configuration.flags;
}

kb2_status_t kb2_action_copy_digest(const kb2_action_t *action,
                                    kb2_digest_kind_t kind,
                                    uint8_t *digest_out,
                                    size_t digest_size) {
    if (action == NULL || digest_out == NULL || !kb2_digest_kind_valid(kind) ||
        digest_size != KB2_DIGEST_SIZE) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    memcpy(digest_out, action->configuration.digests[kind], KB2_DIGEST_SIZE);
    return KB2_STATUS_OK;
}

kb2_status_t kb2_action_limit(const kb2_action_t *action,
                              kb2_limit_kind_t kind,
                              uint64_t *value_out) {
    if (action == NULL || value_out == NULL || !kb2_limit_kind_valid(kind)) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    *value_out = action->configuration.limits[kind];
    return KB2_STATUS_OK;
}

kb2_status_t kb2_controller_start(kb2_controller_t *controller) {
    if (controller == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (controller->state != KB2_STATE_IDLE) {
        return KB2_STATUS_INVALID_STATE;
    }
    if (controller->action_pending) {
        return KB2_STATUS_ACTION_PENDING;
    }
    return kb2_begin_start(controller);
}

kb2_status_t kb2_controller_stop(kb2_controller_t *controller) {
    if (controller == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (controller->action_pending) {
        return KB2_STATUS_ACTION_PENDING;
    }
    if (controller->state != KB2_STATE_HANDSHAKING && controller->state != KB2_STATE_RUNNING &&
        controller->state != KB2_STATE_FAULTED) {
        return KB2_STATUS_INVALID_STATE;
    }
    return kb2_begin_cleanup(controller, 0);
}

kb2_status_t kb2_controller_restart(kb2_controller_t *controller) {
    if (controller == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (controller->action_pending) {
        return KB2_STATUS_ACTION_PENDING;
    }
    if (controller->state != KB2_STATE_HANDSHAKING && controller->state != KB2_STATE_RUNNING &&
        controller->state != KB2_STATE_FAULTED) {
        return KB2_STATUS_INVALID_STATE;
    }
    return kb2_begin_cleanup(controller, 1);
}

kb2_status_t kb2_controller_complete_action(kb2_controller_t *controller,
                                            uint64_t generation,
                                            uint64_t token,
                                            kb2_status_t result,
                                            uint64_t resource_set_id) {
    kb2_action_type_t completed;

    if (controller == NULL || !kb2_action_result_valid(result)) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (!controller->action_pending) {
        return KB2_STATUS_INVALID_STATE;
    }
    if (generation != controller->generation) {
        return KB2_STATUS_STALE_GENERATION;
    }
    if (token != controller->action.token) {
        return KB2_STATUS_STALE_ACTION;
    }
    if (controller->action.type == KB2_ACTION_ALLOCATE_RESOURCES) {
        if (result == KB2_STATUS_OK && resource_set_id == 0) {
            return KB2_STATUS_INVALID_ARGUMENT;
        }
    } else if (resource_set_id != 0) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }

    completed = controller->action.type;
    kb2_clear_action(controller);
    if (result != KB2_STATUS_OK) {
        controller->state = KB2_STATE_FAULTED;
        controller->fault_kind = KB2_FAULT_HOST_ACTION;
        controller->fault_code = (uint64_t)completed;
        return result;
    }

    switch (completed) {
    case KB2_ACTION_ALLOCATE_RESOURCES:
        controller->resource_set_id = resource_set_id;
        controller->resources_allocated = 1;
        return kb2_emit_action(controller, KB2_ACTION_LAUNCH_SANDBOX);
    case KB2_ACTION_LAUNCH_SANDBOX:
        controller->process_started = 1;
        return kb2_emit_action(controller, KB2_ACTION_TRANSFER_RESOURCES);
    case KB2_ACTION_TRANSFER_RESOURCES:
        controller->state = KB2_STATE_HANDSHAKING;
        return KB2_STATUS_OK;
    case KB2_ACTION_REVOKE_RESOURCES:
        controller->resources_revoked = 1;
        return kb2_next_cleanup_action(controller);
    case KB2_ACTION_RESET_RESOURCES:
        controller->resources_reset = 1;
        return kb2_next_cleanup_action(controller);
    case KB2_ACTION_TERMINATE_SANDBOX:
        controller->process_started = 0;
        return kb2_next_cleanup_action(controller);
    case KB2_ACTION_RELEASE_RESOURCES:
        controller->resources_allocated = 0;
        controller->resource_set_id = 0;
        return kb2_next_cleanup_action(controller);
    case KB2_ACTION_NONE:
    default:
        controller->state = KB2_STATE_FAULTED;
        controller->fault_kind = KB2_FAULT_HOST_ACTION;
        controller->fault_code = (uint64_t)completed;
        return KB2_STATUS_INVALID_STATE;
    }
}

kb2_status_t kb2_controller_report_ready(kb2_controller_t *controller, uint64_t generation) {
    if (controller == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (generation != controller->generation) {
        return KB2_STATUS_STALE_GENERATION;
    }
    if (controller->action_pending) {
        return KB2_STATUS_ACTION_PENDING;
    }
    if (controller->state != KB2_STATE_HANDSHAKING) {
        return KB2_STATUS_INVALID_STATE;
    }

    controller->state = KB2_STATE_RUNNING;
    return KB2_STATUS_OK;
}

kb2_status_t kb2_controller_report_fault(kb2_controller_t *controller,
                                         uint64_t generation,
                                         kb2_fault_kind_t kind,
                                         uint64_t code) {
    if (controller == NULL ||
        (kind != KB2_FAULT_PROCESS_EXIT && kind != KB2_FAULT_PROTOCOL)) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (generation != controller->generation) {
        return KB2_STATUS_STALE_GENERATION;
    }
    if (controller->action_pending) {
        return KB2_STATUS_ACTION_PENDING;
    }
    if (controller->state != KB2_STATE_HANDSHAKING && controller->state != KB2_STATE_RUNNING) {
        return KB2_STATUS_INVALID_STATE;
    }

    if (kind == KB2_FAULT_PROCESS_EXIT) {
        controller->process_started = 0;
    }
    controller->state = KB2_STATE_FAULTED;
    controller->fault_kind = kind;
    controller->fault_code = code;
    return KB2_STATUS_OK;
}
