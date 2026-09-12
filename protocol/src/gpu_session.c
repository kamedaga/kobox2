/* SPDX-License-Identifier: MIT */
#include <kobox2/gpu_session.h>

#include <string.h>

static const uint8_t abi[] = KB2_GPU_ABI_IDENTITY_BYTES;
static const uint8_t schema[] = KB2_GPU_SCHEMA_SHA256_BYTES;

static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t read_u64(const uint8_t *p) {
    return read_u32(p) | (uint64_t)read_u32(p + 4) << 32;
}

static void write_u32(uint8_t *p, uint32_t value) {
    for (unsigned int i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (i * 8));
}

static void write_u64(uint8_t *p, uint64_t value) {
    write_u32(p, (uint32_t)value);
    write_u32(p + 4, (uint32_t)(value >> 32));
}

static int valid_open(const kb2_gpu_session_open_t *request) {
    return request && request->client_id &&
           (request->node_type == KB2_GPU_NODE_RENDER ||
            request->node_type == KB2_GPU_NODE_PRIMARY);
}

kb2_protocol_status_t
kb2_gpu_session_open_encode(uint8_t *bytes, size_t size, const kb2_gpu_session_open_t *request) {
    if (!bytes || !valid_open(request))
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (size != KB2_GPU_SESSION_OPEN_REQUEST_SIZE)
        return KB2_PROTOCOL_MALFORMED;
    memset(bytes, 0, size);
    write_u32(bytes + KB2_GPU_SESSION_OPEN_REQUEST_NODE_TYPE_OFFSET, request->node_type);
    write_u64(bytes + KB2_GPU_SESSION_OPEN_REQUEST_CLIENT_ID_OFFSET, request->client_id);
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_gpu_session_open_decode(const uint8_t *bytes,
                                                  size_t size,
                                                  kb2_gpu_session_open_t *request_out) {
    if (!bytes || !request_out)
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (size != KB2_GPU_SESSION_OPEN_REQUEST_SIZE)
        return KB2_PROTOCOL_MALFORMED;
    kb2_gpu_session_open_t request = {
        .node_type = read_u32(bytes + KB2_GPU_SESSION_OPEN_REQUEST_NODE_TYPE_OFFSET),
        .client_id = read_u64(bytes + KB2_GPU_SESSION_OPEN_REQUEST_CLIENT_ID_OFFSET)};
    if (!valid_open(&request) || read_u32(bytes + KB2_GPU_SESSION_OPEN_REQUEST_FLAGS_OFFSET) ||
        read_u64(bytes + KB2_GPU_SESSION_OPEN_REQUEST_RESERVED_OFFSET))
        return KB2_PROTOCOL_MALFORMED;
    *request_out = request;
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t
kb2_gpu_session_close_encode(uint8_t *bytes, size_t size, uint64_t session_id) {
    if (!bytes || !session_id)
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (size != KB2_GPU_SESSION_CLOSE_REQUEST_SIZE)
        return KB2_PROTOCOL_MALFORMED;
    memset(bytes, 0, size);
    write_u64(bytes + KB2_GPU_SESSION_CLOSE_REQUEST_SESSION_ID_OFFSET, session_id);
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t
kb2_gpu_session_close_decode(const uint8_t *bytes, size_t size, uint64_t *session_out) {
    if (!bytes || !session_out)
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (size != KB2_GPU_SESSION_CLOSE_REQUEST_SIZE)
        return KB2_PROTOCOL_MALFORMED;
    uint64_t session = read_u64(bytes + KB2_GPU_SESSION_CLOSE_REQUEST_SESSION_ID_OFFSET);
    if (!session || read_u32(bytes + KB2_GPU_SESSION_CLOSE_REQUEST_FLAGS_OFFSET) ||
        read_u32(bytes + KB2_GPU_SESSION_CLOSE_REQUEST_RESERVED_OFFSET))
        return KB2_PROTOCOL_MALFORMED;
    *session_out = session;
    return KB2_PROTOCOL_OK;
}

static int valid_completion(const kb2_gpu_session_completion_t *completion) {
    if (!completion || completion->status > KB2_GPU_STATUS_DEVICE_LOST)
        return 0;
    if (completion->opcode == KB2_GPU_OPCODE_SESSION_OPEN) {
        if (completion->status == KB2_GPU_STATUS_OK)
            return completion->session_id && completion->topology_epoch;
        return !completion->session_id && !completion->topology_epoch && !completion->capabilities;
    }
    return completion->opcode == KB2_GPU_OPCODE_SESSION_CLOSE && completion->session_id &&
           !completion->topology_epoch && !completion->capabilities;
}

kb2_protocol_status_t
kb2_gpu_session_completion_encode(uint8_t *bytes,
                                  size_t capacity,
                                  size_t *size_out,
                                  const kb2_gpu_session_completion_t *completion) {
    if (!bytes || !size_out || !completion)
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (!valid_completion(completion))
        return KB2_PROTOCOL_MALFORMED;
    size_t payload = completion->opcode == KB2_GPU_OPCODE_SESSION_OPEN && !completion->status
                         ? KB2_GPU_SESSION_OPEN_RESPONSE_SIZE
                         : 0;
    size_t size = KB2_GPU_COMPLETION_HEADER_SIZE + payload;
    if (size > capacity)
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    memset(bytes, 0, size);
    write_u32(bytes + KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET, (uint32_t)size);
    memcpy(bytes + KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET, abi, sizeof(abi));
    memcpy(bytes + KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET, schema, sizeof(schema));
    write_u64(bytes + KB2_GPU_COMPLETION_HEADER_SESSION_ID_OFFSET, completion->session_id);
    write_u32(bytes + KB2_GPU_COMPLETION_HEADER_STATUS_OFFSET, completion->status);
    write_u32(bytes + KB2_GPU_COMPLETION_HEADER_DISPOSITION_OFFSET, KB2_GPU_DISPOSITION_COMPLETED);
    write_u32(bytes + KB2_GPU_COMPLETION_HEADER_INLINE_LENGTH_OFFSET, (uint32_t)payload);
    write_u32(bytes + KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET,
              KB2_GPU_COMPLETION_HEADER_SIZE);
    write_u32(bytes + KB2_GPU_COMPLETION_HEADER_SPAN_TABLE_OFFSET_OFFSET,
              KB2_GPU_COMPLETION_HEADER_SIZE);
    write_u32(bytes + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET,
              KB2_GPU_COMPLETION_HEADER_SIZE);
    write_u32(bytes + KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET,
              KB2_GPU_COMPLETION_HEADER_SIZE);
    if (payload) {
        uint8_t *result = bytes + KB2_GPU_COMPLETION_HEADER_SIZE;
        write_u64(result + KB2_GPU_SESSION_OPEN_RESPONSE_SESSION_ID_OFFSET, completion->session_id);
        write_u64(result + KB2_GPU_SESSION_OPEN_RESPONSE_TOPOLOGY_EPOCH_OFFSET,
                  completion->topology_epoch);
        write_u64(result + KB2_GPU_SESSION_OPEN_RESPONSE_CAPABILITIES_OFFSET,
                  completion->capabilities);
    }
    *size_out = size;
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t
kb2_gpu_session_completion_decode(const uint8_t *bytes,
                                  size_t size,
                                  uint32_t opcode,
                                  kb2_gpu_session_completion_t *completion_out) {
    if (!bytes || !completion_out)
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (size < KB2_GPU_COMPLETION_HEADER_SIZE)
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    if (memcmp(bytes + KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET, abi, sizeof(abi)) ||
        memcmp(bytes + KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET, schema, sizeof(schema)))
        return KB2_PROTOCOL_SCHEMA_MISMATCH;
    kb2_gpu_session_completion_t completion = {
        .opcode = opcode,
        .session_id = read_u64(bytes + KB2_GPU_COMPLETION_HEADER_SESSION_ID_OFFSET),
        .status = read_u32(bytes + KB2_GPU_COMPLETION_HEADER_STATUS_OFFSET)};
    size_t payload = opcode == KB2_GPU_OPCODE_SESSION_OPEN && !completion.status
                         ? KB2_GPU_SESSION_OPEN_RESPONSE_SIZE
                         : 0;
    if (size != KB2_GPU_COMPLETION_HEADER_SIZE + payload ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET) != size ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_DISPOSITION_OFFSET) !=
            KB2_GPU_DISPOSITION_COMPLETED ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_ARGUMENT_COUNT_OFFSET) ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_SPAN_COUNT_OFFSET) ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET) ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_INLINE_LENGTH_OFFSET) != payload ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET) !=
            KB2_GPU_COMPLETION_HEADER_SIZE ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_SPAN_TABLE_OFFSET_OFFSET) !=
            KB2_GPU_COMPLETION_HEADER_SIZE ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET) !=
            KB2_GPU_COMPLETION_HEADER_SIZE ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET) !=
            KB2_GPU_COMPLETION_HEADER_SIZE ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_DETAIL_SET_ID_OFFSET) ||
        read_u32(bytes + KB2_GPU_COMPLETION_HEADER_DETAIL_CODE_OFFSET) ||
        read_u64(bytes + KB2_GPU_COMPLETION_HEADER_RESERVED_OFFSET))
        return KB2_PROTOCOL_MALFORMED;
    if (payload) {
        const uint8_t *result = bytes + KB2_GPU_COMPLETION_HEADER_SIZE;
        completion.topology_epoch =
            read_u64(result + KB2_GPU_SESSION_OPEN_RESPONSE_TOPOLOGY_EPOCH_OFFSET);
        completion.capabilities =
            read_u64(result + KB2_GPU_SESSION_OPEN_RESPONSE_CAPABILITIES_OFFSET);
        if (read_u64(result + KB2_GPU_SESSION_OPEN_RESPONSE_SESSION_ID_OFFSET) !=
                completion.session_id ||
            read_u64(result + KB2_GPU_SESSION_OPEN_RESPONSE_RESERVED_OFFSET))
            return KB2_PROTOCOL_MALFORMED;
    }
    if (!valid_completion(&completion))
        return KB2_PROTOCOL_MALFORMED;
    *completion_out = completion;
    return KB2_PROTOCOL_OK;
}
