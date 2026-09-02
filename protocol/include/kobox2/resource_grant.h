/* SPDX-License-Identifier: MIT */

#ifndef KOBOX2_RESOURCE_GRANT_H
#define KOBOX2_RESOURCE_GRANT_H

#include <kobox2/closure_manifest.h>
#include <kobox2/resource_grant_layout.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb2_resource_grant_slot_source {
    uint32_t slot_id;
    uint32_t resource_type;
    uint32_t state;
    uint8_t interface_schema_digest[KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE];
} kb2_resource_grant_slot_source_t;

typedef struct kb2_resource_grant_object_source {
    uint32_t slot_id;
    uint64_t object_id;
    uint64_t granted_rights;
} kb2_resource_grant_object_source_t;

typedef struct kb2_resource_grant_handle_binding {
    uint64_t object_id;
    uint32_t role;
    uint32_t transfer_handle_index;
} kb2_resource_grant_handle_binding_t;

typedef struct kb2_resource_grant_source {
    uint64_t generation;
    uint8_t closure_manifest_digest[KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE];
    const kb2_resource_grant_slot_source_t *slots;
    size_t slot_count;
    const kb2_resource_grant_object_source_t *objects;
    size_t object_count;
    const kb2_resource_grant_handle_binding_t *handle_bindings;
    size_t handle_binding_count;
} kb2_resource_grant_source_t;

typedef struct kb2_resource_grant_slot {
    uint32_t slot_id;
    uint32_t resource_type;
    uint32_t state;
    uint32_t object_start;
    uint32_t object_count;
    uint8_t interface_schema_digest[KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE];
} kb2_resource_grant_slot_t;

typedef struct kb2_resource_grant_object {
    uint32_t slot_id;
    uint64_t object_id;
    uint64_t granted_rights;
    uint32_t handle_start;
    uint32_t handle_count;
} kb2_resource_grant_object_t;

typedef struct kb2_resource_grant {
    const uint8_t *bytes;
    size_t size;
    uint64_t generation;
    uint8_t closure_manifest_digest[KB2_RESOURCE_GRANT_SCHEMA_DIGEST_SIZE];
    uint32_t counts[3];
    uint32_t offsets[3];
} kb2_resource_grant_t;

const char *kb2_resource_grant_schema_sha256_hex(void);
kb2_protocol_status_t kb2_resource_grant_copy_schema_digest(uint8_t *digest_out,
                                                            size_t digest_size);
kb2_protocol_status_t kb2_resource_grant_encoded_size(
    const kb2_resource_grant_source_t *source, size_t *size_out);
kb2_protocol_status_t kb2_resource_grant_encode(
    uint8_t *buffer,
    size_t buffer_size,
    size_t *encoded_size_out,
    const kb2_resource_grant_source_t *source);
kb2_protocol_status_t kb2_resource_grant_decode(const uint8_t *buffer,
                                                size_t buffer_size,
                                                kb2_resource_grant_t *grant_out);
kb2_protocol_status_t kb2_resource_grant_validate_manifest(
    const kb2_resource_grant_t *grant,
    const kb2_closure_manifest_t *manifest);
size_t kb2_resource_grant_slot_count(const kb2_resource_grant_t *grant);
size_t kb2_resource_grant_object_count(const kb2_resource_grant_t *grant);
size_t kb2_resource_grant_handle_binding_count(const kb2_resource_grant_t *grant);
kb2_protocol_status_t kb2_resource_grant_slot(const kb2_resource_grant_t *grant,
                                              size_t index,
                                              kb2_resource_grant_slot_t *slot_out);
kb2_protocol_status_t kb2_resource_grant_object(const kb2_resource_grant_t *grant,
                                                size_t index,
                                                kb2_resource_grant_object_t *object_out);
kb2_protocol_status_t kb2_resource_grant_handle_binding(
    const kb2_resource_grant_t *grant,
    size_t index,
    kb2_resource_grant_handle_binding_t *binding_out);

#ifdef __cplusplus
}
#endif

#endif
