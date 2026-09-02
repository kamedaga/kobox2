/* SPDX-License-Identifier: Apache-2.0 */

#ifndef KOBOX2_CONTROLLER_H
#define KOBOX2_CONTROLLER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KB2_DIGEST_SIZE 32u

typedef struct kb2_controller kb2_controller_t;
typedef struct kb2_action kb2_action_t;
typedef struct kb2_closure kb2_closure_t;

typedef void *(*kb2_allocate_fn)(void *context, size_t size);
typedef void (*kb2_deallocate_fn)(void *context, void *pointer, size_t size);

typedef enum kb2_status {
    KB2_STATUS_OK = 0,
    KB2_STATUS_INVALID_ARGUMENT,
    KB2_STATUS_INVALID_STATE,
    KB2_STATUS_INVALID_CONFIGURATION,
    KB2_STATUS_ACTION_PENDING,
    KB2_STATUS_STALE_GENERATION,
    KB2_STATUS_STALE_ACTION,
    KB2_STATUS_NO_MEMORY,
    KB2_STATUS_RESOURCE_DENIED,
    KB2_STATUS_RESOURCE_EXHAUSTED,
    KB2_STATUS_HOST_FAILURE,
    KB2_STATUS_COUNTER_EXHAUSTED,
} kb2_status_t;

typedef enum kb2_state {
    KB2_STATE_INVALID = 0,
    KB2_STATE_IDLE,
    KB2_STATE_STARTING,
    KB2_STATE_HANDSHAKING,
    KB2_STATE_RUNNING,
    KB2_STATE_STOPPING,
    KB2_STATE_FAULTED,
} kb2_state_t;

typedef enum kb2_digest_kind {
    KB2_DIGEST_MANIFEST = 0,
    KB2_DIGEST_PROFILE,
    KB2_DIGEST_CAPABILITY_SET,
    KB2_DIGEST_CHANNEL_SET,
} kb2_digest_kind_t;

typedef enum kb2_limit_kind {
    KB2_LIMIT_SHARED_MEMORY_BYTES = 0,
    KB2_LIMIT_CHANNEL_COUNT,
    KB2_LIMIT_QUEUE_COUNT,
    KB2_LIMIT_OUTSTANDING_REQUEST_COUNT,
} kb2_limit_kind_t;

typedef enum kb2_launch_flag {
    KB2_LAUNCH_RESET_REQUIRED = 1u << 0,
} kb2_launch_flag_t;

typedef enum kb2_action_type {
    KB2_ACTION_NONE = 0,
    KB2_ACTION_ALLOCATE_RESOURCES,
    KB2_ACTION_LAUNCH_SANDBOX,
    KB2_ACTION_TRANSFER_RESOURCES,
    KB2_ACTION_QUIESCE_SANDBOX,
    KB2_ACTION_TERMINATE_SANDBOX,
    KB2_ACTION_REVOKE_RESOURCES,
    KB2_ACTION_RESET_RESOURCES,
    KB2_ACTION_REAP_SANDBOX,
    KB2_ACTION_RELEASE_RESOURCES,
} kb2_action_type_t;

typedef enum kb2_fault_kind {
    KB2_FAULT_NONE = 0,
    KB2_FAULT_PROCESS_EXIT,
    KB2_FAULT_PROTOCOL,
    KB2_FAULT_HOST_ACTION,
    KB2_FAULT_CONTROLLER,
} kb2_fault_kind_t;

/*
 * Controller objects are single-owner. The caller serializes all access and
 * must finish host-side cleanup before destroying a non-idle object.
 */
kb2_status_t kb2_controller_create(kb2_allocate_fn allocate,
                                   kb2_deallocate_fn deallocate,
                                   void *allocator_context,
                                   kb2_controller_t **controller_out);
void kb2_controller_destroy(kb2_controller_t *controller);

/* Configuration is copied and may be changed only while the controller is idle.
 * The manifest digest and reset policy come from the immutable closure. */
kb2_status_t kb2_controller_set_closure(kb2_controller_t *controller,
                                        const kb2_closure_t *closure);
kb2_status_t kb2_controller_set_digest(kb2_controller_t *controller,
                                       kb2_digest_kind_t kind,
                                       const uint8_t *digest,
                                       size_t digest_size);
kb2_status_t kb2_controller_set_limit(kb2_controller_t *controller,
                                      kb2_limit_kind_t kind,
                                      uint64_t value);

kb2_state_t kb2_controller_state(const kb2_controller_t *controller);
uint64_t kb2_controller_generation(const kb2_controller_t *controller);
kb2_fault_kind_t kb2_controller_fault_kind(const kb2_controller_t *controller);
uint64_t kb2_controller_fault_code(const kb2_controller_t *controller);

/* The returned action remains valid until the next mutating controller call. */
const kb2_action_t *kb2_controller_pending_action(const kb2_controller_t *controller);
kb2_action_type_t kb2_action_type(const kb2_action_t *action);
uint64_t kb2_action_token(const kb2_action_t *action);
uint64_t kb2_action_generation(const kb2_action_t *action);
uint64_t kb2_action_resource_set_id(const kb2_action_t *action);
uint64_t kb2_action_sandbox_id(const kb2_action_t *action);
uint32_t kb2_action_launch_flags(const kb2_action_t *action);
const kb2_closure_t *kb2_action_closure(const kb2_action_t *action);
kb2_status_t kb2_action_copy_digest(const kb2_action_t *action,
                                    kb2_digest_kind_t kind,
                                    uint8_t *digest_out,
                                    size_t digest_size);
kb2_status_t kb2_action_limit(const kb2_action_t *action,
                              kb2_limit_kind_t kind,
                              uint64_t *value_out);

kb2_status_t kb2_controller_start(kb2_controller_t *controller);
kb2_status_t kb2_controller_stop(kb2_controller_t *controller);
kb2_status_t kb2_controller_restart(kb2_controller_t *controller);
/* Allocation returns a resource ID, launch returns a sandbox ID, and other
 * completions return none. Failed completions return neither ID. */
kb2_status_t kb2_controller_complete_action(kb2_controller_t *controller,
                                            uint64_t generation,
                                            uint64_t token,
                                            kb2_status_t result,
                                            uint64_t resource_set_id,
                                            uint64_t sandbox_id);
kb2_status_t kb2_controller_report_ready(kb2_controller_t *controller, uint64_t generation);
/* Callers report only process-exit and protocol faults; other kinds are controller-owned. */
kb2_status_t kb2_controller_report_fault(kb2_controller_t *controller,
                                         uint64_t generation,
                                         kb2_fault_kind_t kind,
                                         uint64_t code);

#ifdef __cplusplus
}
#endif

#endif
