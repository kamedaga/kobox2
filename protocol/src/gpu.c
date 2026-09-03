/* SPDX-License-Identifier: MIT */

#include <kobox2/gpu.h>

#include <limits.h>
#include <string.h>

static const uint8_t kb2_gpu_abi_identity[KB2_GPU_ABI_IDENTITY_SIZE] =
    KB2_GPU_ABI_IDENTITY_BYTES;
static const uint8_t kb2_gpu_schema_digest[KB2_GPU_SCHEMA_DIGEST_SIZE] =
    KB2_GPU_SCHEMA_SHA256_BYTES;
static const uint8_t kb2_gpu_drm_core_digest[KB2_GPU_SCHEMA_DIGEST_SIZE] =
    KB2_GPU_DRM_CORE_SCHEMA_SHA256_BYTES;
static const uint8_t kb2_gpu_drm_mode_digest[KB2_GPU_SCHEMA_DIGEST_SIZE] =
    KB2_GPU_DRM_MODE_SCHEMA_SHA256_BYTES;
static const uint8_t kb2_gpu_drm_virtgpu_digest[KB2_GPU_SCHEMA_DIGEST_SIZE] =
    KB2_GPU_DRM_VIRTGPU_SCHEMA_SHA256_BYTES;
static const uint8_t kb2_gpu_drm_amdgpu_digest[KB2_GPU_SCHEMA_DIGEST_SIZE] =
    KB2_GPU_DRM_AMDGPU_SCHEMA_SHA256_BYTES;

static uint32_t load_u32(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static uint64_t load_u64(const uint8_t *source) {
    return (uint64_t)load_u32(source) | ((uint64_t)load_u32(source + 4) << 32u);
}

static void store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static void store_u64(uint8_t *destination, uint64_t value) {
    store_u32(destination, (uint32_t)value);
    store_u32(destination + 4, (uint32_t)(value >> 32u));
}

static int zero_bytes(const uint8_t *bytes, size_t size) {
    size_t index;

    for (index = 0; index < size; ++index) {
        if (bytes[index] != 0) {
            return 0;
        }
    }
    return 1;
}

static int size_add_multiply(size_t base, size_t count, size_t width, size_t *result_out) {
    if (count != 0 && width > (SIZE_MAX - base) / count) {
        return 0;
    }
    *result_out = base + count * width;
    return 1;
}

static int profile_kind_valid(uint32_t profile_kind) {
    return profile_kind == KB2_GPU_PROFILE_VIRGL || profile_kind == KB2_GPU_PROFILE_AMDGPU;
}

static void set_descriptor(uint32_t set_id,
                           uint32_t queue_class,
                           uint32_t command_count,
                           const uint8_t *digest,
                           kb2_gpu_command_set_t *set_out) {
    memset(set_out, 0, sizeof(*set_out));
    set_out->set_id = set_id;
    set_out->queue_class = queue_class;
    set_out->command_count = command_count;
    memcpy(set_out->schema_digest, digest, sizeof(set_out->schema_digest));
}

const char *kb2_gpu_schema_sha256_hex(void) {
    return KB2_GPU_SCHEMA_SHA256_HEX;
}

kb2_protocol_status_t kb2_gpu_copy_schema_digest(uint8_t *digest_out, size_t digest_size) {
    if (digest_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (digest_size < sizeof(kb2_gpu_schema_digest)) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    memcpy(digest_out, kb2_gpu_schema_digest, sizeof(kb2_gpu_schema_digest));
    return KB2_PROTOCOL_OK;
}

size_t kb2_gpu_profile_command_set_count(uint32_t profile_kind) {
    return profile_kind_valid(profile_kind) ? 3u : 0u;
}

kb2_protocol_status_t kb2_gpu_profile_command_set(uint32_t profile_kind,
                                                   size_t index,
                                                   kb2_gpu_command_set_t *set_out) {
    if (set_out == NULL || !profile_kind_valid(profile_kind)) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index >= kb2_gpu_profile_command_set_count(profile_kind)) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (index == 0) {
        set_descriptor(KB2_GPU_DRM_CORE_SET_ID,
                       KB2_GPU_QUEUE_EXECUTION,
                       KB2_GPU_DRM_CORE_COMMAND_COUNT,
                       kb2_gpu_drm_core_digest,
                       set_out);
    } else if (index == 1) {
        set_descriptor(KB2_GPU_DRM_MODE_SET_ID,
                       KB2_GPU_QUEUE_DISPLAY,
                       KB2_GPU_DRM_MODE_COMMAND_COUNT,
                       kb2_gpu_drm_mode_digest,
                       set_out);
    } else if (profile_kind == KB2_GPU_PROFILE_VIRGL) {
        set_descriptor(KB2_GPU_DRM_VIRTGPU_SET_ID,
                       KB2_GPU_QUEUE_EXECUTION,
                       KB2_GPU_DRM_VIRTGPU_COMMAND_COUNT,
                       kb2_gpu_drm_virtgpu_digest,
                       set_out);
    } else {
        set_descriptor(KB2_GPU_DRM_AMDGPU_SET_ID,
                       KB2_GPU_QUEUE_EXECUTION,
                       KB2_GPU_DRM_AMDGPU_COMMAND_COUNT,
                       kb2_gpu_drm_amdgpu_digest,
                       set_out);
    }
    return KB2_PROTOCOL_OK;
}

static int command_allowed(uint32_t profile_kind,
                           uint32_t queue_class,
                           uint32_t command_set_id,
                           uint32_t command_id) {
    size_t index;

    for (index = 0; index < kb2_gpu_profile_command_set_count(profile_kind); ++index) {
        kb2_gpu_command_set_t set;

        if (kb2_gpu_profile_command_set(profile_kind, index, &set) != KB2_PROTOCOL_OK) {
            return 0;
        }
        if (set.set_id == command_set_id) {
            return set.queue_class == queue_class && command_id != 0 &&
                   command_id <= set.command_count;
        }
    }
    return 0;
}

static int wait_command(uint32_t command_set_id, uint32_t command_id) {
    switch (command_set_id) {
    case KB2_GPU_DRM_CORE_SET_ID:
        return command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_WAIT ||
               command_id == KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_TIMELINE_WAIT;
    case KB2_GPU_DRM_MODE_SET_ID:
        return command_id == KB2_GPU_DRM_MODE_COMMAND_WAIT_VBLANK;
    case KB2_GPU_DRM_VIRTGPU_SET_ID:
        return command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_WAIT;
    case KB2_GPU_DRM_AMDGPU_SET_ID:
        return command_id == KB2_GPU_DRM_AMDGPU_COMMAND_GEM_WAIT_IDLE ||
               command_id == KB2_GPU_DRM_AMDGPU_COMMAND_WAIT_CS ||
               command_id == KB2_GPU_DRM_AMDGPU_COMMAND_WAIT_FENCES ||
               command_id == KB2_GPU_DRM_AMDGPU_COMMAND_USERQ_WAIT;
    default:
        return 0;
    }
}

static int regions_valid(const kb2_gpu_region_t *regions, size_t region_count) {
    const uint32_t known_rights = KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE;
    size_t index;
    size_t previous;

    if (region_count > KB2_PROTOCOL_MAX_REGIONS ||
        (region_count != 0 && regions == NULL)) {
        return 0;
    }
    for (index = 0; index < region_count; ++index) {
        if (regions[index].region_id == 0 || regions[index].length == 0 ||
            regions[index].rights == 0 || (regions[index].rights & ~known_rights) != 0) {
            return 0;
        }
        for (previous = 0; previous < index; ++previous) {
            if (regions[index].region_id == regions[previous].region_id) {
                return 0;
            }
        }
    }
    return 1;
}

static const kb2_gpu_region_t *find_region(const kb2_gpu_region_t *regions,
                                           size_t region_count,
                                           uint32_t region_id) {
    size_t index;

    for (index = 0; index < region_count; ++index) {
        if (regions[index].region_id == region_id) {
            return &regions[index];
        }
    }
    return NULL;
}

static int spans_overlap(const kb2_gpu_span_t *left, const kb2_gpu_span_t *right) {
    return left->region_id == right->region_id && left->offset < right->offset + right->length &&
           right->offset < left->offset + left->length;
}

static int spans_valid(const kb2_gpu_span_t *spans,
                       size_t span_count,
                       const kb2_gpu_region_t *regions,
                       size_t region_count) {
    const uint32_t known_rights = KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE;
    const uint32_t known_flags = KB2_GPU_SPAN_FLAG_INPUT | KB2_GPU_SPAN_FLAG_OUTPUT;
    size_t index;
    size_t previous;

    if (span_count != 0 && spans == NULL) {
        return 0;
    }
    for (index = 0; index < span_count; ++index) {
        const kb2_gpu_span_t *span = &spans[index];
        const kb2_gpu_region_t *region = find_region(regions, region_count, span->region_id);
        uint32_t required_rights = 0;

        if (span->span_id == 0 || (index != 0 && span->span_id <= spans[index - 1].span_id) ||
            span->length == 0 || span->offset > UINT64_MAX - span->length ||
            span->rights == 0 || (span->rights & ~known_rights) != 0 ||
            span->element_count == 0 || span->flags == 0 ||
            (span->flags & ~known_flags) != 0 || region == NULL ||
            span->offset > region->length || span->length > region->length - span->offset) {
            return 0;
        }
        if ((span->flags & KB2_GPU_SPAN_FLAG_INPUT) != 0) {
            required_rights |= KB2_GPU_SPAN_RIGHT_READ;
        }
        if ((span->flags & KB2_GPU_SPAN_FLAG_OUTPUT) != 0) {
            required_rights |= KB2_GPU_SPAN_RIGHT_WRITE;
        }
        if ((span->rights & required_rights) != required_rights ||
            (region->rights & span->rights) != span->rights) {
            return 0;
        }
        for (previous = 0; previous < index; ++previous) {
            if (spans_overlap(span, &spans[previous]) &&
                ((span->rights | spans[previous].rights) & KB2_GPU_SPAN_RIGHT_WRITE) != 0) {
                return 0;
            }
        }
    }
    return 1;
}

static const kb2_gpu_span_t *find_span(const kb2_gpu_span_t *spans,
                                       size_t span_count,
                                       uint32_t span_id) {
    size_t index;

    for (index = 0; index < span_count; ++index) {
        if (spans[index].span_id == span_id) {
            return &spans[index];
        }
    }
    return NULL;
}

static int attachments_valid(const kb2_gpu_attachment_t *attachments,
                             size_t attachment_count,
                             uint64_t generation) {
    size_t index;
    size_t previous;

    if (attachment_count != 0 && attachments == NULL) {
        return 0;
    }
    for (index = 0; index < attachment_count; ++index) {
        const kb2_gpu_attachment_t *attachment = &attachments[index];

        if (attachment->attachment_id == 0 ||
            (index != 0 && attachment->attachment_id <= attachments[index - 1].attachment_id) ||
            attachment->object_class < KB2_GPU_ATTACHMENT_MEMORY ||
            attachment->object_class > KB2_GPU_ATTACHMENT_SYNCOBJ ||
            attachment->exchange_id == 0 || attachment->generation != generation ||
            attachment->rights == 0 || attachment->ownership < KB2_GPU_ATTACHMENT_BORROW ||
            attachment->ownership > KB2_GPU_ATTACHMENT_MOVE ||
            attachment->flags != KB2_GPU_ATTACHMENT_FLAG_INPUT) {
            return 0;
        }
        for (previous = 0; previous < index; ++previous) {
            if (attachment->exchange_id == attachments[previous].exchange_id) {
                return 0;
            }
        }
    }
    return 1;
}

static const kb2_gpu_attachment_t *find_attachment(
    const kb2_gpu_attachment_t *attachments,
    size_t attachment_count,
    uint32_t attachment_id) {
    size_t index;

    for (index = 0; index < attachment_count; ++index) {
        if (attachments[index].attachment_id == attachment_id) {
            return &attachments[index];
        }
    }
    return NULL;
}

static int argument_references_valid(const kb2_gpu_command_source_t *source,
                                     const kb2_gpu_argument_t *argument) {
    switch (argument->kind) {
    case KB2_GPU_ARGUMENT_U64:
    case KB2_GPU_ARGUMENT_I64:
    case KB2_GPU_ARGUMENT_ENUM:
    case KB2_GPU_ARGUMENT_BITSET:
        return argument->record_schema_id == 0 && argument->count == 1;
    case KB2_GPU_ARGUMENT_BOOL:
        return argument->record_schema_id == 0 && argument->count == 1 && argument->value <= 1;
    case KB2_GPU_ARGUMENT_OBJECT:
        return argument->record_schema_id != 0 && argument->count == 1;
    case KB2_GPU_ARGUMENT_INLINE:
        return argument->count != 0 && argument->value <= source->inline_length &&
               argument->count <= source->inline_length - (size_t)argument->value;
    case KB2_GPU_ARGUMENT_SPAN: {
        const kb2_gpu_span_t *span;

        if (argument->value > UINT32_MAX) {
            return 0;
        }
        span = find_span(source->spans, source->span_count, (uint32_t)argument->value);
        return span != NULL && argument->count == span->element_count &&
               argument->record_schema_id == span->record_schema_id;
    }
    case KB2_GPU_ARGUMENT_ATTACHMENT:
        return argument->record_schema_id == 0 && argument->count == 1 &&
               argument->value <= UINT32_MAX &&
               find_attachment(source->attachments,
                               source->attachment_count,
                               (uint32_t)argument->value) != NULL;
    default:
        return 0;
    }
}

static int arguments_valid(const kb2_gpu_command_source_t *source) {
    const uint32_t known_flags = KB2_GPU_ARGUMENT_FLAG_INPUT | KB2_GPU_ARGUMENT_FLAG_OUTPUT;
    size_t index;
    size_t referenced;

    if (source->argument_count != 0 && source->arguments == NULL) {
        return 0;
    }
    for (index = 0; index < source->argument_count; ++index) {
        const kb2_gpu_argument_t *argument = &source->arguments[index];

        if (argument->argument_id == 0 ||
            (index != 0 && argument->argument_id <= source->arguments[index - 1].argument_id) ||
            argument->flags == 0 || (argument->flags & ~known_flags) != 0 ||
            !argument_references_valid(source, argument)) {
            return 0;
        }
    }
    for (index = 0; index < source->span_count; ++index) {
        for (referenced = 0; referenced < source->argument_count; ++referenced) {
            if (source->arguments[referenced].kind == KB2_GPU_ARGUMENT_SPAN &&
                source->arguments[referenced].value == source->spans[index].span_id) {
                break;
            }
        }
        if (referenced == source->argument_count) {
            return 0;
        }
    }
    for (index = 0; index < source->attachment_count; ++index) {
        for (referenced = 0; referenced < source->argument_count; ++referenced) {
            if (source->arguments[referenced].kind == KB2_GPU_ARGUMENT_ATTACHMENT &&
                source->arguments[referenced].value ==
                    source->attachments[index].attachment_id) {
                break;
            }
        }
        if (referenced == source->argument_count) {
            return 0;
        }
    }
    return 1;
}

typedef struct kb2_gpu_inline_contract {
    uint32_t record_id;
    uint32_t size;
} kb2_gpu_inline_contract_t;

typedef struct kb2_gpu_request_contract {
    uint32_t command_id;
    uint32_t inline_record_id;
    uint32_t inline_size;
    uint32_t span_capacity;
    uint32_t attachment_capacity;
} kb2_gpu_request_contract_t;

typedef struct kb2_gpu_span_contract {
    uint32_t command_id;
    uint32_t argument_id;
    uint32_t direction;
    uint32_t minimum;
    uint32_t maximum;
    uint32_t record_id;
    uint32_t record_size;
} kb2_gpu_span_contract_t;

typedef struct kb2_gpu_attachment_contract {
    uint32_t command_id;
    uint32_t argument_id;
    uint32_t role;
    uint32_t ownership;
    uint32_t optional;
    uint32_t object_class;
} kb2_gpu_attachment_contract_t;

#define KB2_GPU_REQUEST_CONTRACT(command_id, record_id, record_size, spans, attachments)       \
    {command_id, record_id, record_size, spans, attachments},
#define KB2_GPU_SPAN_CONTRACT(command_id, argument_id, direction, minimum, maximum, record_id,  \
                              record_size)                                                       \
    {command_id, argument_id, direction, minimum, maximum, record_id, record_size},
#define KB2_GPU_ATTACHMENT_CONTRACT(command_id, argument_id, role, ownership, optional,         \
                                    object_class)                                                \
    {command_id, argument_id, role, ownership, optional, object_class},

static const kb2_gpu_request_contract_t kb2_gpu_core_request_contracts[] = {
    KB2_GPU_DRM_CORE_REQUEST_CONTRACT_CATALOG(KB2_GPU_REQUEST_CONTRACT)
};
static const kb2_gpu_span_contract_t kb2_gpu_core_span_contracts[] = {
    KB2_GPU_DRM_CORE_REQUEST_SPAN_CATALOG(KB2_GPU_SPAN_CONTRACT)
};
static const kb2_gpu_attachment_contract_t kb2_gpu_core_attachment_contracts[] = {
    KB2_GPU_DRM_CORE_REQUEST_ATTACHMENT_CATALOG(KB2_GPU_ATTACHMENT_CONTRACT)
};
static const kb2_gpu_request_contract_t kb2_gpu_mode_request_contracts[] = {
    KB2_GPU_DRM_MODE_REQUEST_CONTRACT_CATALOG(KB2_GPU_REQUEST_CONTRACT)
};
static const kb2_gpu_span_contract_t kb2_gpu_mode_span_contracts[] = {
    KB2_GPU_DRM_MODE_REQUEST_SPAN_CATALOG(KB2_GPU_SPAN_CONTRACT)
};
static const kb2_gpu_request_contract_t kb2_gpu_virtgpu_request_contracts[] = {
    KB2_GPU_DRM_VIRTGPU_REQUEST_CONTRACT_CATALOG(KB2_GPU_REQUEST_CONTRACT)
};
static const kb2_gpu_span_contract_t kb2_gpu_virtgpu_span_contracts[] = {
    KB2_GPU_DRM_VIRTGPU_REQUEST_SPAN_CATALOG(KB2_GPU_SPAN_CONTRACT)
};
static const kb2_gpu_attachment_contract_t kb2_gpu_virtgpu_attachment_contracts[] = {
    KB2_GPU_DRM_VIRTGPU_REQUEST_ATTACHMENT_CATALOG(KB2_GPU_ATTACHMENT_CONTRACT)
};

_Static_assert(sizeof(kb2_gpu_core_request_contracts) /
                       sizeof(kb2_gpu_core_request_contracts[0]) ==
                   KB2_GPU_DRM_CORE_COMMAND_COUNT,
               "DRM core request catalog must cover every command");
_Static_assert(sizeof(kb2_gpu_mode_request_contracts) /
                       sizeof(kb2_gpu_mode_request_contracts[0]) ==
                   KB2_GPU_DRM_MODE_COMMAND_COUNT,
               "DRM mode request catalog must cover every command");
_Static_assert(sizeof(kb2_gpu_virtgpu_request_contracts) /
                       sizeof(kb2_gpu_virtgpu_request_contracts[0]) ==
                   KB2_GPU_DRM_VIRTGPU_COMMAND_COUNT,
               "DRM virtgpu request catalog must cover every command");

#undef KB2_GPU_REQUEST_CONTRACT
#undef KB2_GPU_SPAN_CONTRACT
#undef KB2_GPU_ATTACHMENT_CONTRACT

#define KB2_GPU_INLINE_CONTRACT(record_id, record_size) {record_id, record_size},
static const kb2_gpu_inline_contract_t kb2_gpu_amdgpu_request_contracts[] = {
    KB2_GPU_DRM_AMDGPU_REQUEST_INLINE_CATALOG(KB2_GPU_INLINE_CONTRACT)
};
#undef KB2_GPU_INLINE_CONTRACT

_Static_assert(
    sizeof(kb2_gpu_amdgpu_request_contracts) /
            sizeof(kb2_gpu_amdgpu_request_contracts[0]) ==
        KB2_GPU_DRM_AMDGPU_COMMAND_COUNT,
    "AMDGPU request catalog must cover every command");

static const kb2_gpu_argument_t *find_argument(const kb2_gpu_command_source_t *source,
                                                uint32_t argument_id) {
    size_t index;

    for (index = 0; index < source->argument_count; ++index) {
        if (source->arguments[index].argument_id == argument_id) {
            return &source->arguments[index];
        }
    }
    return NULL;
}

static int request_catalog(uint32_t command_set_id,
                           const kb2_gpu_request_contract_t **requests_out,
                           size_t *request_count_out,
                           const kb2_gpu_span_contract_t **spans_out,
                           size_t *span_count_out,
                           const kb2_gpu_attachment_contract_t **attachments_out,
                           size_t *attachment_count_out) {
    switch (command_set_id) {
    case KB2_GPU_DRM_CORE_SET_ID:
        *requests_out = kb2_gpu_core_request_contracts;
        *request_count_out = sizeof(kb2_gpu_core_request_contracts) /
                             sizeof(kb2_gpu_core_request_contracts[0]);
        *spans_out = kb2_gpu_core_span_contracts;
        *span_count_out = sizeof(kb2_gpu_core_span_contracts) /
                          sizeof(kb2_gpu_core_span_contracts[0]);
        *attachments_out = kb2_gpu_core_attachment_contracts;
        *attachment_count_out = sizeof(kb2_gpu_core_attachment_contracts) /
                                sizeof(kb2_gpu_core_attachment_contracts[0]);
        return 1;
    case KB2_GPU_DRM_MODE_SET_ID:
        *requests_out = kb2_gpu_mode_request_contracts;
        *request_count_out = sizeof(kb2_gpu_mode_request_contracts) /
                             sizeof(kb2_gpu_mode_request_contracts[0]);
        *spans_out = kb2_gpu_mode_span_contracts;
        *span_count_out = sizeof(kb2_gpu_mode_span_contracts) /
                          sizeof(kb2_gpu_mode_span_contracts[0]);
        *attachments_out = NULL;
        *attachment_count_out = 0;
        return 1;
    case KB2_GPU_DRM_VIRTGPU_SET_ID:
        *requests_out = kb2_gpu_virtgpu_request_contracts;
        *request_count_out = sizeof(kb2_gpu_virtgpu_request_contracts) /
                             sizeof(kb2_gpu_virtgpu_request_contracts[0]);
        *spans_out = kb2_gpu_virtgpu_span_contracts;
        *span_count_out = sizeof(kb2_gpu_virtgpu_span_contracts) /
                          sizeof(kb2_gpu_virtgpu_span_contracts[0]);
        *attachments_out = kb2_gpu_virtgpu_attachment_contracts;
        *attachment_count_out = sizeof(kb2_gpu_virtgpu_attachment_contracts) /
                                sizeof(kb2_gpu_virtgpu_attachment_contracts[0]);
        return 1;
    default:
        return 0;
    }
}

static uint32_t direction_flags(uint32_t direction) {
    switch (direction) {
    case 1:
        return KB2_GPU_ARGUMENT_FLAG_INPUT;
    case 2:
        return KB2_GPU_ARGUMENT_FLAG_OUTPUT;
    case 3:
        return KB2_GPU_ARGUMENT_FLAG_INPUT | KB2_GPU_ARGUMENT_FLAG_OUTPUT;
    default:
        return 0;
    }
}

static uint32_t direction_rights(uint32_t direction) {
    switch (direction) {
    case 1:
        return KB2_GPU_SPAN_RIGHT_READ;
    case 2:
        return KB2_GPU_SPAN_RIGHT_WRITE;
    case 3:
        return KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE;
    default:
        return 0;
    }
}

static const kb2_gpu_span_contract_t *find_span_contract(
    const kb2_gpu_span_contract_t *contracts,
    size_t contract_count,
    uint32_t command_id,
    uint32_t argument_id,
    uint32_t record_id) {
    size_t index;

    for (index = 0; index < contract_count; ++index) {
        if (contracts[index].command_id == command_id &&
            contracts[index].argument_id == argument_id &&
            contracts[index].record_id == record_id) {
            return &contracts[index];
        }
    }
    return NULL;
}

static const kb2_gpu_attachment_contract_t *find_attachment_contract(
    const kb2_gpu_attachment_contract_t *contracts,
    size_t contract_count,
    uint32_t command_id,
    uint32_t argument_id,
    uint32_t object_class) {
    size_t index;

    for (index = 0; index < contract_count; ++index) {
        if (contracts[index].command_id == command_id &&
            contracts[index].argument_id == argument_id &&
            contracts[index].object_class == object_class) {
            return &contracts[index];
        }
    }
    return NULL;
}

static int generic_request_valid(const kb2_gpu_command_source_t *source) {
    const kb2_gpu_request_contract_t *requests;
    const kb2_gpu_request_contract_t *request;
    const kb2_gpu_span_contract_t *span_contracts;
    const kb2_gpu_attachment_contract_t *attachment_contracts;
    const kb2_gpu_argument_t *inline_argument;
    size_t request_count;
    size_t span_contract_count;
    size_t attachment_contract_count;
    size_t span_arguments = 0;
    size_t attachment_arguments = 0;
    size_t index;

    if (!request_catalog(source->command_set_id,
                         &requests,
                         &request_count,
                         &span_contracts,
                         &span_contract_count,
                         &attachment_contracts,
                         &attachment_contract_count) ||
        source->command_id == 0 || source->command_id > request_count) {
        return 0;
    }
    request = &requests[source->command_id - 1u];
    if (request->command_id != source->command_id ||
        source->span_count > request->span_capacity ||
        source->attachment_count > request->attachment_capacity) {
        return 0;
    }
    inline_argument = find_argument(source, 1);
    if (request->inline_record_id == 0) {
        if (inline_argument != NULL || source->inline_length != 0) {
            return 0;
        }
    } else if (inline_argument == NULL ||
               inline_argument->kind != KB2_GPU_ARGUMENT_INLINE ||
               inline_argument->flags != KB2_GPU_ARGUMENT_FLAG_INPUT ||
               inline_argument->record_schema_id != request->inline_record_id ||
               inline_argument->value != 0 || inline_argument->count != request->inline_size ||
               source->inline_length != request->inline_size) {
        return 0;
    }

    for (index = 0; index < source->argument_count; ++index) {
        const kb2_gpu_argument_t *argument = &source->arguments[index];

        if (argument->argument_id == 1 && request->inline_record_id != 0) {
            continue;
        }
        if (argument->kind == KB2_GPU_ARGUMENT_SPAN) {
            const kb2_gpu_span_contract_t *contract = find_span_contract(
                span_contracts,
                span_contract_count,
                source->command_id,
                argument->argument_id,
                argument->record_schema_id);
            const kb2_gpu_span_t *span;
            uint32_t flags;
            uint32_t rights;

            if (contract == NULL || argument->value > UINT32_MAX ||
                argument->count < contract->minimum || argument->count > contract->maximum) {
                return 0;
            }
            flags = direction_flags(contract->direction);
            rights = direction_rights(contract->direction);
            span = find_span(source->spans, source->span_count, (uint32_t)argument->value);
            if (flags == 0 || span == NULL || argument->flags != flags ||
                span->flags != flags || span->rights != rights ||
                argument->count > UINT64_MAX / contract->record_size ||
                span->length != (uint64_t)argument->count * contract->record_size) {
                return 0;
            }
            ++span_arguments;
        } else if (argument->kind == KB2_GPU_ARGUMENT_ATTACHMENT) {
            const kb2_gpu_attachment_t *attachment;
            const kb2_gpu_attachment_contract_t *contract;

            if (argument->value > UINT32_MAX) {
                return 0;
            }
            attachment = find_attachment(source->attachments,
                                         source->attachment_count,
                                         (uint32_t)argument->value);
            if (attachment == NULL) {
                return 0;
            }
            contract = find_attachment_contract(attachment_contracts,
                                                attachment_contract_count,
                                                source->command_id,
                                                argument->argument_id,
                                                attachment->object_class);
            if (contract == NULL || argument->flags != KB2_GPU_ARGUMENT_FLAG_INPUT ||
                attachment->role != contract->role ||
                attachment->ownership != contract->ownership) {
                return 0;
            }
            ++attachment_arguments;
        } else {
            return 0;
        }
    }
    if (span_arguments != source->span_count ||
        attachment_arguments != source->attachment_count ||
        source->argument_count != (request->inline_record_id == 0 ? 0u : 1u) +
                                      span_arguments + attachment_arguments) {
        return 0;
    }

    for (index = 0; index < span_contract_count; ++index) {
        size_t earlier;

        if (span_contracts[index].command_id != source->command_id ||
            span_contracts[index].minimum == 0) {
            continue;
        }
        for (earlier = 0; earlier < index; ++earlier) {
            if (span_contracts[earlier].command_id == source->command_id &&
                span_contracts[earlier].argument_id == span_contracts[index].argument_id) {
                break;
            }
        }
        if (earlier == index &&
            find_argument(source, span_contracts[index].argument_id) == NULL) {
            return 0;
        }
    }
    for (index = 0; index < attachment_contract_count; ++index) {
        size_t earlier;

        if (attachment_contracts[index].command_id != source->command_id ||
            attachment_contracts[index].optional != 0) {
            continue;
        }
        for (earlier = 0; earlier < index; ++earlier) {
            if (attachment_contracts[earlier].command_id == source->command_id &&
                attachment_contracts[earlier].argument_id ==
                    attachment_contracts[index].argument_id) {
                break;
            }
        }
        if (earlier == index &&
            find_argument(source, attachment_contracts[index].argument_id) == NULL) {
            return 0;
        }
    }
    return 1;
}

static uint32_t span_element_count(const kb2_gpu_command_source_t *source,
                                   uint32_t argument_id) {
    const kb2_gpu_argument_t *argument = find_argument(source, argument_id);

    return argument == NULL ? 0u : argument->count;
}

static int virtgpu_request_semantics_valid(const kb2_gpu_command_source_t *source) {
    const uint8_t *data = source->inline_data;

    switch (source->command_id) {
    case KB2_GPU_DRM_VIRTGPU_COMMAND_MAP:
        return (load_u32(data +
                         KB2_GPU_DRM_VIRTGPU_RECORD_MAP_REQUEST_MAPPING_RIGHTS_OFFSET) &
                ~(KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE)) == 0 &&
               load_u32(data +
                        KB2_GPU_DRM_VIRTGPU_RECORD_MAP_REQUEST_MAPPING_RIGHTS_OFFSET) != 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER: {
        uint32_t flags = load_u32(
            data + KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_FLAGS_OFFSET);
        int has_input_fence = find_argument(source, 6) != NULL;

        return (flags & ~(KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_FENCE_IN |
                          KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_FENCE_OUT |
                          KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_RING_INDEX)) == 0 &&
               ((flags & KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_FENCE_IN) != 0) ==
                   has_input_fence &&
               ((flags & KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_RING_INDEX) != 0 ||
                load_u32(data +
                         KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_RING_INDEX_OFFSET) == 0) &&
               load_u32(data +
                        KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_COMMAND_BYTES_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_BO_HANDLE_COUNT_OFFSET) ==
                   span_element_count(source, 3) &&
               load_u32(
                   data +
                   KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_INPUT_SYNCOBJ_COUNT_OFFSET) ==
                   span_element_count(source, 4) &&
               load_u32(
                   data +
                   KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_OUTPUT_SYNCOBJ_COUNT_OFFSET) ==
                   span_element_count(source, 5);
    }
    case KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_CREATE:
        return load_u32(data +
                        KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_INFO:
        return load_u32(data +
                        KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_INFO_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_WAIT:
        return (load_u32(data + KB2_GPU_DRM_VIRTGPU_RECORD_WAIT_REQUEST_FLAGS_OFFSET) &
                ~1u) == 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_GET_CAPS:
        return load_u32(data +
                        KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_REQUEST_RESPONSE_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_VIRTGPU_RECORD_GET_CAPS_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_RESOURCE_CREATE_BLOB:
        return load_u32(
                   data +
                   KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_BLOB_REQUEST_COMMAND_BYTES_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_VIRTGPU_RECORD_RESOURCE_CREATE_BLOB_REQUEST_RESERVED_OFFSET) ==
                   0;
    case KB2_GPU_DRM_VIRTGPU_COMMAND_CONTEXT_INIT: {
        uint32_t mask = load_u32(
            data + KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_PARAMETER_MASK_OFFSET);
        uint32_t debug_name_bytes = load_u32(
            data + KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_DEBUG_NAME_BYTES_OFFSET);

        return mask != 0 &&
               (mask & ~(KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_CAPSET_ID |
                         KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_RING_COUNT |
                         KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_POLL_RING_MASK |
                         KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_DEBUG_NAME)) == 0 &&
               debug_name_bytes ==
                   span_element_count(source, 2) &&
               (((mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_DEBUG_NAME) != 0 &&
                 debug_name_bytes != 0) ||
                ((mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_DEBUG_NAME) == 0 &&
                 debug_name_bytes == 0)) &&
               ((mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_CAPSET_ID) != 0 ||
                load_u32(data +
                         KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_CAPSET_ID_OFFSET) == 0) &&
               ((mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_RING_COUNT) != 0 ||
                load_u32(data +
                         KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_RING_COUNT_OFFSET) == 0) &&
               ((mask & KB2_GPU_DRM_VIRTGPU_CONTEXT_PARAMETER_POLL_RING_MASK) != 0 ||
                load_u64(data +
                         KB2_GPU_DRM_VIRTGPU_RECORD_CONTEXT_INIT_REQUEST_POLL_RING_MASK_OFFSET) ==
                    0);
    }
    default:
        return 1;
    }
}

static int core_request_semantics_valid(const kb2_gpu_command_source_t *source) {
    const uint8_t *data = source->inline_data;

    switch (source->command_id) {
    case KB2_GPU_DRM_CORE_COMMAND_VERSION:
        return load_u32(data + KB2_GPU_DRM_CORE_RECORD_VERSION_REQUEST_NAME_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data + KB2_GPU_DRM_CORE_RECORD_VERSION_REQUEST_DATE_CAPACITY_OFFSET) ==
                   span_element_count(source, 3) &&
               load_u32(
                   data + KB2_GPU_DRM_CORE_RECORD_VERSION_REQUEST_DESCRIPTION_CAPACITY_OFFSET) ==
                   span_element_count(source, 4) &&
               load_u32(data + KB2_GPU_DRM_CORE_RECORD_VERSION_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_GET_UNIQUE:
        return load_u32(data + KB2_GPU_DRM_CORE_RECORD_LENGTH_REQUEST_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data + KB2_GPU_DRM_CORE_RECORD_LENGTH_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_GET_CLIENT:
        return load_u32(data + KB2_GPU_DRM_CORE_RECORD_CLIENT_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_SET_CLIENT_NAME:
        return load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_CLIENT_NAME_REQUEST_NAME_BYTES_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_CLIENT_NAME_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_GEM_CLOSE:
    case KB2_GPU_DRM_CORE_COMMAND_GEM_FLINK:
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_DESTROY:
        return load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_GEM_HANDLE_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_GEM_OPEN:
        return load_u32(data + KB2_GPU_DRM_CORE_RECORD_GEM_OPEN_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_PRIME_ATTACHMENT_TO_HANDLE:
        return load_u32(data + KB2_GPU_DRM_CORE_RECORD_PRIME_IMPORT_REQUEST_FLAGS_OFFSET) == 0 &&
               load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_PRIME_IMPORT_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_CREATE:
        return load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_CREATE_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_ATTACHMENT_TO_HANDLE:
        return load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_IMPORT_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_WAIT:
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_TIMELINE_WAIT: {
        uint32_t flags = load_u32(
            data + KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST_FLAGS_OFFSET);

        return load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST_HANDLE_COUNT_OFFSET) ==
                   span_element_count(source, 2) &&
               (flags & ~15u) == 0 &&
               ((flags & 8u) != 0 ||
                load_u64(
                    data +
                    KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST_FENCE_DEADLINE_NS_OFFSET) == 0);
    }
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_RESET:
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_SIGNAL:
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_TIMELINE_SIGNAL:
        return load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_ARRAY_REQUEST_HANDLE_COUNT_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_ARRAY_REQUEST_FLAGS_OFFSET) == 0;
    case KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_QUERY:
        return load_u32(data +
                        KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_QUERY_REQUEST_HANDLE_COUNT_OFFSET) ==
                   span_element_count(source, 2) &&
               (load_u32(data +
                         KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_QUERY_REQUEST_FLAGS_OFFSET) &
                ~1u) == 0;
    default:
        return 1;
    }
}

static int mode_topology_required(uint32_t command_id) {
    switch (command_id) {
    case KB2_GPU_DRM_MODE_COMMAND_WAIT_VBLANK:
    case KB2_GPU_DRM_MODE_COMMAND_CRTC_GET_SEQUENCE:
    case KB2_GPU_DRM_MODE_COMMAND_CRTC_QUEUE_SEQUENCE:
    case KB2_GPU_DRM_MODE_COMMAND_GET_CRTC:
    case KB2_GPU_DRM_MODE_COMMAND_SET_CRTC:
    case KB2_GPU_DRM_MODE_COMMAND_CURSOR:
    case KB2_GPU_DRM_MODE_COMMAND_GET_GAMMA:
    case KB2_GPU_DRM_MODE_COMMAND_SET_GAMMA:
    case KB2_GPU_DRM_MODE_COMMAND_GET_ENCODER:
    case KB2_GPU_DRM_MODE_COMMAND_GET_CONNECTOR:
    case KB2_GPU_DRM_MODE_COMMAND_ATTACH_MODE:
    case KB2_GPU_DRM_MODE_COMMAND_DETACH_MODE:
    case KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY:
    case KB2_GPU_DRM_MODE_COMMAND_SET_PROPERTY:
    case KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY_BLOB:
    case KB2_GPU_DRM_MODE_COMMAND_GET_FB:
    case KB2_GPU_DRM_MODE_COMMAND_ADD_FB:
    case KB2_GPU_DRM_MODE_COMMAND_REMOVE_FB:
    case KB2_GPU_DRM_MODE_COMMAND_PAGE_FLIP:
    case KB2_GPU_DRM_MODE_COMMAND_DIRTY_FB:
    case KB2_GPU_DRM_MODE_COMMAND_GET_PLANE:
    case KB2_GPU_DRM_MODE_COMMAND_SET_PLANE:
    case KB2_GPU_DRM_MODE_COMMAND_ADD_FB2:
    case KB2_GPU_DRM_MODE_COMMAND_OBJECT_GET_PROPERTIES:
    case KB2_GPU_DRM_MODE_COMMAND_OBJECT_SET_PROPERTY:
    case KB2_GPU_DRM_MODE_COMMAND_CURSOR2:
    case KB2_GPU_DRM_MODE_COMMAND_ATOMIC:
    case KB2_GPU_DRM_MODE_COMMAND_CREATE_PROPERTY_BLOB:
    case KB2_GPU_DRM_MODE_COMMAND_DESTROY_PROPERTY_BLOB:
    case KB2_GPU_DRM_MODE_COMMAND_CREATE_LEASE:
    case KB2_GPU_DRM_MODE_COMMAND_GET_LEASE:
    case KB2_GPU_DRM_MODE_COMMAND_GET_FB2:
    case KB2_GPU_DRM_MODE_COMMAND_CLOSE_FB:
        return 1;
    default:
        return 0;
    }
}

static int mode_request_semantics_valid(const kb2_gpu_command_source_t *source) {
    const uint8_t *data = source->inline_data;

    if (mode_topology_required(source->command_id) && load_u64(data) == 0) {
        return 0;
    }
    switch (source->command_id) {
    case KB2_GPU_DRM_MODE_COMMAND_GET_RESOURCES:
        return load_u32(data + KB2_GPU_DRM_MODE_RECORD_RESOURCES_REQUEST_FB_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_RESOURCES_REQUEST_CRTC_CAPACITY_OFFSET) ==
                   span_element_count(source, 3) &&
               load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_RESOURCES_REQUEST_CONNECTOR_CAPACITY_OFFSET) ==
                   span_element_count(source, 4) &&
               load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_RESOURCES_REQUEST_ENCODER_CAPACITY_OFFSET) ==
                   span_element_count(source, 5);
    case KB2_GPU_DRM_MODE_COMMAND_GET_CRTC:
        return load_u32(data + KB2_GPU_DRM_MODE_RECORD_CRTC_GET_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_SET_CRTC:
        return load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_CRTC_SET_REQUEST_CONNECTOR_COUNT_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_CRTC_SET_REQUEST_MODE_VALID_OFFSET) ==
                   span_element_count(source, 3);
    case KB2_GPU_DRM_MODE_COMMAND_GET_GAMMA:
    case KB2_GPU_DRM_MODE_COMMAND_SET_GAMMA:
        return load_u32(data + KB2_GPU_DRM_MODE_RECORD_GAMMA_REQUEST_ENTRY_COUNT_OFFSET) ==
                   span_element_count(source, 2) &&
               span_element_count(source, 2) == span_element_count(source, 3) &&
               span_element_count(source, 2) == span_element_count(source, 4);
    case KB2_GPU_DRM_MODE_COMMAND_GET_CONNECTOR:
        return load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_CONNECTOR_REQUEST_MODE_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_CONNECTOR_REQUEST_PROPERTY_CAPACITY_OFFSET) ==
                   span_element_count(source, 3) &&
               load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_CONNECTOR_REQUEST_ENCODER_CAPACITY_OFFSET) ==
                   span_element_count(source, 4) &&
               (load_u32(data + KB2_GPU_DRM_MODE_RECORD_CONNECTOR_REQUEST_FLAGS_OFFSET) &
                ~KB2_GPU_DRM_MODE_CONNECTOR_FLAG_FORCE_PROBE) == 0 &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_CONNECTOR_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_CURSOR:
        return (load_u32(data + KB2_GPU_DRM_MODE_RECORD_CURSOR_REQUEST_FLAGS_OFFSET) &
                ~(KB2_GPU_DRM_MODE_CURSOR_FLAG_BUFFER |
                  KB2_GPU_DRM_MODE_CURSOR_FLAG_MOVE)) == 0 &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_CURSOR_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_CURSOR2:
        return (load_u32(data + KB2_GPU_DRM_MODE_RECORD_CURSOR2_REQUEST_FLAGS_OFFSET) &
                ~(KB2_GPU_DRM_MODE_CURSOR_FLAG_BUFFER |
                  KB2_GPU_DRM_MODE_CURSOR_FLAG_MOVE)) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY:
        return load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_PROPERTY_REQUEST_VALUE_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_PROPERTY_REQUEST_ENUM_CAPACITY_OFFSET) ==
                   span_element_count(source, 3) &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_PROPERTY_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_GET_PROPERTY_BLOB:
        return load_u32(data + KB2_GPU_DRM_MODE_RECORD_BLOB_GET_REQUEST_CAPACITY_OFFSET) ==
               span_element_count(source, 2);
    case KB2_GPU_DRM_MODE_COMMAND_DIRTY_FB:
        return load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_RECTANGLE_COUNT_OFFSET) ==
                   span_element_count(source, 2) &&
               (load_u32(data + KB2_GPU_DRM_MODE_RECORD_DIRTY_FB_REQUEST_FLAGS_OFFSET) &
                ~(KB2_GPU_DRM_MODE_DIRTY_FLAG_COPY |
                  KB2_GPU_DRM_MODE_DIRTY_FLAG_FILL)) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_PAGE_FLIP: {
        uint32_t flags =
            load_u32(data + KB2_GPU_DRM_MODE_RECORD_PAGE_FLIP_REQUEST_FLAGS_OFFSET);
        uint32_t target = flags &
                          (KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_ABSOLUTE |
                           KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_RELATIVE);

        return (flags & ~(KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_EVENT |
                          KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_ASYNC |
                          KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_ABSOLUTE |
                          KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_RELATIVE)) == 0 &&
               target != (KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_ABSOLUTE |
                          KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_TARGET_RELATIVE) &&
               (target != 0 ||
                load_u32(data + KB2_GPU_DRM_MODE_RECORD_PAGE_FLIP_REQUEST_SEQUENCE_OFFSET) == 0) &&
               ((flags & KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_EVENT) != 0 ||
                load_u64(data +
                         KB2_GPU_DRM_MODE_RECORD_PAGE_FLIP_REQUEST_EVENT_TOKEN_OFFSET) == 0);
    }
    case KB2_GPU_DRM_MODE_COMMAND_CREATE_DUMB:
        return load_u32(data + KB2_GPU_DRM_MODE_RECORD_DUMB_CREATE_REQUEST_FLAGS_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_MAP_DUMB:
        return (load_u32(data + KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST_MAPPING_RIGHTS_OFFSET) &
                ~(KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE)) == 0 &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_MAP_REQUEST_MAPPING_RIGHTS_OFFSET) != 0;
    case KB2_GPU_DRM_MODE_COMMAND_GET_PLANE_RESOURCES:
        return load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_PLANE_RESOURCES_REQUEST_PLANE_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_PLANE_RESOURCES_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_GET_PLANE:
        return load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_PLANE_GET_REQUEST_FORMAT_CAPACITY_OFFSET) ==
               span_element_count(source, 2);
    case KB2_GPU_DRM_MODE_COMMAND_OBJECT_GET_PROPERTIES:
        return load_u32(
                   data +
                   KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_PROPERTY_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_OBJECT_PROPERTIES_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_ATOMIC: {
        uint32_t flags =
            load_u32(data + KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_FLAGS_OFFSET);

        return (flags & ~(KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_EVENT |
                          KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_ASYNC |
                          KB2_GPU_DRM_MODE_ATOMIC_FLAG_TEST_ONLY |
                          KB2_GPU_DRM_MODE_ATOMIC_FLAG_NONBLOCK |
                          KB2_GPU_DRM_MODE_ATOMIC_FLAG_ALLOW_MODESET)) == 0 &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_OBJECT_COUNT_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_PROPERTY_COUNT_OFFSET) ==
                   span_element_count(source, 3) &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_FENCE_CAPACITY_OFFSET) ==
                   span_element_count(source, 4) &&
               ((flags & KB2_GPU_DRM_MODE_PAGE_FLIP_FLAG_EVENT) != 0 ||
                load_u64(data + KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_EVENT_TOKEN_OFFSET) == 0);
    }
    case KB2_GPU_DRM_MODE_COMMAND_ADD_FB2:
        return load_u32(data + KB2_GPU_DRM_MODE_RECORD_FB2_FB_ID_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_CREATE_PROPERTY_BLOB:
        return load_u32(data + KB2_GPU_DRM_MODE_RECORD_BLOB_CREATE_REQUEST_BYTES_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_BLOB_CREATE_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_CREATE_LEASE:
        return load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_LEASE_CREATE_REQUEST_OBJECT_COUNT_OFFSET) ==
               span_element_count(source, 2);
    case KB2_GPU_DRM_MODE_COMMAND_LIST_LESSEES:
        return load_u32(data + KB2_GPU_DRM_MODE_RECORD_CAPACITY_REQUEST_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_CAPACITY_REQUEST_RESERVED_OFFSET) == 0;
    case KB2_GPU_DRM_MODE_COMMAND_GET_LEASE:
        return load_u32(data +
                        KB2_GPU_DRM_MODE_RECORD_LEASE_GET_REQUEST_OBJECT_CAPACITY_OFFSET) ==
                   span_element_count(source, 2) &&
               load_u32(data + KB2_GPU_DRM_MODE_RECORD_LEASE_GET_REQUEST_RESERVED_OFFSET) == 0;
    default:
        return 1;
    }
}

static uint32_t amdgpu_record_size(uint32_t record_id) {
    switch (record_id) {
#define KB2_GPU_RECORD_SIZE_CASE(known_id, known_size)                                         \
    case known_id:                                                                             \
        return known_size;
        KB2_GPU_DRM_AMDGPU_RECORD_CATALOG(KB2_GPU_RECORD_SIZE_CASE)
#undef KB2_GPU_RECORD_SIZE_CASE
    default:
        return 0;
    }
}

static int cs_record(uint32_t record_id) {
    return record_id >= KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_IB &&
           record_id <= KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_GFX_SHADOW;
}

static int info_record(uint32_t record_id) {
    return record_id >= KB2_GPU_DRM_AMDGPU_RECORD_SCALAR_U32 &&
           record_id <= KB2_GPU_DRM_AMDGPU_RECORD_INFO_USERQ_METADATA;
}

static int handle_record(uint32_t record_id) {
    return record_id == KB2_GPU_DRM_AMDGPU_RECORD_HANDLE_U32;
}

static int gem_op_record(uint32_t record_id) {
    return record_id == KB2_GPU_DRM_AMDGPU_RECORD_GEM_CREATE_INFO ||
           record_id == KB2_GPU_DRM_AMDGPU_RECORD_GEM_VM_ENTRY;
}

static int userq_mqd_record(uint32_t record_id) {
    return record_id >= KB2_GPU_DRM_AMDGPU_RECORD_USERQ_MQD_GFX11 &&
           record_id <= KB2_GPU_DRM_AMDGPU_RECORD_USERQ_MQD_COMPUTE_GFX11;
}

static int timeline_record(uint32_t record_id) {
    return record_id == KB2_GPU_DRM_AMDGPU_RECORD_TIMELINE_SYNCOBJ;
}

static int userq_fence_record(uint32_t record_id) {
    return record_id == KB2_GPU_DRM_AMDGPU_RECORD_USERQ_FENCE_INFO;
}

static int bo_list_record(uint32_t record_id) {
    return record_id == KB2_GPU_DRM_AMDGPU_RECORD_BO_LIST_ENTRY;
}

static int fence_record(uint32_t record_id) {
    return record_id == KB2_GPU_DRM_AMDGPU_RECORD_FENCE;
}

static int gem_list_record(uint32_t record_id) {
    return record_id == KB2_GPU_DRM_AMDGPU_RECORD_GEM_LIST_HANDLES_ENTRY;
}

typedef int (*kb2_gpu_record_predicate_t)(uint32_t record_id);

static int amdgpu_span_valid(const kb2_gpu_command_source_t *source,
                             uint32_t argument_id,
                             uint32_t flags,
                             uint32_t minimum,
                             uint32_t maximum,
                             kb2_gpu_record_predicate_t record_predicate,
                             const kb2_gpu_span_t **span_out) {
    const kb2_gpu_argument_t *argument = find_argument(source, argument_id);
    const kb2_gpu_span_t *span;
    uint32_t size;

    *span_out = NULL;
    if (argument == NULL) {
        return minimum == 0;
    }
    if (argument->kind != KB2_GPU_ARGUMENT_SPAN || argument->flags != flags ||
        !record_predicate(argument->record_schema_id) || argument->count < minimum ||
        argument->count > maximum || argument->value > UINT32_MAX) {
        return 0;
    }
    span = find_span(source->spans, source->span_count, (uint32_t)argument->value);
    size = amdgpu_record_size(argument->record_schema_id);
    if (span == NULL || size == 0 || span->flags != flags ||
        span->rights != ((flags == KB2_GPU_ARGUMENT_FLAG_INPUT)
                            ? KB2_GPU_SPAN_RIGHT_READ
                            : KB2_GPU_SPAN_RIGHT_WRITE) ||
        argument->count > UINT64_MAX / size ||
        span->length != (uint64_t)argument->count * size) {
        return 0;
    }
    *span_out = span;
    return 1;
}

static int amdgpu_attachment_valid(const kb2_gpu_command_source_t *source,
                                   uint32_t argument_id,
                                   uint32_t object_class,
                                   uint32_t role,
                                   uint32_t ownership) {
    const kb2_gpu_argument_t *argument = find_argument(source, argument_id);
    const kb2_gpu_attachment_t *attachment;

    if (argument == NULL || argument->kind != KB2_GPU_ARGUMENT_ATTACHMENT ||
        argument->flags != KB2_GPU_ARGUMENT_FLAG_INPUT || argument->value > UINT32_MAX) {
        return 0;
    }
    attachment = find_attachment(source->attachments,
                                 source->attachment_count,
                                 (uint32_t)argument->value);
    return attachment != NULL && attachment->object_class == object_class &&
           attachment->role == role && attachment->ownership == ownership;
}

static int amdgpu_info_query_record_valid(uint32_t query, uint32_t record_id) {
    switch (query) {
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_ACCELERATION_WORKING:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_CRTC_FROM_ID:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_HW_IP_COUNT:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_SENSOR:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VRAM_LOST_COUNTER:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_MAXIMUM_IBS:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_SCALAR_U32;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_TIMESTAMP:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_BYTES_MOVED:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VRAM_USAGE:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_GTT_USAGE:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VISIBLE_VRAM_USAGE:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_EVICTION_COUNT:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VRAM_CPU_PAGE_FAULTS:
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_RAS_ENABLED_FEATURES:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_SCALAR_U64;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_HW_IP_INFO:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_HW_IP;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_FIRMWARE_VERSION:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_FIRMWARE;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_GDS_CONFIG:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_GDS;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VRAM_GTT:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_VRAM_GTT;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_READ_MMR_REGISTER:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_SCALAR_U32;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_DEVICE_INFO:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_MEMORY_INFO:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_MEMORY;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VCE_CLOCK_TABLE:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_VCE_CLOCK_ENTRY;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VBIOS:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_SCALAR_U32 ||
               record_id == KB2_GPU_DRM_AMDGPU_RECORD_BYTE ||
               record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_VBIOS;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VIDEO_HANDLE_COUNT:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_NUM_HANDLES;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_VIDEO_CAPS:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_VIDEO_CAPS;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_GPUVM_FAULT:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_GPUVM_FAULT;
    case KB2_GPU_DRM_AMDGPU_INFO_QUERY_USERQ_FIRMWARE_AREAS:
        return record_id == KB2_GPU_DRM_AMDGPU_RECORD_INFO_USERQ_METADATA;
    default:
        return 0;
    }
}

static int amdgpu_cs_spans_valid(const kb2_gpu_command_source_t *source) {
    size_t index;

    if (source->span_count < KB2_GPU_DRM_AMDGPU_COMMAND_CS_REQUEST_SPAN_CHUNKS_MINIMUM ||
        source->span_count > KB2_GPU_DRM_AMDGPU_COMMAND_CS_REQUEST_SPAN_CHUNKS_MAXIMUM ||
        source->argument_count != source->span_count + 1u ||
        source->attachment_count != 0) {
        return 0;
    }
    for (index = 0; index < source->span_count; ++index) {
        const kb2_gpu_argument_t *argument = &source->arguments[index + 1u];
        const kb2_gpu_span_t *span;
        uint32_t size;

        if (argument->argument_id != index + 2u || argument->kind != KB2_GPU_ARGUMENT_SPAN ||
            argument->flags != KB2_GPU_ARGUMENT_FLAG_INPUT ||
            argument->count <
                KB2_GPU_DRM_AMDGPU_COMMAND_CS_REQUEST_SPAN_CHUNKS_ELEMENT_MINIMUM ||
            argument->count >
                KB2_GPU_DRM_AMDGPU_COMMAND_CS_REQUEST_SPAN_CHUNKS_ELEMENT_MAXIMUM ||
            !cs_record(argument->record_schema_id) || argument->value > UINT32_MAX) {
            return 0;
        }
        span = find_span(source->spans, source->span_count, (uint32_t)argument->value);
        size = amdgpu_record_size(argument->record_schema_id);
        if (span == NULL || size == 0 || span->flags != KB2_GPU_SPAN_FLAG_INPUT ||
            span->rights != KB2_GPU_SPAN_RIGHT_READ ||
            argument->count > UINT64_MAX / size ||
            span->length != (uint64_t)argument->count * size) {
            return 0;
        }
    }
    return load_u32(source->inline_data +
                    KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST_CHUNK_COUNT_OFFSET) ==
           source->span_count;
}

static int amdgpu_request_valid(const kb2_gpu_command_source_t *source) {
    const kb2_gpu_inline_contract_t *contract;
    const kb2_gpu_argument_t *inline_argument;
    const kb2_gpu_span_t *spans[5] = {NULL, NULL, NULL, NULL, NULL};
    size_t span_count = 0;
    size_t attachment_count = 0;
    size_t index;

    if (source->command_id == 0 ||
        source->command_id > sizeof(kb2_gpu_amdgpu_request_contracts) /
                                 sizeof(kb2_gpu_amdgpu_request_contracts[0])) {
        return 0;
    }
    contract = &kb2_gpu_amdgpu_request_contracts[source->command_id - 1u];
    inline_argument = find_argument(source, KB2_GPU_DRM_AMDGPU_INLINE_ARGUMENT_ID);
    if (inline_argument == NULL || inline_argument->kind != KB2_GPU_ARGUMENT_INLINE ||
        inline_argument->flags != KB2_GPU_ARGUMENT_FLAG_INPUT ||
        inline_argument->record_schema_id != contract->record_id ||
        inline_argument->value != 0 || inline_argument->count != contract->size ||
        source->inline_length != contract->size) {
        return 0;
    }

    switch (source->command_id) {
    case KB2_GPU_DRM_AMDGPU_COMMAND_BO_LIST:
        if (!amdgpu_span_valid(source, 2, KB2_GPU_ARGUMENT_FLAG_INPUT, 0,
                               KB2_GPU_DRM_AMDGPU_MAX_BO_LIST_ENTRIES,
                               bo_list_record, &spans[0])) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_CS:
        return amdgpu_cs_spans_valid(source);
    case KB2_GPU_DRM_AMDGPU_COMMAND_INFO:
        if (!amdgpu_span_valid(source, 2, KB2_GPU_ARGUMENT_FLAG_OUTPUT, 1,
                               KB2_GPU_DRM_AMDGPU_MAX_INFO_BYTES,
                               info_record, &spans[0])) {
            return 0;
        }
        if (spans[0]->length > KB2_GPU_DRM_AMDGPU_MAX_INFO_BYTES ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_RESPONSE_RECORD_ID_OFFSET) !=
                spans[0]->record_schema_id ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_RESPONSE_CAPACITY_OFFSET) !=
                spans[0]->length ||
            !amdgpu_info_query_record_valid(
                load_u32(source->inline_data +
                         KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_QUERY_OFFSET),
                spans[0]->record_schema_id)) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_VA:
        if (!amdgpu_span_valid(source, 2, KB2_GPU_ARGUMENT_FLAG_INPUT, 0,
                               KB2_GPU_DRM_AMDGPU_MAX_SYNC_HANDLES,
                               handle_record, &spans[0])) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_OP:
        if (!amdgpu_span_valid(source, 2, KB2_GPU_ARGUMENT_FLAG_OUTPUT, 0,
                               KB2_GPU_DRM_AMDGPU_MAX_VM_MAPPINGS,
                               gem_op_record, &spans[0])) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_USERPTR:
        if (source->span_count != 0 || source->attachment_count != 1 ||
            !amdgpu_attachment_valid(source, 2, KB2_GPU_ATTACHMENT_MEMORY, 1,
                                     KB2_GPU_ATTACHMENT_SHARE)) {
            return 0;
        }
        attachment_count = 1;
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_WAIT_FENCES:
        if (!amdgpu_span_valid(source, 2, KB2_GPU_ARGUMENT_FLAG_INPUT, 1,
                               KB2_GPU_DRM_AMDGPU_MAX_SYNC_HANDLES,
                               fence_record, &spans[0])) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_SCHED:
        if (source->span_count != 0 || source->attachment_count != 1 ||
            !amdgpu_attachment_valid(source, 2, KB2_GPU_ATTACHMENT_SESSION, 1,
                                     KB2_GPU_ATTACHMENT_BORROW)) {
            return 0;
        }
        attachment_count = 1;
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_USERQ:
        if (!amdgpu_span_valid(source, 2, KB2_GPU_ARGUMENT_FLAG_INPUT, 0, 1,
                               userq_mqd_record, &spans[0])) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_USERQ_SIGNAL:
        for (index = 0; index < 3; ++index) {
            if (!amdgpu_span_valid(source, (uint32_t)index + 2u,
                                   KB2_GPU_ARGUMENT_FLAG_INPUT, 0,
                                   KB2_GPU_DRM_AMDGPU_MAX_SYNC_HANDLES,
                                   handle_record, &spans[index])) {
                return 0;
            }
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_USERQ_WAIT:
        if (!amdgpu_span_valid(source, 2, KB2_GPU_ARGUMENT_FLAG_INPUT, 0,
                               KB2_GPU_DRM_AMDGPU_MAX_SYNC_HANDLES,
                               handle_record, &spans[0]) ||
            !amdgpu_span_valid(source, 3, KB2_GPU_ARGUMENT_FLAG_INPUT, 0,
                               KB2_GPU_DRM_AMDGPU_MAX_SYNC_HANDLES,
                               timeline_record, &spans[1]) ||
            !amdgpu_span_valid(source, 4, KB2_GPU_ARGUMENT_FLAG_INPUT, 0,
                               KB2_GPU_DRM_AMDGPU_MAX_SYNC_HANDLES,
                               handle_record, &spans[2]) ||
            !amdgpu_span_valid(source, 5, KB2_GPU_ARGUMENT_FLAG_INPUT, 0,
                               KB2_GPU_DRM_AMDGPU_MAX_SYNC_HANDLES,
                               handle_record, &spans[3]) ||
            !amdgpu_span_valid(source, 6, KB2_GPU_ARGUMENT_FLAG_OUTPUT, 0,
                               KB2_GPU_DRM_AMDGPU_MAX_SYNC_HANDLES,
                               userq_fence_record, &spans[4])) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_LIST_HANDLES:
        if (!amdgpu_span_valid(source, 2, KB2_GPU_ARGUMENT_FLAG_OUTPUT, 0,
                               KB2_GPU_DRM_AMDGPU_MAX_BO_LIST_ENTRIES,
                               gem_list_record, &spans[0])) {
            return 0;
        }
        break;
    default:
        if (source->span_count != 0 || source->attachment_count != 0) {
            return 0;
        }
        break;
    }

    for (index = 0; index < sizeof(spans) / sizeof(spans[0]); ++index) {
        if (spans[index] != NULL) {
            ++span_count;
        }
    }
    switch (source->command_id) {
    case KB2_GPU_DRM_AMDGPU_COMMAND_BO_LIST:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_BO_LIST_REQUEST_ENTRY_COUNT_OFFSET) !=
                (spans[0] == NULL ? 0u : spans[0]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_BO_LIST_REQUEST_RESERVED_OFFSET) != 0) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_INFO:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_INFO_REQUEST_FLAGS_OFFSET) != 0) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_METADATA:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_REQUEST_DATA_SIZE_OFFSET) > 256u ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_GEM_METADATA_REQUEST_RESERVED_OFFSET) != 0) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_VA:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_GEM_VA_REQUEST_INPUT_SYNCOBJ_COUNT_OFFSET) !=
                (spans[0] == NULL ? 0u : spans[0]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_GEM_VA_REQUEST_RESERVED_OFFSET) != 0) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_OP:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_GEM_OP_REQUEST_MAPPING_CAPACITY_OFFSET) !=
                (spans[0] == NULL ? 0u : spans[0]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_GEM_OP_REQUEST_RESERVED_OFFSET) != 0) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_USERPTR:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_GEM_USERPTR_REQUEST_RESERVED_OFFSET) != 0) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_WAIT_FENCES:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_WAIT_FENCES_REQUEST_FENCE_COUNT_OFFSET) !=
            spans[0]->element_count) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_FENCE_TO_HANDLE:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_FENCE_TO_HANDLE_REQUEST_RESERVED_OFFSET) != 0) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_SCHED:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_SCHED_REQUEST_RESERVED_OFFSET) != 0) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_USERQ:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_REQUEST_MQD_RECORD_ID_OFFSET) !=
                (spans[0] == NULL ? 0u : spans[0]->record_schema_id) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_REQUEST_MQD_SIZE_OFFSET) !=
                (spans[0] == NULL ? 0u : spans[0]->length)) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_USERQ_SIGNAL:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_SIGNAL_REQUEST_SYNCOBJ_COUNT_OFFSET) !=
                (spans[0] == NULL ? 0u : spans[0]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_SIGNAL_REQUEST_READ_BO_COUNT_OFFSET) !=
                (spans[1] == NULL ? 0u : spans[1]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_SIGNAL_REQUEST_WRITE_BO_COUNT_OFFSET) !=
                (spans[2] == NULL ? 0u : spans[2]->element_count)) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_USERQ_WAIT:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_WAIT_REQUEST_SYNCOBJ_COUNT_OFFSET) !=
                (spans[0] == NULL ? 0u : spans[0]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_WAIT_REQUEST_TIMELINE_SYNCOBJ_COUNT_OFFSET) !=
                (spans[1] == NULL ? 0u : spans[1]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_WAIT_REQUEST_READ_BO_COUNT_OFFSET) !=
                (spans[2] == NULL ? 0u : spans[2]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_WAIT_REQUEST_WRITE_BO_COUNT_OFFSET) !=
                (spans[3] == NULL ? 0u : spans[3]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_USERQ_WAIT_REQUEST_OUTPUT_CAPACITY_OFFSET) !=
                (spans[4] == NULL ? 0u : spans[4]->element_count)) {
            return 0;
        }
        break;
    case KB2_GPU_DRM_AMDGPU_COMMAND_GEM_LIST_HANDLES:
        if (load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_GEM_LIST_HANDLES_REQUEST_ENTRY_CAPACITY_OFFSET) !=
                (spans[0] == NULL ? 0u : spans[0]->element_count) ||
            load_u32(source->inline_data +
                     KB2_GPU_DRM_AMDGPU_RECORD_GEM_LIST_HANDLES_REQUEST_RESERVED_OFFSET) != 0) {
            return 0;
        }
        break;
    default:
        break;
    }
    return source->span_count == span_count &&
           source->attachment_count == attachment_count &&
           source->argument_count == 1u + span_count + attachment_count;
}

static kb2_protocol_status_t validate_source(const kb2_gpu_command_source_t *source) {
    if (source == NULL || source->generation == 0 || source->session_id == 0 ||
        !profile_kind_valid(source->profile_kind) || source->flags != 0 ||
        source->argument_count > KB2_GPU_MAX_ARGUMENTS ||
        source->span_count > KB2_GPU_MAX_SPANS ||
        source->attachment_count > KB2_GPU_MAX_ATTACHMENTS ||
        source->inline_length > KB2_GPU_MAX_INLINE_BYTES ||
        (source->inline_length != 0 && source->inline_data == NULL) ||
        !command_allowed(source->profile_kind,
                         source->queue_class,
                         source->command_set_id,
                         source->command_id) ||
        (wait_command(source->command_set_id, source->command_id) !=
         (source->deadline_ns != 0)) ||
        !regions_valid(source->regions, source->region_count) ||
        !spans_valid(source->spans,
                     source->span_count,
                     source->regions,
                     source->region_count) ||
        !attachments_valid(source->attachments,
                           source->attachment_count,
                           source->generation) ||
        !arguments_valid(source)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (source->command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID &&
        !amdgpu_request_valid(source)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (source->command_set_id != KB2_GPU_DRM_AMDGPU_SET_ID &&
        !generic_request_valid(source)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (source->command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID &&
        !virtgpu_request_semantics_valid(source)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (source->command_set_id == KB2_GPU_DRM_CORE_SET_ID &&
        !core_request_semantics_valid(source)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    if (source->command_set_id == KB2_GPU_DRM_MODE_SET_ID &&
        !mode_request_semantics_valid(source)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_gpu_command_encoded_size(const kb2_gpu_command_source_t *source,
                                                    size_t *size_out) {
    size_t size;

    if (size_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    *size_out = 0;
    if (validate_source(source) != KB2_PROTOCOL_OK) {
        return source == NULL ? KB2_PROTOCOL_INVALID_ARGUMENT : KB2_PROTOCOL_MALFORMED;
    }
    size = KB2_GPU_COMMAND_HEADER_SIZE;
    if (!size_add_multiply(size,
                           source->argument_count,
                           KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE,
                           &size) ||
        !size_add_multiply(size,
                           source->span_count,
                           KB2_GPU_SPAN_DESCRIPTOR_SIZE,
                           &size) ||
        !size_add_multiply(size,
                           source->attachment_count,
                           KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE,
                           &size) ||
        source->inline_length > SIZE_MAX - size) {
        return KB2_PROTOCOL_OVERFLOW;
    }
    size += source->inline_length;
    if (size > UINT32_MAX) {
        return KB2_PROTOCOL_OVERFLOW;
    }
    *size_out = size;
    return KB2_PROTOCOL_OK;
}

static void encode_argument(uint8_t *destination, const kb2_gpu_argument_t *argument) {
    store_u32(destination + KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET,
              argument->argument_id);
    store_u32(destination + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET, argument->kind);
    store_u32(destination + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET, argument->flags);
    store_u32(destination + KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET,
              argument->record_schema_id);
    store_u64(destination + KB2_GPU_ARGUMENT_DESCRIPTOR_VALUE_OFFSET, argument->value);
    store_u32(destination + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET, argument->count);
}

static void encode_span(uint8_t *destination, const kb2_gpu_span_t *span) {
    store_u32(destination + KB2_GPU_SPAN_DESCRIPTOR_SPAN_ID_OFFSET, span->span_id);
    store_u32(destination + KB2_GPU_SPAN_DESCRIPTOR_REGION_ID_OFFSET, span->region_id);
    store_u64(destination + KB2_GPU_SPAN_DESCRIPTOR_OFFSET_OFFSET, span->offset);
    store_u64(destination + KB2_GPU_SPAN_DESCRIPTOR_LENGTH_OFFSET, span->length);
    store_u32(destination + KB2_GPU_SPAN_DESCRIPTOR_RIGHTS_OFFSET, span->rights);
    store_u32(destination + KB2_GPU_SPAN_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET,
              span->record_schema_id);
    store_u32(destination + KB2_GPU_SPAN_DESCRIPTOR_ELEMENT_COUNT_OFFSET,
              span->element_count);
    store_u32(destination + KB2_GPU_SPAN_DESCRIPTOR_FLAGS_OFFSET, span->flags);
}

static void encode_attachment(uint8_t *destination,
                              const kb2_gpu_attachment_t *attachment) {
    store_u32(destination + KB2_GPU_ATTACHMENT_DESCRIPTOR_ATTACHMENT_ID_OFFSET,
              attachment->attachment_id);
    store_u32(destination + KB2_GPU_ATTACHMENT_DESCRIPTOR_OBJECT_CLASS_OFFSET,
              attachment->object_class);
    store_u64(destination + KB2_GPU_ATTACHMENT_DESCRIPTOR_EXCHANGE_ID_OFFSET,
              attachment->exchange_id);
    store_u64(destination + KB2_GPU_ATTACHMENT_DESCRIPTOR_GENERATION_OFFSET,
              attachment->generation);
    store_u64(destination + KB2_GPU_ATTACHMENT_DESCRIPTOR_RIGHTS_OFFSET,
              attachment->rights);
    store_u32(destination + KB2_GPU_ATTACHMENT_DESCRIPTOR_ROLE_OFFSET, attachment->role);
    store_u32(destination + KB2_GPU_ATTACHMENT_DESCRIPTOR_OWNERSHIP_OFFSET,
              attachment->ownership);
    store_u32(destination + KB2_GPU_ATTACHMENT_DESCRIPTOR_FLAGS_OFFSET, attachment->flags);
}

kb2_protocol_status_t kb2_gpu_command_encode(uint8_t *buffer,
                                              size_t buffer_size,
                                              size_t *encoded_size_out,
                                              const kb2_gpu_command_source_t *source) {
    size_t size;
    size_t argument_offset;
    size_t span_offset;
    size_t attachment_offset;
    size_t inline_offset;
    size_t index;
    kb2_protocol_status_t status;

    if (buffer == NULL || encoded_size_out == NULL) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    *encoded_size_out = 0;
    status = kb2_gpu_command_encoded_size(source, &size);
    if (status != KB2_PROTOCOL_OK) {
        return status;
    }
    if (buffer_size < size) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    argument_offset = KB2_GPU_COMMAND_HEADER_SIZE;
    span_offset = argument_offset + source->argument_count * KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    attachment_offset = span_offset + source->span_count * KB2_GPU_SPAN_DESCRIPTOR_SIZE;
    inline_offset =
        attachment_offset + source->attachment_count * KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE;
    memset(buffer, 0, size);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_TOTAL_SIZE_OFFSET, (uint32_t)size);
    memcpy(buffer + KB2_GPU_COMMAND_HEADER_ABI_IDENTITY_OFFSET,
           kb2_gpu_abi_identity,
           sizeof(kb2_gpu_abi_identity));
    memcpy(buffer + KB2_GPU_COMMAND_HEADER_SCHEMA_DIGEST_OFFSET,
           kb2_gpu_schema_digest,
           sizeof(kb2_gpu_schema_digest));
    store_u64(buffer + KB2_GPU_COMMAND_HEADER_GENERATION_OFFSET, source->generation);
    store_u64(buffer + KB2_GPU_COMMAND_HEADER_SESSION_ID_OFFSET, source->session_id);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_COMMAND_SET_ID_OFFSET,
              source->command_set_id);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_COMMAND_ID_OFFSET, source->command_id);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_FLAGS_OFFSET, source->flags);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_ARGUMENT_COUNT_OFFSET,
              (uint32_t)source->argument_count);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_SPAN_COUNT_OFFSET,
              (uint32_t)source->span_count);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_ATTACHMENT_COUNT_OFFSET,
              (uint32_t)source->attachment_count);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_INLINE_LENGTH_OFFSET,
              (uint32_t)source->inline_length);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET,
              (uint32_t)argument_offset);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_SPAN_TABLE_OFFSET_OFFSET,
              (uint32_t)span_offset);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET,
              (uint32_t)attachment_offset);
    store_u32(buffer + KB2_GPU_COMMAND_HEADER_INLINE_OFFSET_OFFSET,
              (uint32_t)inline_offset);
    store_u64(buffer + KB2_GPU_COMMAND_HEADER_DEADLINE_NS_OFFSET, source->deadline_ns);
    for (index = 0; index < source->argument_count; ++index) {
        encode_argument(buffer + argument_offset + index * KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE,
                        &source->arguments[index]);
    }
    for (index = 0; index < source->span_count; ++index) {
        encode_span(buffer + span_offset + index * KB2_GPU_SPAN_DESCRIPTOR_SIZE,
                    &source->spans[index]);
    }
    for (index = 0; index < source->attachment_count; ++index) {
        encode_attachment(
            buffer + attachment_offset + index * KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE,
            &source->attachments[index]);
    }
    if (source->inline_length != 0) {
        memcpy(buffer + inline_offset, source->inline_data, source->inline_length);
    }
    *encoded_size_out = size;
    return KB2_PROTOCOL_OK;
}

static void decode_argument(const uint8_t *source, kb2_gpu_argument_t *argument_out) {
    memset(argument_out, 0, sizeof(*argument_out));
    argument_out->argument_id =
        load_u32(source + KB2_GPU_ARGUMENT_DESCRIPTOR_ARGUMENT_ID_OFFSET);
    argument_out->kind = load_u32(source + KB2_GPU_ARGUMENT_DESCRIPTOR_KIND_OFFSET);
    argument_out->flags = load_u32(source + KB2_GPU_ARGUMENT_DESCRIPTOR_FLAGS_OFFSET);
    argument_out->record_schema_id =
        load_u32(source + KB2_GPU_ARGUMENT_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET);
    argument_out->value = load_u64(source + KB2_GPU_ARGUMENT_DESCRIPTOR_VALUE_OFFSET);
    argument_out->count = load_u32(source + KB2_GPU_ARGUMENT_DESCRIPTOR_COUNT_OFFSET);
}

static void decode_span(const uint8_t *source, kb2_gpu_span_t *span_out) {
    memset(span_out, 0, sizeof(*span_out));
    span_out->span_id = load_u32(source + KB2_GPU_SPAN_DESCRIPTOR_SPAN_ID_OFFSET);
    span_out->region_id = load_u32(source + KB2_GPU_SPAN_DESCRIPTOR_REGION_ID_OFFSET);
    span_out->offset = load_u64(source + KB2_GPU_SPAN_DESCRIPTOR_OFFSET_OFFSET);
    span_out->length = load_u64(source + KB2_GPU_SPAN_DESCRIPTOR_LENGTH_OFFSET);
    span_out->rights = load_u32(source + KB2_GPU_SPAN_DESCRIPTOR_RIGHTS_OFFSET);
    span_out->record_schema_id =
        load_u32(source + KB2_GPU_SPAN_DESCRIPTOR_RECORD_SCHEMA_ID_OFFSET);
    span_out->element_count =
        load_u32(source + KB2_GPU_SPAN_DESCRIPTOR_ELEMENT_COUNT_OFFSET);
    span_out->flags = load_u32(source + KB2_GPU_SPAN_DESCRIPTOR_FLAGS_OFFSET);
}

static void decode_attachment(const uint8_t *source,
                              kb2_gpu_attachment_t *attachment_out) {
    memset(attachment_out, 0, sizeof(*attachment_out));
    attachment_out->attachment_id =
        load_u32(source + KB2_GPU_ATTACHMENT_DESCRIPTOR_ATTACHMENT_ID_OFFSET);
    attachment_out->object_class =
        load_u32(source + KB2_GPU_ATTACHMENT_DESCRIPTOR_OBJECT_CLASS_OFFSET);
    attachment_out->exchange_id =
        load_u64(source + KB2_GPU_ATTACHMENT_DESCRIPTOR_EXCHANGE_ID_OFFSET);
    attachment_out->generation =
        load_u64(source + KB2_GPU_ATTACHMENT_DESCRIPTOR_GENERATION_OFFSET);
    attachment_out->rights =
        load_u64(source + KB2_GPU_ATTACHMENT_DESCRIPTOR_RIGHTS_OFFSET);
    attachment_out->role = load_u32(source + KB2_GPU_ATTACHMENT_DESCRIPTOR_ROLE_OFFSET);
    attachment_out->ownership =
        load_u32(source + KB2_GPU_ATTACHMENT_DESCRIPTOR_OWNERSHIP_OFFSET);
    attachment_out->flags = load_u32(source + KB2_GPU_ATTACHMENT_DESCRIPTOR_FLAGS_OFFSET);
}

static int command_offsets_valid(const uint8_t *buffer, size_t buffer_size) {
    size_t expected = KB2_GPU_COMMAND_HEADER_SIZE;
    uint32_t argument_count =
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_ARGUMENT_COUNT_OFFSET);
    uint32_t span_count = load_u32(buffer + KB2_GPU_COMMAND_HEADER_SPAN_COUNT_OFFSET);
    uint32_t attachment_count =
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_ATTACHMENT_COUNT_OFFSET);
    uint32_t inline_length =
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_INLINE_LENGTH_OFFSET);

    if (argument_count > KB2_GPU_MAX_ARGUMENTS || span_count > KB2_GPU_MAX_SPANS ||
        attachment_count > KB2_GPU_MAX_ATTACHMENTS ||
        inline_length > KB2_GPU_MAX_INLINE_BYTES ||
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET) != expected ||
        !size_add_multiply(expected,
                           argument_count,
                           KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE,
                           &expected) ||
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_SPAN_TABLE_OFFSET_OFFSET) != expected ||
        !size_add_multiply(expected, span_count, KB2_GPU_SPAN_DESCRIPTOR_SIZE, &expected) ||
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET) != expected ||
        !size_add_multiply(expected,
                           attachment_count,
                           KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE,
                           &expected) ||
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_INLINE_OFFSET_OFFSET) != expected ||
        inline_length > SIZE_MAX - expected) {
        return 0;
    }
    expected += inline_length;
    return expected == buffer_size &&
           load_u32(buffer + KB2_GPU_COMMAND_HEADER_TOTAL_SIZE_OFFSET) == buffer_size;
}

kb2_protocol_status_t kb2_gpu_command_decode(const uint8_t *buffer,
                                              size_t buffer_size,
                                              uint64_t expected_generation,
                                              uint32_t profile_kind,
                                              uint32_t queue_class,
                                              const kb2_gpu_region_t *regions,
                                              size_t region_count,
                                              kb2_gpu_command_t *command_out) {
    kb2_gpu_argument_t arguments[KB2_GPU_MAX_ARGUMENTS];
    kb2_gpu_span_t spans[KB2_GPU_MAX_SPANS];
    kb2_gpu_attachment_t attachments[KB2_GPU_MAX_ATTACHMENTS];
    kb2_gpu_command_source_t source;
    kb2_gpu_command_t command;
    size_t index;

    if (buffer == NULL || command_out == NULL || expected_generation == 0 ||
        !profile_kind_valid(profile_kind)) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    if (buffer_size < KB2_GPU_COMMAND_HEADER_SIZE) {
        return KB2_PROTOCOL_BUFFER_TOO_SMALL;
    }
    if (memcmp(buffer + KB2_GPU_COMMAND_HEADER_ABI_IDENTITY_OFFSET,
               kb2_gpu_abi_identity,
               sizeof(kb2_gpu_abi_identity)) != 0 ||
        memcmp(buffer + KB2_GPU_COMMAND_HEADER_SCHEMA_DIGEST_OFFSET,
               kb2_gpu_schema_digest,
               sizeof(kb2_gpu_schema_digest)) != 0) {
        return KB2_PROTOCOL_SCHEMA_MISMATCH;
    }
    if (!zero_bytes(buffer + KB2_GPU_COMMAND_HEADER_RESERVED_0_OFFSET, sizeof(uint32_t)) ||
        !zero_bytes(buffer + KB2_GPU_COMMAND_HEADER_RESERVED_1_OFFSET, 16) ||
        !command_offsets_valid(buffer, buffer_size)) {
        return KB2_PROTOCOL_MALFORMED;
    }
    memset(&command, 0, sizeof(command));
    command.bytes = buffer;
    command.size = buffer_size;
    command.generation = load_u64(buffer + KB2_GPU_COMMAND_HEADER_GENERATION_OFFSET);
    command.session_id = load_u64(buffer + KB2_GPU_COMMAND_HEADER_SESSION_ID_OFFSET);
    command.command_set_id =
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_COMMAND_SET_ID_OFFSET);
    command.command_id = load_u32(buffer + KB2_GPU_COMMAND_HEADER_COMMAND_ID_OFFSET);
    command.flags = load_u32(buffer + KB2_GPU_COMMAND_HEADER_FLAGS_OFFSET);
    command.deadline_ns = load_u64(buffer + KB2_GPU_COMMAND_HEADER_DEADLINE_NS_OFFSET);
    command.counts[0] = load_u32(buffer + KB2_GPU_COMMAND_HEADER_ARGUMENT_COUNT_OFFSET);
    command.counts[1] = load_u32(buffer + KB2_GPU_COMMAND_HEADER_SPAN_COUNT_OFFSET);
    command.counts[2] = load_u32(buffer + KB2_GPU_COMMAND_HEADER_ATTACHMENT_COUNT_OFFSET);
    command.offsets[0] =
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_ARGUMENT_TABLE_OFFSET_OFFSET);
    command.offsets[1] = load_u32(buffer + KB2_GPU_COMMAND_HEADER_SPAN_TABLE_OFFSET_OFFSET);
    command.offsets[2] =
        load_u32(buffer + KB2_GPU_COMMAND_HEADER_ATTACHMENT_TABLE_OFFSET_OFFSET);
    command.offsets[3] = load_u32(buffer + KB2_GPU_COMMAND_HEADER_INLINE_OFFSET_OFFSET);
    command.inline_length = load_u32(buffer + KB2_GPU_COMMAND_HEADER_INLINE_LENGTH_OFFSET);
    for (index = 0; index < command.counts[0]; ++index) {
        decode_argument(buffer + command.offsets[0] +
                            index * KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE,
                        &arguments[index]);
        if (!zero_bytes(buffer + command.offsets[0] +
                            index * KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE +
                            KB2_GPU_ARGUMENT_DESCRIPTOR_RESERVED_OFFSET,
                        sizeof(uint32_t))) {
            return KB2_PROTOCOL_MALFORMED;
        }
    }
    for (index = 0; index < command.counts[1]; ++index) {
        decode_span(buffer + command.offsets[1] + index * KB2_GPU_SPAN_DESCRIPTOR_SIZE,
                    &spans[index]);
    }
    for (index = 0; index < command.counts[2]; ++index) {
        decode_attachment(
            buffer + command.offsets[2] + index * KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE,
            &attachments[index]);
        if (!zero_bytes(buffer + command.offsets[2] +
                            index * KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE +
                            KB2_GPU_ATTACHMENT_DESCRIPTOR_RESERVED_OFFSET,
                        sizeof(uint32_t))) {
            return KB2_PROTOCOL_MALFORMED;
        }
    }
    memset(&source, 0, sizeof(source));
    source.generation = command.generation;
    source.session_id = command.session_id;
    source.profile_kind = profile_kind;
    source.queue_class = queue_class;
    source.command_set_id = command.command_set_id;
    source.command_id = command.command_id;
    source.flags = command.flags;
    source.deadline_ns = command.deadline_ns;
    source.arguments = arguments;
    source.argument_count = command.counts[0];
    source.spans = spans;
    source.span_count = command.counts[1];
    source.attachments = attachments;
    source.attachment_count = command.counts[2];
    source.inline_data = buffer + command.offsets[3];
    source.inline_length = command.inline_length;
    source.regions = regions;
    source.region_count = region_count;
    if (command.generation != expected_generation || validate_source(&source) != KB2_PROTOCOL_OK) {
        return KB2_PROTOCOL_MALFORMED;
    }
    *command_out = command;
    return KB2_PROTOCOL_OK;
}

size_t kb2_gpu_command_argument_count(const kb2_gpu_command_t *command) {
    return command == NULL ? 0 : command->counts[0];
}

size_t kb2_gpu_command_span_count(const kb2_gpu_command_t *command) {
    return command == NULL ? 0 : command->counts[1];
}

size_t kb2_gpu_command_attachment_count(const kb2_gpu_command_t *command) {
    return command == NULL ? 0 : command->counts[2];
}

kb2_protocol_status_t kb2_gpu_command_argument(const kb2_gpu_command_t *command,
                                                size_t index,
                                                kb2_gpu_argument_t *argument_out) {
    if (command == NULL || argument_out == NULL || index >= command->counts[0]) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    decode_argument(command->bytes + command->offsets[0] +
                        index * KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE,
                    argument_out);
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_gpu_command_span(const kb2_gpu_command_t *command,
                                            size_t index,
                                            kb2_gpu_span_t *span_out) {
    if (command == NULL || span_out == NULL || index >= command->counts[1]) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    decode_span(command->bytes + command->offsets[1] +
                    index * KB2_GPU_SPAN_DESCRIPTOR_SIZE,
                span_out);
    return KB2_PROTOCOL_OK;
}

kb2_protocol_status_t kb2_gpu_command_attachment(
    const kb2_gpu_command_t *command,
    size_t index,
    kb2_gpu_attachment_t *attachment_out) {
    if (command == NULL || attachment_out == NULL || index >= command->counts[2]) {
        return KB2_PROTOCOL_INVALID_ARGUMENT;
    }
    decode_attachment(command->bytes + command->offsets[2] +
                          index * KB2_GPU_ATTACHMENT_DESCRIPTOR_SIZE,
                      attachment_out);
    return KB2_PROTOCOL_OK;
}

const uint8_t *kb2_gpu_command_inline_data(const kb2_gpu_command_t *command,
                                           size_t *length_out) {
    if (command == NULL || length_out == NULL) {
        return NULL;
    }
    *length_out = command->inline_length;
    return command->bytes + command->offsets[3];
}
