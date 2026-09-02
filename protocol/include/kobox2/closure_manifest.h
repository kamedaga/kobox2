/* SPDX-License-Identifier: MIT */

#ifndef KOBOX2_CLOSURE_MANIFEST_H
#define KOBOX2_CLOSURE_MANIFEST_H

#include <kobox2/closure_layout.h>
#include <kobox2/protocol.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct kb2_closure_string {
    const char *data;
    uint32_t length;
} kb2_closure_string_t;

typedef struct kb2_closure_manifest_artifact {
    uint32_t node_id;
    uint32_t kind;
    uint32_t flags;
    uint64_t content_size;
    uint8_t content_digest[KB2_CLOSURE_SCHEMA_DIGEST_SIZE];
    kb2_closure_string_t namespace_name;
    kb2_closure_string_t init_symbol;
    kb2_closure_string_t quiesce_symbol;
    kb2_closure_string_t cleanup_symbol;
} kb2_closure_manifest_artifact_t;

typedef struct kb2_closure_manifest_dependency {
    uint32_t consumer_node_id;
    uint32_t provider_node_id;
} kb2_closure_manifest_dependency_t;

typedef struct kb2_closure_manifest_symbol {
    uint32_t node_id;
    uint32_t kind;
    kb2_closure_string_t name;
} kb2_closure_manifest_symbol_t;

typedef struct kb2_closure_manifest_import {
    uint32_t consumer_node_id;
    uint32_t provider_node_id;
    uint32_t kind;
    uint32_t flags;
    kb2_closure_string_t consumer_name;
    kb2_closure_string_t provider_name;
} kb2_closure_manifest_import_t;

typedef struct kb2_closure_manifest_resource {
    uint32_t slot_id;
    uint32_t type;
    uint32_t minimum_count;
    uint32_t maximum_count;
    uint64_t required_rights;
    uint64_t maximum_rights;
    uint32_t flags;
    uint8_t interface_schema_digest[KB2_CLOSURE_SCHEMA_DIGEST_SIZE];
} kb2_closure_manifest_resource_t;

typedef struct kb2_closure_manifest_binding {
    uint32_t slot_id;
    uint32_t node_id;
} kb2_closure_manifest_binding_t;

typedef struct kb2_closure_manifest_source {
    const kb2_closure_manifest_artifact_t *artifacts;
    size_t artifact_count;
    const kb2_closure_manifest_dependency_t *dependencies;
    size_t dependency_count;
    const kb2_closure_manifest_symbol_t *exports;
    size_t export_count;
    const kb2_closure_manifest_import_t *imports;
    size_t import_count;
    const kb2_closure_manifest_resource_t *resources;
    size_t resource_count;
    const kb2_closure_manifest_binding_t *bindings;
    size_t binding_count;
} kb2_closure_manifest_source_t;

typedef struct kb2_closure_manifest {
    const uint8_t *bytes;
    size_t size;
    uint32_t counts[6];
    uint32_t offsets[7];
    uint32_t string_table_size;
} kb2_closure_manifest_t;

const char *kb2_closure_schema_sha256_hex(void);
kb2_protocol_status_t kb2_closure_copy_schema_digest(uint8_t *digest_out,
                                                      size_t digest_size);
kb2_protocol_status_t kb2_closure_manifest_encoded_size(
    const kb2_closure_manifest_source_t *source, size_t *size_out);
kb2_protocol_status_t kb2_closure_manifest_encode(
    uint8_t *buffer,
    size_t buffer_size,
    size_t *encoded_size_out,
    const kb2_closure_manifest_source_t *source);
kb2_protocol_status_t kb2_closure_manifest_decode(const uint8_t *buffer,
                                                   size_t buffer_size,
                                                   kb2_closure_manifest_t *manifest_out);
size_t kb2_closure_manifest_artifact_count(const kb2_closure_manifest_t *manifest);
size_t kb2_closure_manifest_dependency_count(const kb2_closure_manifest_t *manifest);
size_t kb2_closure_manifest_export_count(const kb2_closure_manifest_t *manifest);
size_t kb2_closure_manifest_import_count(const kb2_closure_manifest_t *manifest);
size_t kb2_closure_manifest_resource_count(const kb2_closure_manifest_t *manifest);
size_t kb2_closure_manifest_binding_count(const kb2_closure_manifest_t *manifest);
kb2_protocol_status_t kb2_closure_manifest_artifact(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_artifact_t *artifact_out);
kb2_protocol_status_t kb2_closure_manifest_dependency(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_dependency_t *dependency_out);
kb2_protocol_status_t kb2_closure_manifest_import(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_import_t *import_out);
kb2_protocol_status_t kb2_closure_manifest_export(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_symbol_t *export_out);
kb2_protocol_status_t kb2_closure_manifest_resource(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_resource_t *resource_out);
kb2_protocol_status_t kb2_closure_manifest_binding(
    const kb2_closure_manifest_t *manifest,
    size_t index,
    kb2_closure_manifest_binding_t *binding_out);

#ifdef __cplusplus
}
#endif

#endif
