/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_GPU_SESSION_H
#define KOBOX2_GPU_SESSION_H

#include <kobox2/gpu.h>

typedef struct kb2_gpu_session_open {
    uint32_t node_type;
    uint64_t client_id;
} kb2_gpu_session_open_t;

typedef struct kb2_gpu_session_completion {
    uint32_t opcode;
    uint32_t status;
    uint64_t session_id;
    uint64_t topology_epoch;
    uint64_t capabilities;
} kb2_gpu_session_completion_t;

kb2_protocol_status_t
kb2_gpu_session_open_encode(uint8_t *bytes, size_t size, const kb2_gpu_session_open_t *request);
kb2_protocol_status_t
kb2_gpu_session_open_decode(const uint8_t *bytes, size_t size, kb2_gpu_session_open_t *request_out);
kb2_protocol_status_t
kb2_gpu_session_close_encode(uint8_t *bytes, size_t size, uint64_t session_id);
kb2_protocol_status_t
kb2_gpu_session_close_decode(const uint8_t *bytes, size_t size, uint64_t *session_out);

/* Base session results have no command argument/span/attachment descriptors.
 * SESSION_OPEN success carries session_open_response in the inline area;
 * errors and SESSION_CLOSE have no inline result. OPEN failure has session 0.
 * The caller independently validates envelope opcode/generation/correlation
 * and CLOSE's requested ID. Decode consumes a private immutable snapshot.
 */
kb2_protocol_status_t
kb2_gpu_session_completion_encode(uint8_t *bytes,
                                  size_t capacity,
                                  size_t *size_out,
                                  const kb2_gpu_session_completion_t *completion);
kb2_protocol_status_t
kb2_gpu_session_completion_decode(const uint8_t *bytes,
                                  size_t size,
                                  uint32_t opcode,
                                  kb2_gpu_session_completion_t *completion_out);

#endif
