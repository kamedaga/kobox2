/* SPDX-License-Identifier: MIT */

#ifndef KOBOX2_GPU_H
#define KOBOX2_GPU_H

#include <kobox2/gpu_drm_amdgpu_layout.h>
#include <kobox2/gpu_drm_core_layout.h>
#include <kobox2/gpu_drm_mode_layout.h>
#include <kobox2/gpu_drm_virtgpu_layout.h>
#include <kobox2/gpu_layout.h>
#include <kobox2/protocol.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb2_gpu_command_set {
    uint32_t set_id;
    uint32_t queue_class;
    uint32_t command_count;
    uint8_t schema_digest[KB2_GPU_SCHEMA_DIGEST_SIZE];
} kb2_gpu_command_set_t;

/* Region offsets are relative to the region, unlike transport addresses. */
typedef struct kb2_gpu_region {
    uint32_t region_id;
    uint32_t rights;
    uint64_t length;
} kb2_gpu_region_t;

typedef struct kb2_gpu_argument {
    uint32_t argument_id;
    uint32_t kind;
    uint32_t flags;
    uint32_t record_schema_id;
    uint64_t value;
    uint32_t count;
} kb2_gpu_argument_t;

typedef struct kb2_gpu_span {
    uint32_t span_id;
    uint32_t region_id;
    uint64_t offset;
    uint64_t length;
    uint32_t rights;
    uint32_t record_schema_id;
    uint32_t element_count;
    uint32_t flags;
} kb2_gpu_span_t;

typedef struct kb2_gpu_attachment {
    uint32_t attachment_id;
    uint32_t object_class;
    uint64_t exchange_id;
    uint64_t generation;
    uint64_t rights;
    uint32_t role;
    uint32_t ownership;
    uint32_t flags;
} kb2_gpu_attachment_t;

typedef struct kb2_gpu_command_source {
    uint64_t generation;
    uint64_t session_id;
    uint32_t profile_kind;
    uint32_t queue_class;
    uint32_t command_set_id;
    uint32_t command_id;
    uint32_t flags;
    uint64_t deadline_ns;
    const kb2_gpu_argument_t *arguments;
    size_t argument_count;
    const kb2_gpu_span_t *spans;
    size_t span_count;
    const kb2_gpu_attachment_t *attachments;
    size_t attachment_count;
    const uint8_t *inline_data;
    size_t inline_length;
    const kb2_gpu_region_t *regions;
    size_t region_count;
} kb2_gpu_command_source_t;

typedef struct kb2_gpu_command {
    const uint8_t *bytes;
    size_t size;
    uint64_t generation;
    uint64_t session_id;
    uint32_t command_set_id;
    uint32_t command_id;
    uint32_t flags;
    uint64_t deadline_ns;
    uint32_t counts[3];
    uint32_t offsets[4];
    uint32_t inline_length;
} kb2_gpu_command_t;

/* Inline-only synchronous command completion. This subset cannot represent
 * attachment transfers, asynchronous submission, or command-specific detail.
 * Decode validates framing, not the selected command's result record. The
 * caller must bind the outer envelope's generation/correlation and validate
 * record identity, size, reserved fields and output spans against its request.
 * Inputs must be private immutable snapshots; decoded data borrows those bytes.
 */
typedef struct kb2_gpu_inline_completion {
    uint64_t session_id;
    uint32_t status;
    uint32_t record_schema_id;
    const uint8_t *data;
    size_t length;
} kb2_gpu_inline_completion_t;

kb2_protocol_status_t kb2_gpu_inline_completion_encode(uint8_t *buffer,
    size_t capacity, size_t *size_out, const kb2_gpu_inline_completion_t *completion);
kb2_protocol_status_t kb2_gpu_inline_completion_decode(const uint8_t *buffer,
    size_t size, uint64_t expected_session, kb2_gpu_inline_completion_t *completion_out);

const char *kb2_gpu_schema_sha256_hex(void);
kb2_protocol_status_t kb2_gpu_copy_schema_digest(uint8_t *digest_out,
                                                  size_t digest_size);
size_t kb2_gpu_profile_command_set_count(uint32_t profile_kind);
kb2_protocol_status_t kb2_gpu_profile_command_set(uint32_t profile_kind,
                                                   size_t index,
                                                   kb2_gpu_command_set_t *set_out);
kb2_protocol_status_t kb2_gpu_command_encoded_size(
    const kb2_gpu_command_source_t *source, size_t *size_out);
kb2_protocol_status_t kb2_gpu_command_encode(
    uint8_t *buffer,
    size_t buffer_size,
    size_t *encoded_size_out,
    const kb2_gpu_command_source_t *source);
kb2_protocol_status_t kb2_gpu_command_decode(
    const uint8_t *buffer,
    size_t buffer_size,
    uint64_t expected_generation,
    uint32_t profile_kind,
    uint32_t queue_class,
    const kb2_gpu_region_t *regions,
    size_t region_count,
    kb2_gpu_command_t *command_out);
size_t kb2_gpu_command_argument_count(const kb2_gpu_command_t *command);
size_t kb2_gpu_command_span_count(const kb2_gpu_command_t *command);
size_t kb2_gpu_command_attachment_count(const kb2_gpu_command_t *command);
kb2_protocol_status_t kb2_gpu_command_argument(const kb2_gpu_command_t *command,
                                                size_t index,
                                                kb2_gpu_argument_t *argument_out);
kb2_protocol_status_t kb2_gpu_command_span(const kb2_gpu_command_t *command,
                                            size_t index,
                                            kb2_gpu_span_t *span_out);
kb2_protocol_status_t kb2_gpu_command_attachment(
    const kb2_gpu_command_t *command,
    size_t index,
    kb2_gpu_attachment_t *attachment_out);
const uint8_t *kb2_gpu_command_inline_data(const kb2_gpu_command_t *command,
                                           size_t *length_out);

#ifdef __cplusplus
}
#endif

#endif
