/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/gpu.h>
#include <kobox2/sha256.h>

#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

#define COUNT_INLINE(record_id, record_size) +1
enum {
    core_completion_contract_count =
        0 KB2_GPU_DRM_CORE_COMPLETION_INLINE_CATALOG(COUNT_INLINE),
    mode_completion_contract_count =
        0 KB2_GPU_DRM_MODE_COMPLETION_INLINE_CATALOG(COUNT_INLINE),
    virtgpu_completion_contract_count =
        0 KB2_GPU_DRM_VIRTGPU_COMPLETION_INLINE_CATALOG(COUNT_INLINE),
    amdgpu_completion_contract_count =
        0 KB2_GPU_DRM_AMDGPU_COMPLETION_INLINE_CATALOG(COUNT_INLINE),
};
#undef COUNT_INLINE

typedef struct command_vector {
    kb2_gpu_command_source_t source;
    kb2_gpu_region_t regions[2];
    kb2_gpu_argument_t arguments[8];
    kb2_gpu_span_t spans[4];
    kb2_gpu_attachment_t attachments[2];
    uint8_t inline_data[64];
} command_vector_t;

static const uint8_t virgl_vector_digest[KB2_SHA256_DIGEST_SIZE] = {
    0x10, 0xad, 0x62, 0xf5, 0xc5, 0x62, 0x2d, 0x49, 0x9e, 0x7d, 0x26,
    0x53, 0x91, 0x2e, 0xd0, 0xb0, 0x62, 0xaf, 0xe3, 0xa6, 0x01, 0x0b,
    0x69, 0xbf, 0x2f, 0xb8, 0x24, 0x86, 0xf5, 0x6c, 0xab, 0x60,
};

static const uint8_t amdgpu_vector_digest[KB2_SHA256_DIGEST_SIZE] = {
    0x34, 0x9d, 0x69, 0xa7, 0x3e, 0x1b, 0xc1, 0x80, 0x6b, 0xde, 0xe5,
    0xc8, 0xd8, 0x89, 0xec, 0xfa, 0x62, 0x69, 0xaf, 0x43, 0xb8, 0x99,
    0x99, 0xd6, 0x2a, 0x1d, 0x7b, 0xd6, 0x4d, 0x38, 0x02, 0x41,
};

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

static void make_virgl_vector(command_vector_t *vector) {
    memset(vector, 0, sizeof(*vector));
    vector->regions[0] = (kb2_gpu_region_t){
        .region_id = 7,
        .rights = KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE,
        .length = 4096,
    };
    vector->spans[0] = (kb2_gpu_span_t){
        .span_id = 1,
        .region_id = 7,
        .offset = 0,
        .length = 64,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_BYTE,
        .element_count = 64,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector->spans[1] = (kb2_gpu_span_t){
        .span_id = 2,
        .region_id = 7,
        .offset = 128,
        .length = 16,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_HANDLE_U32,
        .element_count = 4,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector->spans[2] = (kb2_gpu_span_t){
        .span_id = 3,
        .region_id = 7,
        .offset = 256,
        .length = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ,
        .element_count = 1,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector->spans[3] = (kb2_gpu_span_t){
        .span_id = 4,
        .region_id = 7,
        .offset = 384,
        .length = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ_SIZE,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ,
        .element_count = 1,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector->attachments[0] = (kb2_gpu_attachment_t){
        .attachment_id = 1,
        .object_class = KB2_GPU_ATTACHMENT_SYNC_FILE,
        .exchange_id = 55,
        .generation = 9,
        .rights = 1,
        .role = 1,
        .ownership = KB2_GPU_ATTACHMENT_SHARE,
        .flags = KB2_GPU_ATTACHMENT_FLAG_INPUT,
    };
    vector->arguments[0] = (kb2_gpu_argument_t){
        .argument_id = KB2_GPU_DRM_VIRTGPU_INLINE_ARGUMENT_ID,
        .kind = KB2_GPU_ARGUMENT_INLINE,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST,
        .count = KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_SIZE,
    };
    vector->arguments[1] = (kb2_gpu_argument_t){
        .argument_id = 2,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_BYTE,
        .value = 1,
        .count = 64,
    };
    vector->arguments[2] = (kb2_gpu_argument_t){
        .argument_id = 3,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_HANDLE_U32,
        .value = 2,
        .count = 4,
    };
    vector->arguments[3] = (kb2_gpu_argument_t){
        .argument_id = 4,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ,
        .value = 3,
        .count = 1,
    };
    vector->arguments[4] = (kb2_gpu_argument_t){
        .argument_id = 5,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_VIRTGPU_RECORD_EXEC_SYNCOBJ,
        .value = 4,
        .count = 1,
    };
    vector->arguments[5] = (kb2_gpu_argument_t){
        .argument_id = 6,
        .kind = KB2_GPU_ARGUMENT_ATTACHMENT,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .value = 1,
        .count = 1,
    };
    store_u32(vector->inline_data +
                  KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_FLAGS_OFFSET,
              KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_FENCE_IN |
                  KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_RING_INDEX);
    store_u32(vector->inline_data +
                  KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_RING_INDEX_OFFSET,
              2);
    store_u32(vector->inline_data +
                  KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_COMMAND_BYTES_OFFSET,
              64);
    store_u32(vector->inline_data +
                  KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_BO_HANDLE_COUNT_OFFSET,
              4);
    store_u32(vector->inline_data +
                  KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_INPUT_SYNCOBJ_COUNT_OFFSET,
              1);
    store_u32(vector->inline_data +
                  KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_OUTPUT_SYNCOBJ_COUNT_OFFSET,
              1);
    vector->source = (kb2_gpu_command_source_t){
        .generation = 9,
        .session_id = 21,
        .profile_kind = KB2_GPU_PROFILE_VIRGL,
        .queue_class = KB2_GPU_QUEUE_EXECUTION,
        .command_set_id = KB2_GPU_DRM_VIRTGPU_SET_ID,
        .command_id = KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER,
        .arguments = vector->arguments,
        .argument_count = 6,
        .spans = vector->spans,
        .span_count = 4,
        .attachments = vector->attachments,
        .attachment_count = 1,
        .inline_data = vector->inline_data,
        .inline_length = KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_SIZE,
        .regions = vector->regions,
        .region_count = 1,
    };
}

static void make_amdgpu_vector(command_vector_t *vector) {
    memset(vector, 0, sizeof(*vector));
    vector->regions[0] = (kb2_gpu_region_t){
        .region_id = 9,
        .rights = KB2_GPU_SPAN_RIGHT_READ | KB2_GPU_SPAN_RIGHT_WRITE,
        .length = 8192,
    };
    vector->spans[0] = (kb2_gpu_span_t){
        .span_id = 1,
        .region_id = 9,
        .offset = 0,
        .length = KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_IB_SIZE,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_IB,
        .element_count = 1,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector->spans[1] = (kb2_gpu_span_t){
        .span_id = 2,
        .region_id = 9,
        .offset = 64,
        .length = KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_DEPENDENCY_SIZE,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_DEPENDENCY,
        .element_count = 1,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector->spans[2] = (kb2_gpu_span_t){
        .span_id = 3,
        .region_id = 9,
        .offset = 128,
        .length = KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_TIMELINE_WAIT_SIZE,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_TIMELINE_WAIT,
        .element_count = 1,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector->arguments[0] = (kb2_gpu_argument_t){
        .argument_id = 1,
        .kind = KB2_GPU_ARGUMENT_INLINE,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST,
        .value = 0,
        .count = KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST_SIZE,
    };
    vector->arguments[1] = (kb2_gpu_argument_t){
        .argument_id = 2,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_IB,
        .value = 1,
        .count = 1,
    };
    vector->arguments[2] = (kb2_gpu_argument_t){
        .argument_id = 3,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_DEPENDENCY,
        .value = 2,
        .count = 1,
    };
    vector->arguments[3] = (kb2_gpu_argument_t){
        .argument_id = 4,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_TIMELINE_WAIT,
        .value = 3,
        .count = 1,
    };
    store_u32(vector->inline_data +
                  KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST_CONTEXT_ID_OFFSET,
              31);
    store_u32(vector->inline_data +
                  KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST_BO_LIST_HANDLE_OFFSET,
              7);
    store_u32(vector->inline_data +
                  KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST_CHUNK_COUNT_OFFSET,
              3);
    vector->source = (kb2_gpu_command_source_t){
        .generation = 11,
        .session_id = 41,
        .profile_kind = KB2_GPU_PROFILE_AMDGPU,
        .queue_class = KB2_GPU_QUEUE_EXECUTION,
        .command_set_id = KB2_GPU_DRM_AMDGPU_SET_ID,
        .command_id = KB2_GPU_DRM_AMDGPU_COMMAND_CS,
        .arguments = vector->arguments,
        .argument_count = 4,
        .spans = vector->spans,
        .span_count = 3,
        .inline_data = vector->inline_data,
        .inline_length = KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST_SIZE,
        .regions = vector->regions,
        .region_count = 1,
    };
}

static int encode_vector(command_vector_t *vector,
                         uint8_t *buffer,
                         size_t capacity,
                         size_t *size_out) {
    return kb2_gpu_command_encode(buffer, capacity, size_out, &vector->source) ==
           KB2_PROTOCOL_OK;
}

static int test_profile_catalogs(void) {
    kb2_gpu_command_set_t virgl[3];
    kb2_gpu_command_set_t amdgpu[3];
    size_t index;

    CHECK(kb2_gpu_profile_command_set_count(KB2_GPU_PROFILE_VIRGL) == 3);
    CHECK(kb2_gpu_profile_command_set_count(KB2_GPU_PROFILE_AMDGPU) == 3);
    CHECK(kb2_gpu_profile_command_set_count(99) == 0);
    for (index = 0; index < 3; ++index) {
        CHECK(kb2_gpu_profile_command_set(KB2_GPU_PROFILE_VIRGL, index, &virgl[index]) ==
              KB2_PROTOCOL_OK);
        CHECK(kb2_gpu_profile_command_set(KB2_GPU_PROFILE_AMDGPU, index, &amdgpu[index]) ==
              KB2_PROTOCOL_OK);
    }
    CHECK(virgl[0].set_id == KB2_GPU_DRM_CORE_SET_ID);
    CHECK(virgl[1].set_id == KB2_GPU_DRM_MODE_SET_ID);
    CHECK(virgl[2].set_id == KB2_GPU_DRM_VIRTGPU_SET_ID);
    CHECK(virgl[0].command_count == 26);
    CHECK(virgl[1].command_count == 44);
    CHECK(amdgpu[0].set_id == virgl[0].set_id);
    CHECK(amdgpu[1].set_id == virgl[1].set_id);
    CHECK(memcmp(amdgpu[0].schema_digest,
                 virgl[0].schema_digest,
                 sizeof(virgl[0].schema_digest)) == 0);
    CHECK(memcmp(amdgpu[1].schema_digest,
                 virgl[1].schema_digest,
                 sizeof(virgl[1].schema_digest)) == 0);
    CHECK(amdgpu[2].set_id == KB2_GPU_DRM_AMDGPU_SET_ID);
    CHECK(memcmp(amdgpu[2].schema_digest,
                 virgl[2].schema_digest,
                 sizeof(virgl[2].schema_digest)) != 0);
    CHECK(virgl[2].command_count == 11);
    CHECK(amdgpu[2].command_count == 20);
    CHECK(KB2_GPU_DRM_CORE_RECORD_COUNT == 33);
    CHECK(KB2_GPU_DRM_MODE_RECORD_COUNT == 66);
    CHECK(KB2_GPU_DRM_VIRTGPU_RECORD_COUNT == 20);
    CHECK(KB2_GPU_DRM_AMDGPU_RECORD_COUNT == 72);
    CHECK(core_completion_contract_count == KB2_GPU_DRM_CORE_COMMAND_COUNT);
    CHECK(mode_completion_contract_count == KB2_GPU_DRM_MODE_COMMAND_COUNT);
    CHECK(virtgpu_completion_contract_count == KB2_GPU_DRM_VIRTGPU_COMMAND_COUNT);
    CHECK(amdgpu_completion_contract_count == KB2_GPU_DRM_AMDGPU_COMMAND_COUNT);
    CHECK(KB2_GPU_DRM_AMDGPU_INFO_QUERY_COUNT == 27);
    CHECK(KB2_GPU_DRM_AMDGPU_CS_CHUNK_COUNT == 10);
    CHECK(KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_SYNCOBJ_IN_UAPI_CHUNK_ID == 4);
    CHECK(KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_SYNCOBJ_OUT_UAPI_CHUNK_ID == 5);
    CHECK(KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_SCHEDULED_DEPENDENCY_UAPI_CHUNK_ID == 7);
    CHECK(KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_TIMELINE_WAIT_UAPI_CHUNK_ID == 8);
    CHECK(KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_TIMELINE_SIGNAL_UAPI_CHUNK_ID == 9);
    CHECK(KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE_SIZE == 436);
    CHECK(KB2_GPU_DRM_AMDGPU_RECORD_USERQ_REQUEST_SIZE == 64);
    CHECK(KB2_GPU_DRM_MODE_RECORD_MODE_INFO_SIZE == 68);
    CHECK(KB2_GPU_DRM_MODE_RECORD_ATOMIC_PROPERTY_SIZE == 32);
    CHECK(KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_SIZE == 24);
    CHECK(KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER_REQUEST_SPAN_COUNT == 4);
    CHECK(KB2_GPU_DRM_AMDGPU_COMMAND_FENCE_TO_HANDLE_COMPLETION_ATTACHMENT_NATIVE_SYNC_CLASS_0 ==
          KB2_GPU_ATTACHMENT_SYNCOBJ);
    CHECK(KB2_GPU_DRM_AMDGPU_COMMAND_FENCE_TO_HANDLE_COMPLETION_ATTACHMENT_NATIVE_SYNC_CLASS_1 ==
          KB2_GPU_ATTACHMENT_SYNC_FILE);
    return 0;
}

static int test_virgl_vector(void) {
    command_vector_t vector;
    kb2_gpu_command_t command;
    kb2_gpu_argument_t argument;
    kb2_gpu_span_t span;
    kb2_gpu_attachment_t attachment;
    uint8_t first[1024];
    uint8_t second[1024];
    const uint8_t *inline_data;
    size_t first_size;
    size_t second_size;
    size_t inline_length;
    uint8_t digest[KB2_SHA256_DIGEST_SIZE];
    uint8_t schema_digest[KB2_GPU_SCHEMA_DIGEST_SIZE];

    make_virgl_vector(&vector);
    CHECK(encode_vector(&vector, first, sizeof(first), &first_size));
    CHECK(encode_vector(&vector, second, sizeof(second), &second_size));
    CHECK(first_size == 552);
    CHECK(first_size == second_size && memcmp(first, second, first_size) == 0);
    kb2_sha256(first, first_size, digest);
    CHECK(memcmp(digest, virgl_vector_digest, sizeof(digest)) == 0);
    CHECK(kb2_gpu_copy_schema_digest(schema_digest, sizeof(schema_digest)) ==
          KB2_PROTOCOL_OK);
    CHECK(memcmp(first + KB2_GPU_COMMAND_HEADER_SCHEMA_DIGEST_OFFSET,
                 schema_digest,
                 sizeof(schema_digest)) == 0);
    CHECK(kb2_gpu_command_decode(first,
                                 first_size,
                                 9,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_OK);
    CHECK(kb2_gpu_command_decode(first,
                                 first_size,
                                 9,
                                 KB2_GPU_PROFILE_AMDGPU,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);
    CHECK(command.generation == 9 && command.session_id == 21);
    CHECK(command.command_set_id == KB2_GPU_DRM_VIRTGPU_SET_ID);
    CHECK(command.command_id == KB2_GPU_DRM_VIRTGPU_COMMAND_EXECBUFFER);
    CHECK(kb2_gpu_command_argument_count(&command) == 6);
    CHECK(kb2_gpu_command_span_count(&command) == 4);
    CHECK(kb2_gpu_command_attachment_count(&command) == 1);
    CHECK(kb2_gpu_command_argument(&command, 1, &argument) == KB2_PROTOCOL_OK);
    CHECK(argument.kind == KB2_GPU_ARGUMENT_SPAN && argument.value == 1);
    CHECK(kb2_gpu_command_span(&command, 1, &span) == KB2_PROTOCOL_OK);
    CHECK(span.region_id == 7 && span.offset == 128 && span.length == 16);
    CHECK(kb2_gpu_command_attachment(&command, 0, &attachment) == KB2_PROTOCOL_OK);
    CHECK(attachment.exchange_id == 55 && attachment.generation == 9);
    inline_data = kb2_gpu_command_inline_data(&command, &inline_length);
    CHECK(inline_data != NULL &&
          inline_length == KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_SIZE);
    CHECK(inline_data[KB2_GPU_DRM_VIRTGPU_RECORD_EXECBUFFER_REQUEST_FLAGS_OFFSET] ==
          (KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_FENCE_IN |
           KB2_GPU_DRM_VIRTGPU_EXEC_FLAG_RING_INDEX));
    return 0;
}

static int test_amdgpu_vector(void) {
    command_vector_t vector;
    kb2_gpu_command_t command;
    kb2_gpu_argument_t argument;
    kb2_gpu_span_t span;
    uint8_t buffer[1024];
    size_t size;
    uint8_t digest[KB2_SHA256_DIGEST_SIZE];
    uint8_t schema_digest[KB2_GPU_SCHEMA_DIGEST_SIZE];

    make_amdgpu_vector(&vector);
    CHECK(encode_vector(&vector, buffer, sizeof(buffer), &size));
    CHECK(size == 392);
    kb2_sha256(buffer, size, digest);
    CHECK(memcmp(digest, amdgpu_vector_digest, sizeof(digest)) == 0);
    CHECK(kb2_gpu_copy_schema_digest(schema_digest, sizeof(schema_digest)) ==
          KB2_PROTOCOL_OK);
    CHECK(memcmp(buffer + KB2_GPU_COMMAND_HEADER_SCHEMA_DIGEST_OFFSET,
                 schema_digest,
                 sizeof(schema_digest)) == 0);
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 11,
                                 KB2_GPU_PROFILE_AMDGPU,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_OK);
    CHECK(command.command_set_id == KB2_GPU_DRM_AMDGPU_SET_ID);
    CHECK(command.command_id == KB2_GPU_DRM_AMDGPU_COMMAND_CS);
    CHECK(kb2_gpu_command_argument_count(&command) == 4);
    CHECK(kb2_gpu_command_span_count(&command) == 3);
    CHECK(kb2_gpu_command_attachment_count(&command) == 0);
    CHECK(kb2_gpu_command_argument(&command, 0, &argument) == KB2_PROTOCOL_OK);
    CHECK(argument.kind == KB2_GPU_ARGUMENT_INLINE);
    CHECK(argument.record_schema_id == KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST);
    CHECK(kb2_gpu_command_span(&command, 2, &span) == KB2_PROTOCOL_OK);
    CHECK(span.record_schema_id == KB2_GPU_DRM_AMDGPU_RECORD_CS_CHUNK_TIMELINE_WAIT);
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 11,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);
    return 0;
}

static int test_wire_rejection(void) {
    command_vector_t vector;
    kb2_gpu_command_t command;
    uint8_t buffer[1024];
    size_t size;
    const size_t span_offset = KB2_GPU_COMMAND_HEADER_SIZE +
                               6u * KB2_GPU_ARGUMENT_DESCRIPTOR_SIZE;
    const size_t attachment_offset = span_offset + 4u * KB2_GPU_SPAN_DESCRIPTOR_SIZE;

    make_virgl_vector(&vector);
    CHECK(encode_vector(&vector, buffer, sizeof(buffer), &size));
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 8,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);

    store_u64(buffer + KB2_GPU_COMMAND_HEADER_GENERATION_OFFSET, 8);
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 9,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);
    store_u64(buffer + KB2_GPU_COMMAND_HEADER_GENERATION_OFFSET, 9);

    store_u64(buffer + attachment_offset + KB2_GPU_ATTACHMENT_DESCRIPTOR_GENERATION_OFFSET, 8);
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 9,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);
    store_u64(buffer + attachment_offset + KB2_GPU_ATTACHMENT_DESCRIPTOR_GENERATION_OFFSET, 9);

    store_u32(buffer + attachment_offset + KB2_GPU_ATTACHMENT_DESCRIPTOR_OBJECT_CLASS_OFFSET,
              0);
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 9,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);
    store_u32(buffer + attachment_offset + KB2_GPU_ATTACHMENT_DESCRIPTOR_OBJECT_CLASS_OFFSET,
              KB2_GPU_ATTACHMENT_SYNC_FILE);

    store_u64(buffer + span_offset + KB2_GPU_SPAN_DESCRIPTOR_OFFSET_OFFSET, 4090);
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 9,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);
    store_u64(buffer + span_offset + KB2_GPU_SPAN_DESCRIPTOR_OFFSET_OFFSET, 0);

    store_u32(buffer + span_offset + KB2_GPU_SPAN_DESCRIPTOR_RIGHTS_OFFSET,
              KB2_GPU_SPAN_RIGHT_WRITE);
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 9,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);
    store_u32(buffer + span_offset + KB2_GPU_SPAN_DESCRIPTOR_RIGHTS_OFFSET,
              KB2_GPU_SPAN_RIGHT_READ);

    buffer[KB2_GPU_COMMAND_HEADER_RESERVED_1_OFFSET] = 1;
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 9,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);
    buffer[KB2_GPU_COMMAND_HEADER_RESERVED_1_OFFSET] = 0;
    buffer[KB2_GPU_COMMAND_HEADER_SCHEMA_DIGEST_OFFSET] ^= 1u;
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 9,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_SCHEMA_MISMATCH);
    buffer[KB2_GPU_COMMAND_HEADER_SCHEMA_DIGEST_OFFSET] ^= 1u;
    CHECK(kb2_gpu_command_decode(buffer,
                                 size - 1u,
                                 9,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_MALFORMED);
    return 0;
}

static int test_source_rejection(void) {
    command_vector_t vector;
    size_t size;

    make_virgl_vector(&vector);
    vector.spans[1].offset = 32;
    vector.spans[1].rights = KB2_GPU_SPAN_RIGHT_WRITE;
    vector.spans[1].flags = KB2_GPU_SPAN_FLAG_OUTPUT;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_virgl_vector(&vector);
    vector.attachments[0].generation = 8;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_virgl_vector(&vector);
    vector.arguments[3].value = 2;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_virgl_vector(&vector);
    vector.source.command_id = KB2_GPU_DRM_VIRTGPU_COMMAND_COUNT + 1u;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_virgl_vector(&vector);
    vector.source.queue_class = KB2_GPU_QUEUE_DISPLAY;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_virgl_vector(&vector);
    vector.source.deadline_ns = 1;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_virgl_vector(&vector);
    vector.source.command_id = KB2_GPU_DRM_VIRTGPU_COMMAND_WAIT;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);
    vector.source.deadline_ns = 1000;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_amdgpu_vector(&vector);
    vector.spans[0].record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE;
    vector.spans[0].length = KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE_SIZE;
    vector.arguments[1].record_schema_id = KB2_GPU_DRM_AMDGPU_RECORD_INFO_DEVICE;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_amdgpu_vector(&vector);
    store_u32(vector.inline_data +
                  KB2_GPU_DRM_AMDGPU_RECORD_CS_REQUEST_CHUNK_COUNT_OFFSET,
              2);
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_amdgpu_vector(&vector);
    vector.spans[2].rights = KB2_GPU_SPAN_RIGHT_WRITE;
    vector.spans[2].flags = KB2_GPU_SPAN_FLAG_OUTPUT;
    vector.arguments[3].flags = KB2_GPU_ARGUMENT_FLAG_OUTPUT;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    make_amdgpu_vector(&vector);
    vector.arguments[3].argument_id = 5;
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);
    return 0;
}

static int test_common_command_contracts(void) {
    command_vector_t vector;
    kb2_gpu_command_t command;
    uint8_t buffer[1024];
    size_t size;

    memset(&vector, 0, sizeof(vector));
    vector.regions[0] = (kb2_gpu_region_t){
        .region_id = 1,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .length = 4096,
    };
    vector.spans[0] = (kb2_gpu_span_t){
        .span_id = 1,
        .region_id = 1,
        .length = 2u * KB2_GPU_DRM_CORE_RECORD_HANDLE_U32_SIZE,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_CORE_RECORD_HANDLE_U32,
        .element_count = 2,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector.arguments[0] = (kb2_gpu_argument_t){
        .argument_id = KB2_GPU_DRM_CORE_INLINE_ARGUMENT_ID,
        .kind = KB2_GPU_ARGUMENT_INLINE,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST,
        .count = KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST_SIZE,
    };
    vector.arguments[1] = (kb2_gpu_argument_t){
        .argument_id = 2,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_CORE_RECORD_HANDLE_U32,
        .value = 1,
        .count = 2,
    };
    store_u32(vector.inline_data +
                  KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST_HANDLE_COUNT_OFFSET,
              2);
    vector.source = (kb2_gpu_command_source_t){
        .generation = 4,
        .session_id = 8,
        .profile_kind = KB2_GPU_PROFILE_VIRGL,
        .queue_class = KB2_GPU_QUEUE_EXECUTION,
        .command_set_id = KB2_GPU_DRM_CORE_SET_ID,
        .command_id = KB2_GPU_DRM_CORE_COMMAND_SYNCOBJ_WAIT,
        .deadline_ns = 100,
        .arguments = vector.arguments,
        .argument_count = 2,
        .spans = vector.spans,
        .span_count = 1,
        .inline_data = vector.inline_data,
        .inline_length = KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST_SIZE,
        .regions = vector.regions,
        .region_count = 1,
    };
    CHECK(encode_vector(&vector, buffer, sizeof(buffer), &size));
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 4,
                                 KB2_GPU_PROFILE_VIRGL,
                                 KB2_GPU_QUEUE_EXECUTION,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_OK);
    store_u32(vector.inline_data +
                  KB2_GPU_DRM_CORE_RECORD_SYNCOBJ_WAIT_REQUEST_HANDLE_COUNT_OFFSET,
              1);
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);

    memset(&vector, 0, sizeof(vector));
    vector.regions[0] = (kb2_gpu_region_t){
        .region_id = 2,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .length = 4096,
    };
    vector.spans[0] = (kb2_gpu_span_t){
        .span_id = 1,
        .region_id = 2,
        .length = KB2_GPU_DRM_MODE_RECORD_ATOMIC_OBJECT_SIZE,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_MODE_RECORD_ATOMIC_OBJECT,
        .element_count = 1,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector.spans[1] = (kb2_gpu_span_t){
        .span_id = 2,
        .region_id = 2,
        .offset = 64,
        .length = 2u * KB2_GPU_DRM_MODE_RECORD_ATOMIC_PROPERTY_SIZE,
        .rights = KB2_GPU_SPAN_RIGHT_READ,
        .record_schema_id = KB2_GPU_DRM_MODE_RECORD_ATOMIC_PROPERTY,
        .element_count = 2,
        .flags = KB2_GPU_SPAN_FLAG_INPUT,
    };
    vector.arguments[0] = (kb2_gpu_argument_t){
        .argument_id = KB2_GPU_DRM_MODE_INLINE_ARGUMENT_ID,
        .kind = KB2_GPU_ARGUMENT_INLINE,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST,
        .count = KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_SIZE,
    };
    vector.arguments[1] = (kb2_gpu_argument_t){
        .argument_id = 2,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_MODE_RECORD_ATOMIC_OBJECT,
        .value = 1,
        .count = 1,
    };
    vector.arguments[2] = (kb2_gpu_argument_t){
        .argument_id = 3,
        .kind = KB2_GPU_ARGUMENT_SPAN,
        .flags = KB2_GPU_ARGUMENT_FLAG_INPUT,
        .record_schema_id = KB2_GPU_DRM_MODE_RECORD_ATOMIC_PROPERTY,
        .value = 2,
        .count = 2,
    };
    store_u64(vector.inline_data + KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_TOPOLOGY_EPOCH_OFFSET,
              7);
    store_u32(vector.inline_data + KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_OBJECT_COUNT_OFFSET,
              1);
    store_u32(vector.inline_data + KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_PROPERTY_COUNT_OFFSET,
              2);
    vector.source = (kb2_gpu_command_source_t){
        .generation = 5,
        .session_id = 9,
        .profile_kind = KB2_GPU_PROFILE_AMDGPU,
        .queue_class = KB2_GPU_QUEUE_DISPLAY,
        .command_set_id = KB2_GPU_DRM_MODE_SET_ID,
        .command_id = KB2_GPU_DRM_MODE_COMMAND_ATOMIC,
        .arguments = vector.arguments,
        .argument_count = 3,
        .spans = vector.spans,
        .span_count = 2,
        .inline_data = vector.inline_data,
        .inline_length = KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_SIZE,
        .regions = vector.regions,
        .region_count = 1,
    };
    CHECK(encode_vector(&vector, buffer, sizeof(buffer), &size));
    CHECK(kb2_gpu_command_decode(buffer,
                                 size,
                                 5,
                                 KB2_GPU_PROFILE_AMDGPU,
                                 KB2_GPU_QUEUE_DISPLAY,
                                 vector.regions,
                                 1,
                                 &command) == KB2_PROTOCOL_OK);
    store_u64(vector.inline_data + KB2_GPU_DRM_MODE_RECORD_ATOMIC_REQUEST_TOPOLOGY_EPOCH_OFFSET,
              0);
    CHECK(kb2_gpu_command_encoded_size(&vector.source, &size) == KB2_PROTOCOL_MALFORMED);
    return 0;
}

static int test_inline_completion(void) {
    uint8_t buffer[256], corrupted[256], data[8] = {1, 2, 3};
    kb2_gpu_inline_completion_t source = {.session_id = 9, .status = KB2_GPU_STATUS_OK,
        .record_schema_id = KB2_GPU_DRM_CORE_RECORD_SCALAR_U64, .data = data, .length = sizeof(data)};
    kb2_gpu_inline_completion_t decoded, sentinel;
    size_t size = 123;

    CHECK(kb2_gpu_inline_completion_encode(buffer, 1, &size, &source) == KB2_PROTOCOL_BUFFER_TOO_SMALL);
    CHECK(size == 123);
    CHECK(kb2_gpu_inline_completion_encode(buffer, sizeof(buffer), &size, &source) == KB2_PROTOCOL_OK);
    CHECK(kb2_gpu_inline_completion_decode(buffer, size, 9, &decoded) == KB2_PROTOCOL_OK);
    CHECK(decoded.length == sizeof(data) && decoded.record_schema_id == source.record_schema_id);
    CHECK(!memcmp(decoded.data, data, sizeof(data)));
    memset(&sentinel, 0xa5, sizeof(sentinel));
    for (size_t truncated = 0; truncated < size; ++truncated) {
        decoded = sentinel;
        CHECK(kb2_gpu_inline_completion_decode(buffer, truncated, 9, &decoded) != KB2_PROTOCOL_OK);
        CHECK(!memcmp(&decoded, &sentinel, sizeof(decoded)));
    }
    CHECK(kb2_gpu_inline_completion_decode(buffer, size, 10, &decoded) == KB2_PROTOCOL_MALFORMED);
    const size_t bad_offsets[] = {KB2_GPU_COMPLETION_HEADER_SPAN_COUNT_OFFSET,
        KB2_GPU_COMPLETION_HEADER_ATTACHMENT_COUNT_OFFSET, KB2_GPU_COMPLETION_HEADER_RESERVED_OFFSET,
        KB2_GPU_COMPLETION_HEADER_DETAIL_CODE_OFFSET, KB2_GPU_COMPLETION_HEADER_INLINE_OFFSET_OFFSET,
        KB2_GPU_COMPLETION_HEADER_SIZE + KB2_GPU_ARGUMENT_DESCRIPTOR_RESERVED_OFFSET,
        KB2_GPU_COMPLETION_HEADER_SIZE + KB2_GPU_ARGUMENT_DESCRIPTOR_VALUE_OFFSET};
    for (size_t index = 0; index < sizeof(bad_offsets) / sizeof(bad_offsets[0]); ++index) {
        memcpy(corrupted, buffer, size);
        corrupted[bad_offsets[index]] ^= 1;
        CHECK(kb2_gpu_inline_completion_decode(corrupted, size, 9, &decoded) == KB2_PROTOCOL_MALFORMED);
    }
    source.status = KB2_GPU_STATUS_INVALID;
    CHECK(kb2_gpu_inline_completion_encode(buffer, sizeof(buffer), &size, &source) == KB2_PROTOCOL_MALFORMED);
    source.length = source.record_schema_id = 0;
    CHECK(kb2_gpu_inline_completion_encode(buffer, sizeof(buffer), &size, &source) == KB2_PROTOCOL_OK);
    CHECK(size == KB2_GPU_COMPLETION_HEADER_SIZE);
    CHECK(kb2_gpu_inline_completion_decode(buffer, size, 9, &decoded) == KB2_PROTOCOL_OK);
    CHECK(decoded.status == KB2_GPU_STATUS_INVALID && !decoded.length && !decoded.data);
    return 0;
}

int main(void) {
    CHECK(test_profile_catalogs() == 0);
    CHECK(test_virgl_vector() == 0);
    CHECK(test_amdgpu_vector() == 0);
    CHECK(test_wire_rejection() == 0);
    CHECK(test_source_rejection() == 0);
    CHECK(test_common_command_contracts() == 0);
    CHECK(test_inline_completion() == 0);
    return 0;
}
