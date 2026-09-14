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

static int map_valid(const kb2_gpu_virtgpu_map_completion_t *completion) {
    const uint64_t known_rights =
        KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE;

    if (!completion || !completion->session_id ||
        completion->status > KB2_GPU_STATUS_DEVICE_LOST)
        return 0;
    if (completion->status != KB2_GPU_STATUS_OK)
        return !completion->mapping_id && !completion->length &&
            !completion->cache_policy && !completion->exchange_id &&
            !completion->generation && !completion->rights;
    return completion->mapping_id && !(completion->mapping_id & 4095u) &&
        completion->length && !(completion->length & 4095u) &&
        completion->exchange_id && completion->generation &&
        completion->rights && !(completion->rights & ~known_rights);
}

struct mapping_completion_format {
    uint32_t record_schema_id;
    uint32_t record_size;
    uint32_t mapping_id_offset;
    uint32_t length_offset;
    uint32_t cache_policy_offset;
    uint32_t reserved_offset;
    uint32_t attachment_argument_id;
    uint32_t attachment_role;
};

static const struct mapping_completion_format virtgpu_map_format = {
    .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_MAPPING_RESULT,
    .record_size = KB2_GPU_DRM_VIRTGPU_RECORD_MAPPING_RESULT_SIZE,
    .mapping_id_offset = KB2_GPU_DRM_VIRTGPU_RECORD_MAPPING_RESULT_MAPPING_ID_OFFSET,
    .length_offset = KB2_GPU_DRM_VIRTGPU_RECORD_MAPPING_RESULT_LENGTH_OFFSET,
    .cache_policy_offset = KB2_GPU_DRM_VIRTGPU_RECORD_MAPPING_RESULT_CACHE_POLICY_OFFSET,
    .reserved_offset = KB2_GPU_DRM_VIRTGPU_RECORD_MAPPING_RESULT_RESERVED_OFFSET,
    .attachment_argument_id =
        KB2_GPU_DRM_VIRTGPU_COMMAND_MAP_COMPLETION_ATTACHMENT_MAPPING_ARGUMENT_ID,
    .attachment_role =
        KB2_GPU_DRM_VIRTGPU_COMMAND_MAP_COMPLETION_ATTACHMENT_MAPPING_ROLE,
};

static const struct mapping_completion_format mode_map_format = {
    .record_schema_id = KB2_GPU_DRM_MODE_RECORD_MAPPING_RESULT,
    .record_size = KB2_GPU_DRM_MODE_RECORD_MAPPING_RESULT_SIZE,
    .mapping_id_offset = KB2_GPU_DRM_MODE_RECORD_MAPPING_RESULT_MAPPING_ID_OFFSET,
    .length_offset = KB2_GPU_DRM_MODE_RECORD_MAPPING_RESULT_LENGTH_OFFSET,
    .cache_policy_offset = KB2_GPU_DRM_MODE_RECORD_MAPPING_RESULT_CACHE_POLICY_OFFSET,
    .reserved_offset = KB2_GPU_DRM_MODE_RECORD_MAPPING_RESULT_RESERVED_OFFSET,
    .attachment_argument_id =
        KB2_GPU_DRM_MODE_COMMAND_MAP_DUMB_COMPLETION_ATTACHMENT_MAPPING_ARGUMENT_ID,
    .attachment_role =
        KB2_GPU_DRM_MODE_COMMAND_MAP_DUMB_COMPLETION_ATTACHMENT_MAPPING_ROLE,
};

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

static int attachment_completion_valid(
    const kb2_gpu_attachment_completion_t *completion) {
    const kb2_gpu_attachment_t *attachment = completion ?
        &completion->attachment : NULL;
    if (!completion || !completion->session_id ||
        completion->status > KB2_GPU_STATUS_DEVICE_LOST)
        return 0;
    if (completion->status != KB2_GPU_STATUS_OK)
        return !completion->argument_id && attachment &&
            !attachment->attachment_id && !attachment->object_class &&
            !attachment->exchange_id && !attachment->generation &&
            !attachment->rights && !attachment->role &&
            !attachment->ownership && !attachment->flags;
    return completion->argument_id && attachment->attachment_id &&
        attachment->object_class >= KB2_GPU_ATTACHMENT_MEMORY &&
        attachment->object_class <= KB2_GPU_ATTACHMENT_SYNCOBJ &&
        attachment->exchange_id && attachment->generation &&
        attachment->rights && attachment->role &&
        attachment->ownership >= KB2_GPU_ATTACHMENT_BORROW &&
        attachment->ownership <= KB2_GPU_ATTACHMENT_MOVE &&
        attachment->flags == KB2_GPU_ATTACHMENT_FLAG_OUTPUT;
}

kb2_protocol_status_t kb2_gpu_attachment_completion_encode(uint8_t *buffer,
    size_t capacity, size_t *size_out,
    const kb2_gpu_attachment_completion_t *completion) {
    if (!buffer || !size_out || !completion)
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (!attachment_completion_valid(completion)) return KB2_PROTOCOL_MALFORMED;
    if (completion->status != KB2_GPU_STATUS_OK) {
        const kb2_gpu_inline_completion_t error = {
            .session_id = completion->session_id, .status = completion->status};
        return kb2_gpu_inline_completion_encode(buffer, capacity, size_out, &error);
    }
    const size_t argument_offset = KB2_GPU_COMPLETION_HEADER_SIZE;
    const size_t attachment_offset = argument_offset + KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    const size_t size = attachment_offset + KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE;
    if (capacity < size) return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    memset(buffer, 0, size);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET, (uint32_t)size);
    memcpy(buffer + KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET, abi, sizeof(abi));
    memcpy(buffer + KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET, schema, sizeof(schema));
    write_u64(buffer + KB2_GPU_COMPLETION_HEADER_SESSION_ID_OFFSET, completion->session_id);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_STATUS_OFFSET, completion->status);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_DISPOSITION_OFFSET,
        KB2_GPU_DISPOSITION_COMPLETED);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_COUNT_OFFSET, 1);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET, 1);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET,
        (uint32_t)argument_offset);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_SPAN_TABLE_OFFSET_OFFSET,
        (uint32_t)attachment_offset);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET,
        (uint32_t)attachment_offset);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET, (uint32_t)size);

    uint8_t *argument = buffer + argument_offset;
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET,
        completion->argument_id);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET,
        KB2_GPU_ARGUMENT_ATTACHMENT);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET,
        KB2_GPU_ARGUMENT_FLAG_OUTPUT);
    write_u64(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_VALUE_OFFSET,
        completion->attachment.attachment_id);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET, 1);

    uint8_t *attachment = buffer + attachment_offset;
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_ATTACHMENT_ID_OFFSET,
        completion->attachment.attachment_id);
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_OBJECT_CLASS_OFFSET,
        completion->attachment.object_class);
    write_u64(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_EXCHANGE_ID_OFFSET,
        completion->attachment.exchange_id);
    write_u64(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_GENERATION_OFFSET,
        completion->attachment.generation);
    write_u64(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_RIGHTS_OFFSET,
        completion->attachment.rights);
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_ROLE_OFFSET,
        completion->attachment.role);
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_OWNERSHIP_OFFSET,
        completion->attachment.ownership);
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_FLAGS_OFFSET,
        completion->attachment.flags);
    *size_out = size;
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_gpu_attachment_completion_decode(const uint8_t *buffer,
    size_t size, uint64_t expected_session, uint64_t expected_generation,
    kb2_gpu_attachment_completion_t *completion_out) {
    if (!buffer || !completion_out || !expected_session || !expected_generation)
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (size < KB2_GPU_COMPLETION_HEADER_SIZE)
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    uint32_t status = read_u32(buffer + KB2_GPU_COMPLETION_HEADER_STATUS_OFFSET);
    if (status != KB2_GPU_STATUS_OK) {
        kb2_gpu_inline_completion_t error;
        kb2_protocol_status_t decoded = kb2_gpu_inline_completion_decode(
            buffer, size, expected_session, &error);
        if (decoded != KB2_PROTOCOL_OK) return decoded;
        *completion_out = (kb2_gpu_attachment_completion_t){
            .session_id = error.session_id, .status = error.status};
        return KB2_PROTOCOL_OK;
    }
    const size_t argument_offset = KB2_GPU_COMPLETION_HEADER_SIZE;
    const size_t attachment_offset = argument_offset + KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    const size_t expected_size = attachment_offset + KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE;
    if (size != expected_size ||
        memcmp(buffer + KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET, abi, sizeof(abi)) ||
        memcmp(buffer + KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET, schema, sizeof(schema)) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET) != size ||
        read_u64(buffer + KB2_GPU_COMPLETION_HEADER_SESSION_ID_OFFSET) != expected_session ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_DISPOSITION_OFFSET) !=
            KB2_GPU_DISPOSITION_COMPLETED ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_COUNT_OFFSET) != 1 ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_SPAN_COUNT_OFFSET) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET) != 1 ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_LENGTH_OFFSET) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET) !=
            argument_offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_SPAN_TABLE_OFFSET_OFFSET) !=
            attachment_offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET) !=
            attachment_offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET) != size ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_DETAIL_SET_ID_OFFSET) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_DETAIL_CODE_OFFSET) ||
        read_u64(buffer + KB2_GPU_COMPLETION_HEADER_RESERVED_OFFSET))
        return KB2_PROTOCOL_MALFORMED;
    const uint8_t *argument = buffer + argument_offset;
    const uint8_t *attachment = buffer + attachment_offset;
    kb2_gpu_attachment_completion_t completion = {
        .session_id = expected_session,
        .status = status,
        .argument_id = read_u32(argument +
            KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET),
        .attachment = {
            .attachment_id = read_u32(attachment +
                KB2_GPU_ATTACHMENT_DESCRIPTOR_ATTACHMENT_ID_OFFSET),
            .object_class = read_u32(attachment +
                KB2_GPU_ATTACHMENT_DESCRIPTOR_OBJECT_CLASS_OFFSET),
            .exchange_id = read_u64(attachment +
                KB2_GPU_ATTACHMENT_DESCRIPTOR_EXCHANGE_ID_OFFSET),
            .generation = read_u64(attachment +
                KB2_GPU_ATTACHMENT_DESCRIPTOR_GENERATION_OFFSET),
            .rights = read_u64(attachment +
                KB2_GPU_ATTACHMENT_DESCRIPTOR_RIGHTS_OFFSET),
            .role = read_u32(attachment +
                KB2_GPU_ATTACHMENT_DESCRIPTOR_ROLE_OFFSET),
            .ownership = read_u32(attachment +
                KB2_GPU_ATTACHMENT_DESCRIPTOR_OWNERSHIP_OFFSET),
            .flags = read_u32(attachment +
                KB2_GPU_ATTACHMENT_DESCRIPTOR_FLAGS_OFFSET),
        },
    };
    if (read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET) !=
            KB2_GPU_ARGUMENT_ATTACHMENT ||
        read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET) !=
            KB2_GPU_ARGUMENT_FLAG_OUTPUT ||
        read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET) ||
        read_u64(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_VALUE_OFFSET) !=
            completion.attachment.attachment_id ||
        read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET) != 1 ||
        read_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_RESERVED_OFFSET) ||
        completion.attachment.generation != expected_generation ||
        read_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_RESERVED_OFFSET) ||
        !attachment_completion_valid(&completion))
        return KB2_PROTOCOL_MALFORMED;
    *completion_out = completion;
    return KB2_PROTOCOL_OK;
}

static kb2_protocol_status_t mapping_completion_encode(uint8_t *buffer,
    size_t capacity, size_t *size_out,
    const kb2_gpu_virtgpu_map_completion_t *completion,
    const struct mapping_completion_format *format) {
    if (!buffer || !size_out || !completion || !format)
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (!map_valid(completion)) return KB2_PROTOCOL_MALFORMED;
    if (completion->status != KB2_GPU_STATUS_OK) {
        const kb2_gpu_inline_completion_t error = {
            .session_id = completion->session_id,
            .status = completion->status,
        };
        return kb2_gpu_inline_completion_encode(
            buffer, capacity, size_out, &error);
    }

    const size_t argument_offset = KB2_GPU_COMPLETION_HEADER_SIZE;
    const size_t attachment_offset =
        argument_offset + 2 * KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    const size_t inline_offset =
        attachment_offset + KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE;
    const size_t size = inline_offset + format->record_size;
    if (capacity < size) return KB2_PROTOCOL_BUFFER_TOO_SMALL;

    memset(buffer, 0, size);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET,
        (uint32_t)size);
    memcpy(buffer + KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET,
        abi, sizeof(abi));
    memcpy(buffer + KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET,
        schema, sizeof(schema));
    write_u64(buffer + KB2_GPU_COMPLETION_HEADER_SESSION_ID_OFFSET,
        completion->session_id);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_STATUS_OFFSET,
        KB2_GPU_STATUS_OK);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_DISPOSITION_OFFSET,
        KB2_GPU_DISPOSITION_COMPLETED);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_COUNT_OFFSET, 2);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET, 1);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_LENGTH_OFFSET,
        format->record_size);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET,
        (uint32_t)argument_offset);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_SPAN_TABLE_OFFSET_OFFSET,
        (uint32_t)attachment_offset);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET,
        (uint32_t)attachment_offset);
    write_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET,
        (uint32_t)inline_offset);

    uint8_t *argument = buffer + argument_offset;
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET, 1);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET,
        KB2_GPU_ARGUMENT_INLINE);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET,
        KB2_GPU_ARGUMENT_FLAG_OUTPUT);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET,
        format->record_schema_id);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET,
        format->record_size);

    argument += KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET,
        format->attachment_argument_id);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET,
        KB2_GPU_ARGUMENT_ATTACHMENT);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET,
        KB2_GPU_ARGUMENT_FLAG_OUTPUT);
    write_u64(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_VALUE_OFFSET, 1);
    write_u32(argument + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET, 1);

    uint8_t *attachment = buffer + attachment_offset;
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_ATTACHMENT_ID_OFFSET, 1);
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_OBJECT_CLASS_OFFSET,
        KB2_GPU_ATTACHMENT_MEMORY);
    write_u64(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_EXCHANGE_ID_OFFSET,
        completion->exchange_id);
    write_u64(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_GENERATION_OFFSET,
        completion->generation);
    write_u64(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_RIGHTS_OFFSET,
        completion->rights);
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_ROLE_OFFSET,
        format->attachment_role);
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_OWNERSHIP_OFFSET,
        KB2_GPU_ATTACHMENT_SHARE);
    write_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_FLAGS_OFFSET,
        KB2_GPU_ATTACHMENT_FLAG_OUTPUT);

    uint8_t *record = buffer + inline_offset;
    write_u64(record + format->mapping_id_offset,
        completion->mapping_id);
    write_u64(record + format->length_offset,
        completion->length);
    write_u32(record + format->cache_policy_offset,
        completion->cache_policy);
    *size_out = size;
    return KB2_PROTOCOL_OK;
}

static kb2_protocol_status_t mapping_completion_decode(const uint8_t *buffer,
    size_t size, uint64_t expected_session, uint64_t expected_generation,
    kb2_gpu_virtgpu_map_completion_t *completion_out,
    const struct mapping_completion_format *format) {
    if (!buffer || !completion_out || !format || !expected_session ||
        !expected_generation)
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    if (size < KB2_GPU_COMPLETION_HEADER_SIZE)
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;

    const uint32_t status =
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_STATUS_OFFSET);
    if (status != KB2_GPU_STATUS_OK) {
        kb2_gpu_inline_completion_t error;
        kb2_protocol_status_t decoded = kb2_gpu_inline_completion_decode(
            buffer, size, expected_session, &error);
        if (decoded != KB2_PROTOCOL_OK) return decoded;
        *completion_out = (kb2_gpu_virtgpu_map_completion_t){
            .session_id = error.session_id,
            .status = error.status,
        };
        return KB2_PROTOCOL_OK;
    }

    const size_t argument_offset = KB2_GPU_COMPLETION_HEADER_SIZE;
    const size_t attachment_offset =
        argument_offset + 2 * KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    const size_t inline_offset =
        attachment_offset + KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE;
    const size_t expected_size = inline_offset + format->record_size;
    if (size != expected_size ||
        memcmp(buffer + KB2_GPU_COMPLETION_HEADER_ABI_IDENTITY_OFFSET,
            abi, sizeof(abi)) ||
        memcmp(buffer + KB2_GPU_COMPLETION_HEADER_SCHEMA_DIGEST_OFFSET,
            schema, sizeof(schema)) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_TOTAL_SIZE_OFFSET) != size ||
        read_u64(buffer + KB2_GPU_COMPLETION_HEADER_SESSION_ID_OFFSET) !=
            expected_session ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_DISPOSITION_OFFSET) !=
            KB2_GPU_DISPOSITION_COMPLETED ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_COUNT_OFFSET) != 2 ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_SPAN_COUNT_OFFSET) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET) != 1 ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_LENGTH_OFFSET) !=
            format->record_size ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET) !=
            argument_offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_SPAN_TABLE_OFFSET_OFFSET) !=
            attachment_offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET) !=
            attachment_offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET) !=
            inline_offset ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_DETAIL_SET_ID_OFFSET) ||
        read_u32(buffer + KB2_GPU_COMPLETION_HEADER_DETAIL_CODE_OFFSET) ||
        read_u64(buffer + KB2_GPU_COMPLETION_HEADER_RESERVED_OFFSET))
        return KB2_PROTOCOL_MALFORMED;

    const uint8_t *inline_argument = buffer + argument_offset;
    const uint8_t *attachment_argument =
        inline_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    const uint8_t *attachment = buffer + attachment_offset;
    const uint8_t *record = buffer + inline_offset;
    if (read_u32(inline_argument +
            KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET) != 1 ||
        read_u32(inline_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET) !=
            KB2_GPU_ARGUMENT_INLINE ||
        read_u32(inline_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET) !=
            KB2_GPU_ARGUMENT_FLAG_OUTPUT ||
        read_u32(inline_argument +
            KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET) !=
            format->record_schema_id ||
        read_u64(inline_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_VALUE_OFFSET) ||
        read_u32(inline_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET) !=
            format->record_size ||
        read_u32(inline_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_RESERVED_OFFSET) ||
        read_u32(attachment_argument +
            KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET) !=
            format->attachment_argument_id ||
        read_u32(attachment_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET) !=
            KB2_GPU_ARGUMENT_ATTACHMENT ||
        read_u32(attachment_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET) !=
            KB2_GPU_ARGUMENT_FLAG_OUTPUT ||
        read_u32(attachment_argument +
            KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET) ||
        read_u64(attachment_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_VALUE_OFFSET) != 1 ||
        read_u32(attachment_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET) != 1 ||
        read_u32(attachment_argument + KB2_GPU_ARGUMENT_DESCRIPTOR_RESERVED_OFFSET) ||
        read_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_ATTACHMENT_ID_OFFSET) != 1 ||
        read_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_OBJECT_CLASS_OFFSET) !=
            KB2_GPU_ATTACHMENT_MEMORY ||
        read_u64(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_GENERATION_OFFSET) !=
            expected_generation ||
        read_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_ROLE_OFFSET) !=
            format->attachment_role ||
        read_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_OWNERSHIP_OFFSET) !=
            KB2_GPU_ATTACHMENT_SHARE ||
        read_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_FLAGS_OFFSET) !=
            KB2_GPU_ATTACHMENT_FLAG_OUTPUT ||
        read_u32(attachment + KB2_GPU_ATTACHMENT_DESCRIPTOR_RESERVED_OFFSET) ||
        read_u32(record + format->reserved_offset))
        return KB2_PROTOCOL_MALFORMED;

    const kb2_gpu_virtgpu_map_completion_t completion = {
        .session_id = expected_session,
        .status = KB2_GPU_STATUS_OK,
        .mapping_id = read_u64(record + format->mapping_id_offset),
        .length = read_u64(record + format->length_offset),
        .cache_policy = read_u32(record + format->cache_policy_offset),
        .exchange_id = read_u64(attachment +
            KB2_GPU_ATTACHMENT_DESCRIPTOR_EXCHANGE_ID_OFFSET),
        .generation = expected_generation,
        .rights = read_u64(attachment +
            KB2_GPU_ATTACHMENT_DESCRIPTOR_RIGHTS_OFFSET),
    };
    if (!map_valid(&completion)) return KB2_PROTOCOL_MALFORMED;
    *completion_out = completion;
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_gpu_virtgpu_map_completion_encode(uint8_t *buffer,
    size_t capacity, size_t *size_out,
    const kb2_gpu_virtgpu_map_completion_t *completion) {
    return mapping_completion_encode(buffer, capacity, size_out, completion,
        &virtgpu_map_format);
}

kb2_protocol_status_t kb2_gpu_virtgpu_map_completion_decode(const uint8_t *buffer,
    size_t size, uint64_t expected_session, uint64_t expected_generation,
    kb2_gpu_virtgpu_map_completion_t *completion_out) {
    return mapping_completion_decode(buffer, size, expected_session,
        expected_generation, completion_out, &virtgpu_map_format);
}

kb2_protocol_status_t kb2_gpu_mode_map_completion_encode(uint8_t *buffer,
    size_t capacity, size_t *size_out,
    const kb2_gpu_mode_map_completion_t *completion) {
    return mapping_completion_encode(buffer, capacity, size_out, completion,
        &mode_map_format);
}

kb2_protocol_status_t kb2_gpu_mode_map_completion_decode(const uint8_t *buffer,
    size_t size, uint64_t expected_session, uint64_t expected_generation,
    kb2_gpu_mode_map_completion_t *completion_out) {
    return mapping_completion_decode(buffer, size, expected_session,
        expected_generation, completion_out, &mode_map_format);
}
