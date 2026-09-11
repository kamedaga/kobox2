/* SPDX-License-Identifier: Apache-2.0 */

#ifndef KOBOX2_CLOSURE_INTERNAL_H
#define KOBOX2_CLOSURE_INTERNAL_H

#include <kobox2/closure.h>

struct kb2_closure_artifact_record {
    uint8_t content_digest[KB2_DIGEST_SIZE];
    char namespace_name[KB2_CLOSURE_NAME_MAX + 1u];
    char init_symbol[KB2_CLOSURE_NAME_MAX + 1u];
    char quiesce_symbol[KB2_CLOSURE_NAME_MAX + 1u];
    char cleanup_symbol[KB2_CLOSURE_NAME_MAX + 1u];
    uint32_t node_id;
    kb2_artifact_kind_t kind;
    int lifecycle_set;
    int native_lifecycle;
    int is_root;
};

struct kb2_closure_dependency_record {
    uint32_t consumer_node_id;
    uint32_t provider_node_id;
};

struct kb2_closure_export_record {
    char symbol[KB2_CLOSURE_NAME_MAX + 1u];
    uint32_t node_id;
    kb2_symbol_kind_t kind;
};

struct kb2_closure_import_record {
    char consumer_symbol[KB2_CLOSURE_NAME_MAX + 1u];
    char provider_symbol[KB2_CLOSURE_NAME_MAX + 1u];
    uint32_t consumer_node_id;
    uint32_t provider_node_id;
    kb2_symbol_kind_t kind;
    uint32_t flags;
};

struct kb2_closure_resource_record {
    uint8_t interface_schema_digest[KB2_DIGEST_SIZE];
    uint64_t required_rights;
    uint64_t maximum_rights;
    uint32_t slot_id;
    kb2_resource_type_t type;
    uint32_t minimum_count;
    uint32_t maximum_count;
    uint32_t flags;
};

struct kb2_closure_resource_binding_record {
    uint32_t slot_id;
    uint32_t node_id;
};

struct kb2_closure_storage {
    struct kb2_closure_artifact_record *artifacts;
    size_t artifact_count;
    size_t artifact_capacity;
    struct kb2_closure_dependency_record *dependencies;
    size_t dependency_count;
    size_t dependency_capacity;
    struct kb2_closure_export_record *exports;
    size_t export_count;
    size_t export_capacity;
    struct kb2_closure_import_record *imports;
    size_t import_count;
    size_t import_capacity;
    struct kb2_closure_resource_record *resources;
    size_t resource_count;
    size_t resource_capacity;
    struct kb2_closure_resource_binding_record *resource_bindings;
    size_t resource_binding_count;
    size_t resource_binding_capacity;
    uint8_t manifest_digest[KB2_DIGEST_SIZE];
};

struct kb2_closure {
    kb2_deallocate_fn deallocate;
    void *allocator_context;
    struct kb2_closure_storage storage;
};

kb2_status_t kb2_closure_clone(const kb2_closure_t *source,
                               kb2_allocate_fn allocate,
                               kb2_deallocate_fn deallocate,
                               void *allocator_context,
                               kb2_closure_t **closure_out);
uint32_t kb2_closure_launch_flags(const kb2_closure_t *closure);

#endif
