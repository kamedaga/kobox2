/* SPDX-License-Identifier: Apache-2.0 */
#include <kobox2/filesystem.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void request_codec(void) {
    uint8_t bytes[8192] = {0};
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_OPENAT2,
        .generation = 3, .sequence = 7, .client = 9, .credential_generation = 2,
        .offset = INT64_MIN, .length = UINT64_MAX, .umask = 0027,
        .path = {.offset = KB2_FILESYSTEM_REQUEST_SIZE, .length = 4096},
        .groups = {.offset = 4288, .length = 16}};
    memset(bytes + request.path.offset, 'x', 4095);
    assert(!kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &request));
    kb2_fs_request_t decoded = {0};
    assert(!kb2_fs_request_decode(bytes, sizeof(bytes), sizeof(bytes), &decoded));
    assert(decoded.length == UINT64_MAX && decoded.offset == INT64_MIN);
    assert(decoded.client == 9 && decoded.credential_generation == 2);
    assert(!kb2_fs_request_validate_paths(bytes, sizeof(bytes), &decoded));
    bytes[KB2_FILESYSTEM_REQUEST_RESERVED_OFFSET] = 1;
    assert(kb2_fs_request_decode(bytes, sizeof(bytes), sizeof(bytes), &decoded));
    bytes[KB2_FILESYSTEM_REQUEST_RESERVED_OFFSET] = 0;
    assert(kb2_fs_request_decode(bytes, KB2_FILESYSTEM_REQUEST_SIZE - 1,
                                  sizeof(bytes), &decoded));
    bytes[request.path.offset + 1] = 0;
    assert(kb2_fs_request_validate_paths(bytes, sizeof(bytes), &request));
    bytes[request.path.offset + 1] = 'x';
    bytes[request.path.offset + request.path.length - 1] = 'x';
    assert(kb2_fs_request_validate_paths(bytes, sizeof(bytes), &request));
    request.data = request.path;
    assert(kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &request));
    request.data = (kb2_fs_span_t){.offset = UINT64_MAX - 2, .length = 4};
    assert(kb2_fs_request_encode(bytes, sizeof(bytes), UINT64_MAX, &request));
    request.data = (kb2_fs_span_t){0};
    request.groups.offset++;
    assert(kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &request));
    request.groups.offset--;
    request.groups.length++;
    assert(kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &request));
    request.groups.length--;
    request.path = (kb2_fs_span_t){.offset = 192, .length = 0};
    assert(kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &request));
    request.path = (kb2_fs_span_t){0};
    request.generation = 0;
    assert(kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &request));
    request.generation = 3;
    request.sequence = 0;
    assert(kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &request));
    request.sequence = 7;
    request.umask = 01000;
    assert(kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &request));
}

static void response_codec(void) {
    uint8_t bytes[KB2_FILESYSTEM_RESPONSE_SIZE + 32];
    kb2_fs_request_t request = {.opcode = KB2_FILESYSTEM_OP_READ, .generation = 3, .sequence = 7};
    kb2_fs_response_t response = {.opcode = request.opcode, .generation = 3,
        .sequence = 7, .result = 32, .data_length = 32}, decoded;
    assert(!kb2_fs_response_encode(bytes, sizeof(bytes), &response));
    assert(!kb2_fs_response_decode(bytes, sizeof(bytes), &decoded));
    assert(kb2_fs_response_matches(&decoded, &request));
    assert(kb2_fs_response_decode(bytes, sizeof(bytes) - 1, &decoded));
    bytes[KB2_FILESYSTEM_RESPONSE_RESERVED_OFFSET] = 1;
    assert(kb2_fs_response_decode(bytes, sizeof(bytes), &decoded));
    response.result = -9;
    assert(kb2_fs_response_encode(bytes, sizeof(bytes), &response));
    response.data_length = 0;
    assert(!kb2_fs_response_encode(bytes, sizeof(bytes), &response));
    assert(!kb2_fs_response_decode(bytes, KB2_FILESYSTEM_RESPONSE_SIZE, &decoded));
    assert(decoded.result == -9);
    decoded.sequence++;
    assert(!kb2_fs_response_matches(&decoded, &request));
    response.result = -4096;
    assert(kb2_fs_response_encode(bytes, sizeof(bytes), &response));
    response.result = INT64_MAX;
    assert(!kb2_fs_response_encode(bytes, sizeof(bytes), &response));
    assert(!kb2_fs_response_decode(bytes, KB2_FILESYSTEM_RESPONSE_SIZE, &decoded));
    assert(decoded.result == INT64_MAX);
}

static void credential_codec(void) {
    uint8_t bytes[KB2_FILESYSTEM_CREDENTIALS_SIZE];
    kb2_fs_credentials_t value = {.uid = 1000, .gid = 1001, .euid = 1002,
        .egid = 1003, .suid = 1004, .sgid = 1005, .fsuid = 1006, .fsgid = 1007,
        .cap_permitted = KB2_FILESYSTEM_CREDENTIAL_CAP_VALID_MASK,
        .cap_effective = 3, .cap_inheritable = 7, .cap_ambient = 1,
        .securebits = 3, .umask = 0027}, decoded;
    assert(!kb2_fs_credentials_encode(bytes, sizeof(bytes), &value));
    assert(!kb2_fs_credentials_decode(bytes, sizeof(bytes), &decoded));
    assert(decoded.uid == 1000 && decoded.fsgid == 1007 && decoded.umask == 0027);
    assert(decoded.cap_permitted == KB2_FILESYSTEM_CREDENTIAL_CAP_VALID_MASK);
    assert(decoded.cap_effective == 3 && decoded.cap_ambient == 1);
    bytes[KB2_FILESYSTEM_CREDENTIALS_RESERVED_OFFSET] = 1;
    assert(kb2_fs_credentials_decode(bytes, sizeof(bytes), &decoded));
    bytes[KB2_FILESYSTEM_CREDENTIALS_RESERVED_OFFSET] = 0;
    assert(kb2_fs_credentials_decode(bytes, sizeof(bytes) - 1, &decoded));
    value.umask = 01000;
    assert(kb2_fs_credentials_encode(bytes, sizeof(bytes), &value));
}

static void operation_catalog(void) {
    uint8_t bytes[KB2_FILESYSTEM_REQUEST_SIZE];
    assert(KB2_FILESYSTEM_OP_EXEC_OPEN == KB2_FILESYSTEM_OP_SERVICE_BIND + 1);
    assert(KB2_FILESYSTEM_OP_EXEC_PATH == KB2_FILESYSTEM_OP_EXEC_OPEN + 1);
    assert(KB2_FILESYSTEM_OP_SYNC == KB2_FILESYSTEM_OP_FACCESSAT2 + 1);
    assert(KB2_FILESYSTEM_OP_SYNCFS == KB2_FILESYSTEM_OP_SYNC + 1);
    assert(KB2_FILESYSTEM_OP_MEMFD_CREATE == KB2_FILESYSTEM_OP_SYNCFS + 1);
    for (uint32_t opcode = 1; opcode <= KB2_FILESYSTEM_OPERATION_COUNT; ++opcode) {
        kb2_fs_request_t request = {.opcode = opcode, .generation = 1, .sequence = 1};
        kb2_fs_request_t decoded;
        assert(!kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &request));
        assert(!kb2_fs_request_decode(bytes, sizeof(bytes), sizeof(bytes), &decoded));
        assert(decoded.opcode == opcode);
    }
    kb2_fs_request_t invalid = {.opcode = KB2_FILESYSTEM_OPERATION_COUNT + 1,
        .generation = 1, .sequence = 1};
    assert(kb2_fs_request_encode(bytes, sizeof(bytes), sizeof(bytes), &invalid));
}

int main(void) {
    request_codec();
    response_codec();
    credential_codec();
    operation_catalog();
    puts("filesystem codecs: OK");
    return 0;
}
