/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_FILESYSTEM_H
#define KOBOX2_FILESYSTEM_H

#include <kobox2/filesystem_layout.h>
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

/* Private decoded values, never overlay these structures onto shared memory.
 * Input spans borrow an immutable snapshot through the dispatch call. */
typedef struct kb2_fs_span {
    uint64_t offset, length;
} kb2_fs_span_t;

typedef struct kb2_fs_request {
    uint32_t opcode;
    uint64_t generation, sequence, client, credential_generation;
    uint64_t handle, second_handle;
    int64_t offset;
    uint64_t length, flags, mode, resolve;
    uint32_t mask, uid, gid, umask;
    kb2_fs_span_t path, second_path, groups, data;
    uint64_t capabilities;
} kb2_fs_request_t;

/* FCNTL F_SETLK (Linux command 6): mode is F_RDLCK/F_WRLCK/F_UNLCK,
 * offset is the absolute start, length is the signed Linux l_len bit pattern,
 * mask and data are zero. The authenticated client owns the POSIX lock.
 * LPR resolves SEEK_CUR/END once and implements F_SETLKW as interruptible
 * retries of this nonblocking operation; no client pointer enters storage. */

typedef struct kb2_fs_response {
    uint32_t opcode;
    uint64_t generation, sequence;
    int64_t result;
    uint64_t handle, data_length;
    uint32_t service;
} kb2_fs_response_t;

typedef struct kb2_fs_credentials {
    uint32_t uid, gid, euid, egid, suid, sgid, fsuid, fsgid;
    uint64_t cap_permitted, cap_effective, cap_inheritable, cap_ambient;
    uint32_t securebits, umask;
} kb2_fs_credentials_t;

/* These classes describe authority boundaries, not how a transport schedules
 * requests. Unknown operations fail closed instead of inheriting zero rights.
 * Policy consumes only a decoded private snapshot; granted rights come from
 * the authenticated owner's retained state, never request.capabilities. */
typedef enum kb2_fs_operation_class {
    KB2_FS_OPERATION_INVALID,
    KB2_FS_OPERATION_DATA,
    KB2_FS_OPERATION_FRONTEND,
    KB2_FS_OPERATION_OWNER,
} kb2_fs_operation_class_t;

kb2_fs_operation_class_t kb2_fs_request_policy(const kb2_fs_request_t *request,
                                             uint64_t *required_rights);

/* Supplementary groups occupy request.groups, not a native pointer or a
 * fixed-size array. Linux validates the credential values after decode. */
int kb2_fs_credentials_encode(uint8_t *bytes, size_t capacity,
                             const kb2_fs_credentials_t *credentials);
int kb2_fs_credentials_decode(const uint8_t *bytes, size_t size,
                             kb2_fs_credentials_t *credentials);

/* Header codecs validate wire identity, reserved bytes, generation and spans.
 * payload_size is the full descriptor-chain length, including the header;
 * header_size need only cover the header, enabling bounded queue snapshots. */
int kb2_fs_request_encode(uint8_t *bytes, size_t header_size,
                         uint64_t payload_size, const kb2_fs_request_t *request);
int kb2_fs_request_decode(const uint8_t *bytes, size_t header_size,
                         uint64_t payload_size, kb2_fs_request_t *request);
/* Called on the immutable complete request, not mutable peer memory. */
int kb2_fs_request_validate_paths(const uint8_t *bytes, size_t size,
                                  const kb2_fs_request_t *request);
int kb2_fs_response_encode(uint8_t *bytes, size_t capacity,
                          const kb2_fs_response_t *response);
int kb2_fs_response_decode(const uint8_t *bytes, size_t size,
                          kb2_fs_response_t *response);
int kb2_fs_response_matches(const kb2_fs_response_t *response,
                           const kb2_fs_request_t *request);

#endif
