/* SPDX-License-Identifier: MIT */
#include <kobox2/filesystem.h>

#ifdef __KERNEL__
#include <linux/limits.h>
#include <linux/string.h>
#define UINT32_C(value) value##U
#define INT64_MAX S64_MAX
#define UINT64_MAX U64_MAX
#else
#include <limits.h>
#include <string.h>
#endif

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
           (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static int64_t signed64(uint64_t value) {
    /* Conversion of an out-of-range unsigned integer is implementation-defined. */
    return value <= INT64_MAX ? (int64_t)value : -1 - (int64_t)(UINT64_MAX - value);
}

static void put32(uint8_t *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

static void put64(uint8_t *p, uint64_t value) {
    put32(p, (uint32_t)value);
    put32(p + 4, (uint32_t)(value >> 32));
}

static int opcode_valid(uint32_t opcode) {
    return opcode >= KB2_FILESYSTEM_OP_HELLO && opcode <= KB2_FILESYSTEM_OP_MEMFD_CREATE;
}

kb2_fs_operation_class_t kb2_fs_request_policy(const kb2_fs_request_t *request,
                                             uint64_t *required_rights) {
    if (!request || !required_rights)
        return KB2_FS_OPERATION_INVALID;
    *required_rights = 0;
    switch (request->opcode) {
    case KB2_FILESYSTEM_OP_CLIENT_REGISTER:
    case KB2_FILESYSTEM_OP_CLIENT_CREDENTIALS:
    case KB2_FILESYSTEM_OP_CLIENT_UNREGISTER:
    case KB2_FILESYSTEM_OP_SERVICE_BIND:
    case KB2_FILESYSTEM_OP_EXEC_OPEN:
        return KB2_FS_OPERATION_OWNER;
    case KB2_FILESYSTEM_OP_HELLO:
    case KB2_FILESYSTEM_OP_SESSION_ATTACH:
    case KB2_FILESYSTEM_OP_SESSION_DETACH:
    case KB2_FILESYSTEM_OP_TRANSFER_DUP:
    case KB2_FILESYSTEM_OP_SHARED_FILE_VMO:
        return KB2_FS_OPERATION_FRONTEND;
    case KB2_FILESYSTEM_OP_EXEC_PATH:
    case KB2_FILESYSTEM_OP_EXEC_SELF:
        *required_rights = KB2_FILESYSTEM_RIGHT_EXEC | KB2_FILESYSTEM_RIGHT_READ;
        return KB2_FS_OPERATION_FRONTEND;
    case KB2_FILESYSTEM_OP_FILE_VMO:
        *required_rights = KB2_FILESYSTEM_RIGHT_READ;
        if (request->flags & 2u)
            *required_rights |= KB2_FILESYSTEM_RIGHT_EXEC;
        return KB2_FS_OPERATION_FRONTEND;
    case KB2_FILESYSTEM_OP_READ:
    case KB2_FILESYSTEM_OP_PREAD:
        *required_rights = KB2_FILESYSTEM_RIGHT_READ;
        break;
    case KB2_FILESYSTEM_OP_WRITE:
    case KB2_FILESYSTEM_OP_PWRITE:
    case KB2_FILESYSTEM_OP_FTRUNCATE:
        *required_rights = KB2_FILESYSTEM_RIGHT_WRITE;
        break;
    case KB2_FILESYSTEM_OP_GETDENTS64:
        *required_rights = KB2_FILESYSTEM_RIGHT_GETDENTS;
        break;
    case KB2_FILESYSTEM_OP_STATX:
    case KB2_FILESYSTEM_OP_STATFS:
    case KB2_FILESYSTEM_OP_READLINKAT:
    case KB2_FILESYSTEM_OP_READLINK_HANDLE:
        *required_rights = KB2_FILESYSTEM_RIGHT_STAT;
        break;
    case KB2_FILESYSTEM_OP_FACCESSAT2:
        *required_rights = KB2_FILESYSTEM_RIGHT_LOOKUP;
        break;
    case KB2_FILESYSTEM_OP_MKDIRAT:
    case KB2_FILESYSTEM_OP_MKNODAT:
    case KB2_FILESYSTEM_OP_SYMLINKAT:
    case KB2_FILESYSTEM_OP_LINKAT:
    case KB2_FILESYSTEM_OP_MEMFD_CREATE:
        *required_rights = KB2_FILESYSTEM_RIGHT_CREATE;
        break;
    case KB2_FILESYSTEM_OP_UNLINKAT:
        *required_rights = KB2_FILESYSTEM_RIGHT_REMOVE;
        break;
    case KB2_FILESYSTEM_OP_RENAMEAT2:
        *required_rights = KB2_FILESYSTEM_RIGHT_RENAME | KB2_FILESYSTEM_RIGHT_REMOVE;
        break;
    case KB2_FILESYSTEM_OP_FCHMODAT:
    case KB2_FILESYSTEM_OP_FCHOWNAT:
    case KB2_FILESYSTEM_OP_UTIMENSAT:
        *required_rights = KB2_FILESYSTEM_RIGHT_SETATTR;
        break;
    case KB2_FILESYSTEM_OP_DEVICE_ROUTE:
    case KB2_FILESYSTEM_OP_OPENAT2:
        *required_rights = KB2_FILESYSTEM_RIGHT_LOOKUP;
        /* O_PATH grants a reference rather than read/write access. Linux
         * still validates every flag combination and creation mode. */
        if (request->flags & 010000000u)
            break;
        if (request->flags & 0100u)
            *required_rights |= KB2_FILESYSTEM_RIGHT_CREATE;
        if (request->flags & 01000u)
            *required_rights |= KB2_FILESYSTEM_RIGHT_WRITE;
        if ((request->flags & 3u) != 1u)
            *required_rights |= KB2_FILESYSTEM_RIGHT_READ;
        if ((request->flags & 3u) != 0u)
            *required_rights |= KB2_FILESYSTEM_RIGHT_WRITE;
        break;
    case KB2_FILESYSTEM_OP_CLOSE:
    case KB2_FILESYSTEM_OP_DUP:
    case KB2_FILESYSTEM_OP_LSEEK:
    case KB2_FILESYSTEM_OP_FCNTL:
    case KB2_FILESYSTEM_OP_FSYNC:
    case KB2_FILESYSTEM_OP_SYNC:
    case KB2_FILESYSTEM_OP_SYNCFS:
        /* These preserve the existing capability policy. Linux checks
         * ownership of handles and the operation's actual access rules. */
        break;
    default:
        return KB2_FS_OPERATION_INVALID;
    }
    return request->opcode == KB2_FILESYSTEM_OP_DEVICE_ROUTE ?
        KB2_FS_OPERATION_FRONTEND : KB2_FS_OPERATION_DATA;
}

static int span_valid(kb2_fs_span_t span, uint64_t size) {
    if (!span.length)
        return !span.offset;
    return span.offset >= KB2_FILESYSTEM_REQUEST_SIZE && span.offset <= size &&
           span.length <= size - span.offset;
}

static int request_valid(const kb2_fs_request_t *request, uint64_t size) {
    if (!opcode_valid(request->opcode) || !request->generation || !request->sequence ||
        request->umask & ~UINT32_C(0777) || size < KB2_FILESYSTEM_REQUEST_SIZE)
        return 0;
    const kb2_fs_span_t spans[] = {
        request->path, request->second_path, request->groups, request->data,
    };
    for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); ++i) {
        if (!span_valid(spans[i], size))
            return 0;
        for (size_t j = 0; j < i; ++j) {
            if (spans[i].length && spans[j].length &&
                spans[i].offset < spans[j].offset + spans[j].length &&
                spans[j].offset < spans[i].offset + spans[i].length)
                return 0;
        }
    }
    return !(request->groups.offset & 3) && !(request->groups.length & 3);
}

#define REQUEST_U64_FIELDS(X) \
    X(generation, GENERATION) X(sequence, SEQUENCE) X(client, CLIENT) \
    X(credential_generation, CREDENTIAL_GENERATION) X(handle, HANDLE) \
    X(second_handle, SECOND_HANDLE) X(length, LENGTH) X(flags, FLAGS) \
    X(mode, MODE) X(resolve, RESOLVE) X(capabilities, CAPABILITIES) \
    X(path.offset, PATH_OFFSET) X(path.length, PATH_LENGTH) \
    X(second_path.offset, SECOND_PATH_OFFSET) X(second_path.length, SECOND_PATH_LENGTH) \
    X(groups.offset, GROUPS_OFFSET) X(groups.length, GROUPS_LENGTH) \
    X(data.offset, DATA_OFFSET) X(data.length, DATA_LENGTH)

#define REQUEST_U32_FIELDS(X) \
    X(opcode, OPCODE) X(mask, MASK) X(uid, UID) X(gid, GID) X(umask, UMASK)

int kb2_fs_request_encode(uint8_t *bytes, size_t header_size,
                         uint64_t payload_size, const kb2_fs_request_t *request) {
    static const uint8_t identity[4] = KB2_FILESYSTEM_ABI_IDENTITY_BYTES;
    if (!bytes || !request || header_size < KB2_FILESYSTEM_REQUEST_SIZE ||
        !request_valid(request, payload_size))
        return -1;
    memset(bytes, 0, KB2_FILESYSTEM_REQUEST_SIZE);
    memcpy(bytes, identity, sizeof(identity));
#define ENCODE64(member, field) \
    put64(bytes + KB2_FILESYSTEM_REQUEST_##field##_OFFSET, request->member);
    REQUEST_U64_FIELDS(ENCODE64)
#undef ENCODE64
#define ENCODE32(member, field) \
    put32(bytes + KB2_FILESYSTEM_REQUEST_##field##_OFFSET, request->member);
    REQUEST_U32_FIELDS(ENCODE32)
#undef ENCODE32
    put64(bytes + KB2_FILESYSTEM_REQUEST_OFFSET_OFFSET, (uint64_t)request->offset);
    return 0;
}

int kb2_fs_request_decode(const uint8_t *bytes, size_t header_size,
                         uint64_t payload_size, kb2_fs_request_t *request) {
    static const uint8_t identity[4] = KB2_FILESYSTEM_ABI_IDENTITY_BYTES;
    kb2_fs_request_t value = {0};
    if (!bytes || !request || header_size < KB2_FILESYSTEM_REQUEST_SIZE ||
        memcmp(bytes, identity, sizeof(identity)) ||
        get64(bytes + KB2_FILESYSTEM_REQUEST_RESERVED_OFFSET))
        return -1;
#define DECODE64(member, field) \
    value.member = get64(bytes + KB2_FILESYSTEM_REQUEST_##field##_OFFSET);
    REQUEST_U64_FIELDS(DECODE64)
#undef DECODE64
#define DECODE32(member, field) \
    value.member = get32(bytes + KB2_FILESYSTEM_REQUEST_##field##_OFFSET);
    REQUEST_U32_FIELDS(DECODE32)
#undef DECODE32
    value.offset = signed64(get64(bytes + KB2_FILESYSTEM_REQUEST_OFFSET_OFFSET));
    if (!request_valid(&value, payload_size))
        return -1;
    *request = value;
    return 0;
}

int kb2_fs_request_validate_paths(const uint8_t *bytes, size_t size,
                                  const kb2_fs_request_t *request) {
    if (!bytes || !request || !request_valid(request, size))
        return -1;
    const kb2_fs_span_t paths[] = {request->path, request->second_path};
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        if (!paths[i].length)
            continue;
        const uint8_t *path = bytes + (size_t)paths[i].offset;
        size_t length = (size_t)paths[i].length;
        if (path[length - 1] || memchr(path, 0, length - 1))
            return -1;
    }
    return 0;
}

static int response_valid(const kb2_fs_response_t *response) {
    return opcode_valid(response->opcode) && response->generation && response->sequence &&
           response->result >= -4095 &&
           response->data_length <= UINT64_MAX - KB2_FILESYSTEM_RESPONSE_SIZE &&
           (response->result >= 0 ||
            (!response->handle && !response->data_length && !response->service));
}

int kb2_fs_response_encode(uint8_t *bytes, size_t capacity,
                          const kb2_fs_response_t *response) {
    static const uint8_t identity[4] = KB2_FILESYSTEM_ABI_IDENTITY_BYTES;
    if (!bytes || !response || capacity < KB2_FILESYSTEM_RESPONSE_SIZE ||
        !response_valid(response))
        return -1;
    memset(bytes, 0, KB2_FILESYSTEM_RESPONSE_SIZE);
    memcpy(bytes, identity, sizeof(identity));
    put32(bytes + KB2_FILESYSTEM_RESPONSE_OPCODE_OFFSET, response->opcode);
    put64(bytes + KB2_FILESYSTEM_RESPONSE_GENERATION_OFFSET, response->generation);
    put64(bytes + KB2_FILESYSTEM_RESPONSE_SEQUENCE_OFFSET, response->sequence);
    put64(bytes + KB2_FILESYSTEM_RESPONSE_RESULT_OFFSET, (uint64_t)response->result);
    put64(bytes + KB2_FILESYSTEM_RESPONSE_HANDLE_OFFSET, response->handle);
    put64(bytes + KB2_FILESYSTEM_RESPONSE_DATA_LENGTH_OFFSET, response->data_length);
    put32(bytes + KB2_FILESYSTEM_RESPONSE_SERVICE_OFFSET, response->service);
    return 0;
}

int kb2_fs_response_decode(const uint8_t *bytes, size_t size,
                          kb2_fs_response_t *response) {
    static const uint8_t identity[4] = KB2_FILESYSTEM_ABI_IDENTITY_BYTES;
    kb2_fs_response_t value = {0};
    if (!bytes || !response || size < KB2_FILESYSTEM_RESPONSE_SIZE ||
        memcmp(bytes, identity, sizeof(identity)))
        return -1;
    for (size_t i = KB2_FILESYSTEM_RESPONSE_RESERVED_OFFSET;
         i < KB2_FILESYSTEM_RESPONSE_SIZE; ++i)
        if (bytes[i])
            return -1;
    value.opcode = get32(bytes + KB2_FILESYSTEM_RESPONSE_OPCODE_OFFSET);
    value.generation = get64(bytes + KB2_FILESYSTEM_RESPONSE_GENERATION_OFFSET);
    value.sequence = get64(bytes + KB2_FILESYSTEM_RESPONSE_SEQUENCE_OFFSET);
    value.result = signed64(get64(bytes + KB2_FILESYSTEM_RESPONSE_RESULT_OFFSET));
    value.handle = get64(bytes + KB2_FILESYSTEM_RESPONSE_HANDLE_OFFSET);
    value.data_length = get64(bytes + KB2_FILESYSTEM_RESPONSE_DATA_LENGTH_OFFSET);
    value.service = get32(bytes + KB2_FILESYSTEM_RESPONSE_SERVICE_OFFSET);
    if (!response_valid(&value) || value.data_length != size - KB2_FILESYSTEM_RESPONSE_SIZE)
        return -1;
    *response = value;
    return 0;
}

int kb2_fs_response_matches(const kb2_fs_response_t *response,
                           const kb2_fs_request_t *request) {
    return response && request && response->opcode == request->opcode &&
           response->generation == request->generation && response->sequence == request->sequence;
}

#define CREDENTIAL_U32_FIELDS(X) \
    X(uid, UID) X(gid, GID) X(euid, EUID) X(egid, EGID) \
    X(suid, SUID) X(sgid, SGID) X(fsuid, FSUID) X(fsgid, FSGID) \
    X(securebits, SECUREBITS) X(umask, UMASK)
#define CREDENTIAL_U64_FIELDS(X) \
    X(cap_permitted, CAP_PERMITTED) X(cap_effective, CAP_EFFECTIVE) \
    X(cap_inheritable, CAP_INHERITABLE) X(cap_ambient, CAP_AMBIENT)

int kb2_fs_credentials_encode(uint8_t *bytes, size_t capacity,
                             const kb2_fs_credentials_t *credentials) {
    if (!bytes || !credentials || capacity < KB2_FILESYSTEM_CREDENTIALS_SIZE ||
        credentials->umask & ~UINT32_C(0777))
        return -1;
    memset(bytes, 0, KB2_FILESYSTEM_CREDENTIALS_SIZE);
#define ENCODE32(member, field) \
    put32(bytes + KB2_FILESYSTEM_CREDENTIALS_##field##_OFFSET, credentials->member);
    CREDENTIAL_U32_FIELDS(ENCODE32)
#undef ENCODE32
#define ENCODE64(member, field) \
    put64(bytes + KB2_FILESYSTEM_CREDENTIALS_##field##_OFFSET, credentials->member);
    CREDENTIAL_U64_FIELDS(ENCODE64)
#undef ENCODE64
    return 0;
}

int kb2_fs_credentials_decode(const uint8_t *bytes, size_t size,
                             kb2_fs_credentials_t *credentials) {
    kb2_fs_credentials_t value = {0};
    if (!bytes || !credentials || size != KB2_FILESYSTEM_CREDENTIALS_SIZE ||
        get64(bytes + KB2_FILESYSTEM_CREDENTIALS_RESERVED_OFFSET))
        return -1;
#define DECODE32(member, field) \
    value.member = get32(bytes + KB2_FILESYSTEM_CREDENTIALS_##field##_OFFSET);
    CREDENTIAL_U32_FIELDS(DECODE32)
#undef DECODE32
#define DECODE64(member, field) \
    value.member = get64(bytes + KB2_FILESYSTEM_CREDENTIALS_##field##_OFFSET);
    CREDENTIAL_U64_FIELDS(DECODE64)
#undef DECODE64
    if (value.umask & ~UINT32_C(0777))
        return -1;
    *credentials = value;
    return 0;
}
