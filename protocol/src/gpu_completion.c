/* SPDX-License-Identifier: MIT */
#include <kobox2/gpu.h>

#include <string.h>

static const uint8_t abi[] = KB2_GPU_ABI_IDENTITY_BYTES;
static const uint8_t schema[] = KB2_GPU_SCHEMA_SHA256_BYTES;

static uint32_t read_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
        (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint64_t read_u64(const uint8_t *bytes) {
    return read_u32(bytes) | (uint64_t)read_u32(bytes + 4) << 32;
}

static void write_u32(uint8_t *bytes, uint32_t value) {
    for (size_t index = 0; index < 4; ++index) bytes[index] = (uint8_t)(value >> (8 * index));
}

static void write_u64(uint8_t *bytes, uint64_t value) {
    write_u32(bytes, (uint32_t)value);
    write_u32(bytes + 4, (uint32_t)(value >> 32));
}

static int valid(const kb2_gpu_inline_completion_t *completion) {
    return completion && completion->session_id && completion->status <= KB2_GPU_STATUS_DEVICE_LOST &&
        completion->length <= KB2_GPU_MAX_INLINE_BYTES &&
        (completion->length ? completion->data && completion->record_schema_id :
            !completion->record_schema_id) &&
        (completion->status == KB2_GPU_STATUS_OK || !completion->length);
}

kb2_protocol_status_t kb2_gpu_inline_completion_encode(uint8_t *buffer,
    size_t capacity, size_t *size_out, const kb2_gpu_inline_completion_t *completion) {
    if (!buffer || !size_out || !completion) return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (!valid(completion)) return KB2_PROTOCOL_MALFORMED;
    size_t offset = KB2_GPU_COMPLETION_HEADER_SIZE;
    if (completion->length) offset += KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    size_t size = offset + completion->length;
    if (capacity < size) return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    memset(buffer, 0, size);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET, (uint32_t)size);
    memcpy(buffer + KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET, abi, sizeof(abi));
    memcpy(buffer + KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET, schema, sizeof(schema));
    write_u64(buffer + KB2_GPU_COMPLETION_HEADER_SESSION_ID_OFFSET, completion->session_id);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_STATUS_OFFSET, completion->status);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_DISPOSITION_OFFSET, KB2_GPU_DISPOSITION_COMPLETED);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_COUNT_OFFSET, completion->length ? 1 : 0);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_LENGTH_OFFSET, (uint32_t)completion->length);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET,
        KB2_GPU_COMPLETION_HEADER_SIZE);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_SPAN_TABLE_OFFSET_OFFSET, (uint32_t)offset);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET, (uint32_t)offset);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET, (uint32_t)offset);
    if (completion->length) {
        uint8_t *argument = buffer + KB2_GPU_COMPLETION_HEADER_SIZE;
        write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET, 1);
        write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET, KB2_GPU_ARGUMENT_INLINE);
        write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET, KB2_GPU_ARGUMENT_FLAG_OUTPUT);
        write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET,
            completion->record_schema_id);
        write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET, (uint32_t)completion->length);
        memcpy(buffer + offset, completion->data, completion->length);
    }
    *size_out = size;
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_gpu_inline_completion_decode(const uint8_t *buffer,
    size_t size, uint64_t expected_session, kb2_gpu_inline_completion_t *completion_out) {
    if (!buffer || !completion_out || !expected_session) return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (size < KB2_GPU_COMPLETION_HEADER_SIZE) return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    if (memcmp(buffer + KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET, abi, sizeof(abi)) ||
        memcmp(buffer + KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET, schema, sizeof(schema)))
        return KB2_PROTOCOL_SCHEMA_MISMATCH;
    kb2_gpu_inline_completion_t completion = {
        .session_id = read_u64(buffer + KB2_GPU_COMPLETION_HEADER_SESSION_ID_OFFSET),
        .status = read_u32(buffer + KB2_GPU_COMPLETION_HEADER_STATUS_OFFSET),
        .length = read_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_LENGTH_OFFSET)};
    size_t offset = KB2_GPU_COMPLETION_HEADER_SIZE;
    if (completion.length) offset += KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    if (completion.length > KB2_GPU_MAX_INLINE_BYTES || completion.session_id != expected_session ||
        size != offset + completion.length ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET) != size ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_DISPOSITION_OFFSET) != KB2_GPU_DISPOSITION_COMPLETED ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_COUNT_OFFSET) != (completion.length ? 1u : 0u) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_SPAN_COUNT_OFFSET) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET) != KB2_GPU_COMPLETION_HEADER_SIZE ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_SPAN_TABLE_OFFSET_OFFSET) != offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET) != offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET) != offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_DETAIL_SET_ID_OFFSET) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_DETAIL_CODE_OFFSET) ||
        read_u64(buffer + KB2_GPU_COMPLETION_HEADER_RESERVED_OFFSET)) return KB2_PROTOCOL_MALFORMED;
    if (completion.length) {
        const uint8_t *argument = buffer + KB2_GPU_COMPLETION_HEADER_SIZE;
        if (read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET) != 1 ||
            read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET) != KB2_GPU_ARGUMENT_INLINE ||
            read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET) != KB2_GPU_ARGUMENT_FLAG_OUTPUT ||
            read_u64(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_VALUE_OFFSET) ||
            read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET) != completion.length ||
            read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_RESERVED_OFFSET)) return KB2_PROTOCOL_MALFORMED;
        completion.record_schema_id = read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET);
        completion.data = buffer + offset;
    }
    if (!valid(&completion)) return KB2_PROTOCOL_MALFORMED;
    *completion_out = completion;
    return KB2_PROTOCOL_OK;
}
