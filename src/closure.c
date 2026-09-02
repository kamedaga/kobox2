/* SPDX-License-Identifier: Apache-2.0 */

#include "closure_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define KB2_CLOSURE_MAX_ARTIFACTS 4096u
#define KB2_CLOSURE_MAX_DEPENDENCIES 65536u
#define KB2_CLOSURE_MAX_SYMBOLS 65536u
#define KB2_CLOSURE_MAX_RESOURCES 16384u
#define KB2_CLOSURE_MAX_RESOURCE_BINDINGS 65536u
#define KB2_KNOWN_IMPORT_FLAGS ((uint32_t)KB2_IMPORT_OPTIONAL)
#define KB2_KNOWN_RESOURCE_FLAGS                                                                  \
    ((uint32_t)(KB2_RESOURCE_REQUIRED | KB2_RESOURCE_SHARED | KB2_RESOURCE_RESET_REQUIRED))

struct kb2_closure_builder {
    kb2_allocate_fn allocate;
    kb2_deallocate_fn deallocate;
    void *allocator_context;
    struct kb2_closure_storage storage;
};

struct kb2_node_map_entry {
    uint32_t node_id;
    size_t index;
};

static int kb2_compare_artifacts(const void *left_pointer, const void *right_pointer) {
    const struct kb2_closure_artifact_record *left = left_pointer;
    const struct kb2_closure_artifact_record *right = right_pointer;

    return left->node_id < right->node_id ? -1 : left->node_id > right->node_id;
}

static int kb2_compare_dependencies(const void *left_pointer, const void *right_pointer) {
    const struct kb2_closure_dependency_record *left = left_pointer;
    const struct kb2_closure_dependency_record *right = right_pointer;

    if (left->consumer_node_id != right->consumer_node_id) {
        return left->consumer_node_id < right->consumer_node_id ? -1 : 1;
    }
    return left->provider_node_id < right->provider_node_id
               ? -1
               : left->provider_node_id > right->provider_node_id;
}

static int kb2_compare_exports(const void *left_pointer, const void *right_pointer) {
    const struct kb2_closure_export_record *left = left_pointer;
    const struct kb2_closure_export_record *right = right_pointer;
    int order;

    if (left->node_id != right->node_id) {
        return left->node_id < right->node_id ? -1 : 1;
    }
    order = strcmp(left->symbol, right->symbol);
    if (order != 0) {
        return order;
    }
    return left->kind < right->kind ? -1 : left->kind > right->kind;
}

static int kb2_compare_imports(const void *left_pointer, const void *right_pointer) {
    const struct kb2_closure_import_record *left = left_pointer;
    const struct kb2_closure_import_record *right = right_pointer;
    int order;

    if (left->consumer_node_id != right->consumer_node_id) {
        return left->consumer_node_id < right->consumer_node_id ? -1 : 1;
    }
    order = strcmp(left->consumer_symbol, right->consumer_symbol);
    if (order != 0) {
        return order;
    }
    return left->provider_node_id < right->provider_node_id
               ? -1
               : left->provider_node_id > right->provider_node_id;
}

static int kb2_compare_resources(const void *left_pointer, const void *right_pointer) {
    const struct kb2_closure_resource_record *left = left_pointer;
    const struct kb2_closure_resource_record *right = right_pointer;

    return left->slot_id < right->slot_id ? -1 : left->slot_id > right->slot_id;
}

static int kb2_compare_resource_bindings(const void *left_pointer,
                                         const void *right_pointer) {
    const struct kb2_closure_resource_binding_record *left = left_pointer;
    const struct kb2_closure_resource_binding_record *right = right_pointer;

    if (left->slot_id != right->slot_id) {
        return left->slot_id < right->slot_id ? -1 : 1;
    }
    return left->node_id < right->node_id ? -1 : left->node_id > right->node_id;
}

static void kb2_sort_array(void *items,
                           size_t count,
                           size_t item_size,
                           int (*compare)(const void *, const void *)) {
    if (count > 1) {
        qsort(items, count, item_size, compare);
    }
}

static void kb2_storage_sort(struct kb2_closure_storage *storage) {
    kb2_sort_array(storage->artifacts,
                   storage->artifact_count,
                   sizeof(storage->artifacts[0]),
                   kb2_compare_artifacts);
    kb2_sort_array(storage->dependencies,
                   storage->dependency_count,
                   sizeof(storage->dependencies[0]),
                   kb2_compare_dependencies);
    kb2_sort_array(storage->exports,
                   storage->export_count,
                   sizeof(storage->exports[0]),
                   kb2_compare_exports);
    kb2_sort_array(storage->imports,
                   storage->import_count,
                   sizeof(storage->imports[0]),
                   kb2_compare_imports);
    kb2_sort_array(storage->resources,
                   storage->resource_count,
                   sizeof(storage->resources[0]),
                   kb2_compare_resources);
    kb2_sort_array(storage->resource_bindings,
                   storage->resource_binding_count,
                   sizeof(storage->resource_bindings[0]),
                   kb2_compare_resource_bindings);
}

static int kb2_digest_is_zero(const uint8_t digest[KB2_DIGEST_SIZE]) {
    uint8_t combined = 0;
    size_t index;

    for (index = 0; index < KB2_DIGEST_SIZE; ++index) {
        combined = (uint8_t)(combined | digest[index]);
    }
    return combined == 0;
}

static int kb2_name_valid(const char *name, size_t length) {
    size_t index;

    if (name == NULL || length == 0 || length > KB2_CLOSURE_NAME_MAX) {
        return 0;
    }
    for (index = 0; index < length; ++index) {
        unsigned char value = (unsigned char)name[index];

        if (value < 0x21u || value > 0x7eu || value == '/' || value == '\\') {
            return 0;
        }
    }
    return 1;
}

static void kb2_copy_name(char destination[KB2_CLOSURE_NAME_MAX + 1u],
                          const char *source,
                          size_t length) {
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static int kb2_artifact_kind_valid(kb2_artifact_kind_t kind) {
    return kind >= KB2_ARTIFACT_SHARED_PROVIDER && kind <= KB2_ARTIFACT_RELOCATABLE_MODULE;
}

static int kb2_symbol_kind_valid(kb2_symbol_kind_t kind) {
    return kind >= KB2_SYMBOL_FUNCTION && kind <= KB2_SYMBOL_OBJECT;
}

static int kb2_resource_type_valid(kb2_resource_type_t type) {
    return type >= KB2_RESOURCE_MEMORY && type <= KB2_RESOURCE_CHANNEL;
}

static uint64_t kb2_known_resource_rights(kb2_resource_type_t type) {
    switch (type) {
    case KB2_RESOURCE_MEMORY:
        return KB2_MEMORY_RIGHT_READ | KB2_MEMORY_RIGHT_WRITE | KB2_MEMORY_RIGHT_MAP |
               KB2_MEMORY_RIGHT_DMA;
    case KB2_RESOURCE_DEVICE:
        return KB2_DEVICE_RIGHT_COMMAND | KB2_DEVICE_RIGHT_MAP | KB2_DEVICE_RIGHT_DMA;
    case KB2_RESOURCE_STORAGE:
        return KB2_STORAGE_RIGHT_READ_BLOCKS | KB2_STORAGE_RIGHT_WRITE_BLOCKS |
               KB2_STORAGE_RIGHT_FLUSH | KB2_STORAGE_RIGHT_DISCARD;
    case KB2_RESOURCE_NOTIFICATION:
        return KB2_NOTIFICATION_RIGHT_WAIT | KB2_NOTIFICATION_RIGHT_SIGNAL;
    case KB2_RESOURCE_CHANNEL:
        return KB2_CHANNEL_RIGHT_SEND | KB2_CHANNEL_RIGHT_RECEIVE;
    default:
        return 0;
    }
}

static void kb2_release_array(kb2_deallocate_fn deallocate,
                              void *allocator_context,
                              void **items,
                              size_t item_size,
                              size_t *capacity) {
    if (*items != NULL) {
        deallocate(allocator_context, *items, *capacity * item_size);
    }
    *items = NULL;
    *capacity = 0;
}

static void kb2_storage_release(struct kb2_closure_storage *storage,
                                kb2_deallocate_fn deallocate,
                                void *allocator_context) {
    kb2_release_array(deallocate,
                      allocator_context,
                      (void **)&storage->artifacts,
                      sizeof(storage->artifacts[0]),
                      &storage->artifact_capacity);
    kb2_release_array(deallocate,
                      allocator_context,
                      (void **)&storage->dependencies,
                      sizeof(storage->dependencies[0]),
                      &storage->dependency_capacity);
    kb2_release_array(deallocate,
                      allocator_context,
                      (void **)&storage->exports,
                      sizeof(storage->exports[0]),
                      &storage->export_capacity);
    kb2_release_array(deallocate,
                      allocator_context,
                      (void **)&storage->imports,
                      sizeof(storage->imports[0]),
                      &storage->import_capacity);
    kb2_release_array(deallocate,
                      allocator_context,
                      (void **)&storage->resources,
                      sizeof(storage->resources[0]),
                      &storage->resource_capacity);
    kb2_release_array(deallocate,
                      allocator_context,
                      (void **)&storage->resource_bindings,
                      sizeof(storage->resource_bindings[0]),
                      &storage->resource_binding_capacity);
    memset(storage, 0, sizeof(*storage));
}

static kb2_status_t kb2_reserve(kb2_closure_builder_t *builder,
                                void **items,
                                size_t item_size,
                                size_t count,
                                size_t *capacity,
                                size_t maximum_count) {
    size_t new_capacity;
    void *new_items;

    if (count < *capacity) {
        return KB2_STATUS_OK;
    }
    if (count >= maximum_count) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }
    new_capacity = *capacity == 0 ? 8u : *capacity * 2u;
    if (new_capacity < *capacity || new_capacity > maximum_count) {
        new_capacity = maximum_count;
    }
    if (new_capacity > SIZE_MAX / item_size) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }
    new_items = builder->allocate(builder->allocator_context, new_capacity * item_size);
    if (new_items == NULL) {
        return KB2_STATUS_NO_MEMORY;
    }
    memset(new_items, 0, new_capacity * item_size);
    if (*items != NULL) {
        memcpy(new_items, *items, count * item_size);
        builder->deallocate(builder->allocator_context, *items, *capacity * item_size);
    }
    *items = new_items;
    *capacity = new_capacity;
    return KB2_STATUS_OK;
}

static struct kb2_closure_artifact_record *
kb2_builder_find_artifact(kb2_closure_builder_t *builder, uint32_t node_id) {
    size_t index;

    for (index = 0; index < builder->storage.artifact_count; ++index) {
        if (builder->storage.artifacts[index].node_id == node_id) {
            return &builder->storage.artifacts[index];
        }
    }
    return NULL;
}

static const struct kb2_closure_export_record *
kb2_find_export(const struct kb2_closure_storage *storage,
                uint32_t node_id,
                const char *symbol,
                kb2_symbol_kind_t kind) {
    size_t index;

    for (index = 0; index < storage->export_count; ++index) {
        const struct kb2_closure_export_record *record = &storage->exports[index];

        if (record->node_id == node_id && record->kind == kind &&
            strcmp(record->symbol, symbol) == 0) {
            return record;
        }
    }
    return NULL;
}

static int kb2_has_dependency(const struct kb2_closure_storage *storage,
                              uint32_t consumer_node_id,
                              uint32_t provider_node_id) {
    size_t index;

    for (index = 0; index < storage->dependency_count; ++index) {
        const struct kb2_closure_dependency_record *record = &storage->dependencies[index];

        if (record->consumer_node_id == consumer_node_id &&
            record->provider_node_id == provider_node_id) {
            return 1;
        }
    }
    return 0;
}

kb2_status_t kb2_closure_builder_create(kb2_allocate_fn allocate,
                                        kb2_deallocate_fn deallocate,
                                        void *allocator_context,
                                        const uint8_t *manifest_digest,
                                        size_t digest_size,
                                        kb2_closure_builder_t **builder_out) {
    kb2_closure_builder_t *builder;

    if (allocate == NULL || deallocate == NULL || manifest_digest == NULL ||
        digest_size != KB2_DIGEST_SIZE || builder_out == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    *builder_out = NULL;
    if (kb2_digest_is_zero(manifest_digest)) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    builder = allocate(allocator_context, sizeof(*builder));
    if (builder == NULL) {
        return KB2_STATUS_NO_MEMORY;
    }
    memset(builder, 0, sizeof(*builder));
    builder->allocate = allocate;
    builder->deallocate = deallocate;
    builder->allocator_context = allocator_context;
    memcpy(builder->storage.manifest_digest, manifest_digest, KB2_DIGEST_SIZE);
    *builder_out = builder;
    return KB2_STATUS_OK;
}

void kb2_closure_builder_destroy(kb2_closure_builder_t *builder) {
    kb2_deallocate_fn deallocate;
    void *allocator_context;

    if (builder == NULL) {
        return;
    }
    deallocate = builder->deallocate;
    allocator_context = builder->allocator_context;
    kb2_storage_release(&builder->storage, deallocate, allocator_context);
    memset(builder, 0, sizeof(*builder));
    deallocate(allocator_context, builder, sizeof(*builder));
}

kb2_status_t kb2_closure_builder_add_artifact(kb2_closure_builder_t *builder,
                                              uint32_t node_id,
                                              kb2_artifact_kind_t kind,
                                              const uint8_t *content_digest,
                                              size_t digest_size,
                                              const char *namespace_name,
                                              size_t namespace_length) {
    struct kb2_closure_artifact_record *record;
    kb2_status_t status;
    size_t index;

    if (builder == NULL || node_id == 0 || !kb2_artifact_kind_valid(kind) ||
        content_digest == NULL || digest_size != KB2_DIGEST_SIZE ||
        !kb2_name_valid(namespace_name, namespace_length)) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (kb2_digest_is_zero(content_digest)) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    for (index = 0; index < builder->storage.artifact_count; ++index) {
        record = &builder->storage.artifacts[index];
        if (record->node_id == node_id ||
            (strlen(record->namespace_name) == namespace_length &&
             memcmp(record->namespace_name, namespace_name, namespace_length) == 0)) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    status = kb2_reserve(builder,
                         (void **)&builder->storage.artifacts,
                         sizeof(builder->storage.artifacts[0]),
                         builder->storage.artifact_count,
                         &builder->storage.artifact_capacity,
                         KB2_CLOSURE_MAX_ARTIFACTS);
    if (status != KB2_STATUS_OK) {
        return status;
    }
    record = &builder->storage.artifacts[builder->storage.artifact_count++];
    record->node_id = node_id;
    record->kind = kind;
    memcpy(record->content_digest, content_digest, KB2_DIGEST_SIZE);
    kb2_copy_name(record->namespace_name, namespace_name, namespace_length);
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_builder_set_lifecycle(kb2_closure_builder_t *builder,
                                               uint32_t node_id,
                                               const char *init_symbol,
                                               size_t init_length,
                                               const char *quiesce_symbol,
                                               size_t quiesce_length,
                                               const char *cleanup_symbol,
                                               size_t cleanup_length) {
    struct kb2_closure_artifact_record *record;

    if (builder == NULL || node_id == 0 || !kb2_name_valid(init_symbol, init_length) ||
        !kb2_name_valid(quiesce_symbol, quiesce_length) ||
        !kb2_name_valid(cleanup_symbol, cleanup_length)) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    record = kb2_builder_find_artifact(builder, node_id);
    if (record == NULL || record->lifecycle_set) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    kb2_copy_name(record->init_symbol, init_symbol, init_length);
    kb2_copy_name(record->quiesce_symbol, quiesce_symbol, quiesce_length);
    kb2_copy_name(record->cleanup_symbol, cleanup_symbol, cleanup_length);
    record->lifecycle_set = 1;
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_builder_mark_root(kb2_closure_builder_t *builder, uint32_t node_id) {
    struct kb2_closure_artifact_record *record;

    if (builder == NULL || node_id == 0) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    record = kb2_builder_find_artifact(builder, node_id);
    if (record == NULL || record->kind != KB2_ARTIFACT_RELOCATABLE_MODULE || record->is_root) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    record->is_root = 1;
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_builder_add_dependency(kb2_closure_builder_t *builder,
                                                uint32_t consumer_node_id,
                                                uint32_t provider_node_id) {
    struct kb2_closure_dependency_record *record;
    kb2_status_t status;

    if (builder == NULL || consumer_node_id == 0 || provider_node_id == 0) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (consumer_node_id == provider_node_id ||
        kb2_has_dependency(&builder->storage, consumer_node_id, provider_node_id)) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    status = kb2_reserve(builder,
                         (void **)&builder->storage.dependencies,
                         sizeof(builder->storage.dependencies[0]),
                         builder->storage.dependency_count,
                         &builder->storage.dependency_capacity,
                         KB2_CLOSURE_MAX_DEPENDENCIES);
    if (status != KB2_STATUS_OK) {
        return status;
    }
    record = &builder->storage.dependencies[builder->storage.dependency_count++];
    record->consumer_node_id = consumer_node_id;
    record->provider_node_id = provider_node_id;
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_builder_add_export(kb2_closure_builder_t *builder,
                                            uint32_t node_id,
                                            const char *symbol,
                                            size_t symbol_length,
                                            kb2_symbol_kind_t kind) {
    struct kb2_closure_export_record *record;
    kb2_status_t status;
    size_t index;

    if (builder == NULL || node_id == 0 || !kb2_name_valid(symbol, symbol_length) ||
        !kb2_symbol_kind_valid(kind)) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0; index < builder->storage.export_count; ++index) {
        record = &builder->storage.exports[index];
        if (record->node_id == node_id && strlen(record->symbol) == symbol_length &&
            memcmp(record->symbol, symbol, symbol_length) == 0) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    status = kb2_reserve(builder,
                         (void **)&builder->storage.exports,
                         sizeof(builder->storage.exports[0]),
                         builder->storage.export_count,
                         &builder->storage.export_capacity,
                         KB2_CLOSURE_MAX_SYMBOLS);
    if (status != KB2_STATUS_OK) {
        return status;
    }
    record = &builder->storage.exports[builder->storage.export_count++];
    record->node_id = node_id;
    record->kind = kind;
    kb2_copy_name(record->symbol, symbol, symbol_length);
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_builder_add_import(kb2_closure_builder_t *builder,
                                            uint32_t consumer_node_id,
                                            const char *consumer_symbol,
                                            size_t consumer_symbol_length,
                                            uint32_t provider_node_id,
                                            const char *provider_symbol,
                                            size_t provider_symbol_length,
                                            kb2_symbol_kind_t kind,
                                            uint32_t flags) {
    struct kb2_closure_import_record *record;
    kb2_status_t status;
    size_t index;

    if (builder == NULL || consumer_node_id == 0 ||
        !kb2_name_valid(consumer_symbol, consumer_symbol_length) ||
        !kb2_name_valid(provider_symbol, provider_symbol_length) ||
        !kb2_symbol_kind_valid(kind) || (flags & ~KB2_KNOWN_IMPORT_FLAGS) != 0) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0; index < builder->storage.import_count; ++index) {
        record = &builder->storage.imports[index];
        if (record->consumer_node_id == consumer_node_id &&
            strlen(record->consumer_symbol) == consumer_symbol_length &&
            memcmp(record->consumer_symbol, consumer_symbol, consumer_symbol_length) == 0) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    status = kb2_reserve(builder,
                         (void **)&builder->storage.imports,
                         sizeof(builder->storage.imports[0]),
                         builder->storage.import_count,
                         &builder->storage.import_capacity,
                         KB2_CLOSURE_MAX_SYMBOLS);
    if (status != KB2_STATUS_OK) {
        return status;
    }
    record = &builder->storage.imports[builder->storage.import_count++];
    record->consumer_node_id = consumer_node_id;
    record->provider_node_id = provider_node_id;
    record->kind = kind;
    record->flags = flags;
    kb2_copy_name(record->consumer_symbol, consumer_symbol, consumer_symbol_length);
    kb2_copy_name(record->provider_symbol, provider_symbol, provider_symbol_length);
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_builder_add_resource(kb2_closure_builder_t *builder,
                                              uint32_t slot_id,
                                              kb2_resource_type_t type,
                                              const uint8_t *interface_schema_digest,
                                              size_t digest_size,
                                              uint32_t minimum_count,
                                              uint32_t maximum_count,
                                              uint64_t required_rights,
                                              uint64_t maximum_rights,
                                              uint32_t flags) {
    struct kb2_closure_resource_record *record;
    uint64_t known_rights;
    kb2_status_t status;
    size_t index;

    if (builder == NULL || slot_id == 0 || !kb2_resource_type_valid(type) ||
        interface_schema_digest == NULL || digest_size != KB2_DIGEST_SIZE ||
        (flags & ~KB2_KNOWN_RESOURCE_FLAGS) != 0) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    if (kb2_digest_is_zero(interface_schema_digest)) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    known_rights = kb2_known_resource_rights(type);
    if (maximum_count == 0 || minimum_count > maximum_count || maximum_rights == 0 ||
        (maximum_rights & ~known_rights) != 0 ||
        (required_rights & ~maximum_rights) != 0 ||
        ((flags & KB2_RESOURCE_REQUIRED) != 0 && minimum_count == 0)) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    for (index = 0; index < builder->storage.resource_count; ++index) {
        if (builder->storage.resources[index].slot_id == slot_id) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    status = kb2_reserve(builder,
                         (void **)&builder->storage.resources,
                         sizeof(builder->storage.resources[0]),
                         builder->storage.resource_count,
                         &builder->storage.resource_capacity,
                         KB2_CLOSURE_MAX_RESOURCES);
    if (status != KB2_STATUS_OK) {
        return status;
    }
    record = &builder->storage.resources[builder->storage.resource_count++];
    record->slot_id = slot_id;
    record->type = type;
    memcpy(record->interface_schema_digest, interface_schema_digest, KB2_DIGEST_SIZE);
    record->minimum_count = minimum_count;
    record->maximum_count = maximum_count;
    record->required_rights = required_rights;
    record->maximum_rights = maximum_rights;
    record->flags = flags;
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_builder_bind_resource(kb2_closure_builder_t *builder,
                                               uint32_t slot_id,
                                               uint32_t node_id) {
    struct kb2_closure_resource_binding_record *record;
    kb2_status_t status;
    size_t index;

    if (builder == NULL || slot_id == 0 || node_id == 0) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    for (index = 0; index < builder->storage.resource_binding_count; ++index) {
        record = &builder->storage.resource_bindings[index];
        if (record->slot_id == slot_id && record->node_id == node_id) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    status = kb2_reserve(builder,
                         (void **)&builder->storage.resource_bindings,
                         sizeof(builder->storage.resource_bindings[0]),
                         builder->storage.resource_binding_count,
                         &builder->storage.resource_binding_capacity,
                         KB2_CLOSURE_MAX_RESOURCE_BINDINGS);
    if (status != KB2_STATUS_OK) {
        return status;
    }
    record = &builder->storage.resource_bindings[builder->storage.resource_binding_count++];
    record->slot_id = slot_id;
    record->node_id = node_id;
    return KB2_STATUS_OK;
}

static void kb2_sort_node_map(struct kb2_node_map_entry *map, size_t count) {
    size_t index;

    for (index = 1; index < count; ++index) {
        struct kb2_node_map_entry value = map[index];
        size_t position = index;

        while (position > 0 && map[position - 1u].node_id > value.node_id) {
            map[position] = map[position - 1u];
            --position;
        }
        map[position] = value;
    }
}

static size_t kb2_node_map_find(const struct kb2_node_map_entry *map,
                                size_t count,
                                uint32_t node_id) {
    size_t begin = 0;
    size_t end = count;

    while (begin < end) {
        size_t middle = begin + (end - begin) / 2u;

        if (map[middle].node_id == node_id) {
            return map[middle].index;
        }
        if (map[middle].node_id < node_id) {
            begin = middle + 1u;
        } else {
            end = middle;
        }
    }
    return SIZE_MAX;
}

static kb2_status_t kb2_validate_artifacts(const struct kb2_closure_storage *storage) {
    size_t root_count = 0;
    size_t index;

    if (storage->artifact_count == 0) {
        return KB2_STATUS_INVALID_CONFIGURATION;
    }
    for (index = 0; index < storage->artifact_count; ++index) {
        const struct kb2_closure_artifact_record *artifact = &storage->artifacts[index];

        if (!artifact->lifecycle_set ||
            kb2_find_export(storage, artifact->node_id, artifact->init_symbol,
                            KB2_SYMBOL_FUNCTION) == NULL ||
            kb2_find_export(storage, artifact->node_id, artifact->quiesce_symbol,
                            KB2_SYMBOL_FUNCTION) == NULL ||
            kb2_find_export(storage, artifact->node_id, artifact->cleanup_symbol,
                            KB2_SYMBOL_FUNCTION) == NULL) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
        if (artifact->is_root) {
            if (artifact->kind != KB2_ARTIFACT_RELOCATABLE_MODULE) {
                return KB2_STATUS_INVALID_CONFIGURATION;
            }
            ++root_count;
        }
    }
    return root_count == 0 ? KB2_STATUS_INVALID_CONFIGURATION : KB2_STATUS_OK;
}

static kb2_status_t kb2_build_node_map(const struct kb2_closure_storage *storage,
                                       kb2_allocate_fn allocate,
                                       void *allocator_context,
                                       struct kb2_node_map_entry **map_out) {
    struct kb2_node_map_entry *map;
    size_t index;

    if (storage->artifact_count > SIZE_MAX / sizeof(*map)) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }
    map = allocate(allocator_context, storage->artifact_count * sizeof(*map));
    if (map == NULL) {
        return KB2_STATUS_NO_MEMORY;
    }
    for (index = 0; index < storage->artifact_count; ++index) {
        map[index].node_id = storage->artifacts[index].node_id;
        map[index].index = index;
    }
    kb2_sort_node_map(map, storage->artifact_count);
    *map_out = map;
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_validate_graph(const struct kb2_closure_storage *storage,
                                       kb2_allocate_fn allocate,
                                       kb2_deallocate_fn deallocate,
                                       void *allocator_context,
                                       const struct kb2_node_map_entry *map) {
    size_t *heads = NULL;
    size_t *next = NULL;
    size_t *targets = NULL;
    size_t *queue = NULL;
    uint32_t *incoming = NULL;
    uint8_t *reachable = NULL;
    size_t queue_begin = 0;
    size_t queue_end = 0;
    size_t processed = 0;
    size_t index;
    kb2_status_t status = KB2_STATUS_NO_MEMORY;

    heads = allocate(allocator_context, storage->artifact_count * sizeof(*heads));
    queue = allocate(allocator_context, storage->artifact_count * sizeof(*queue));
    incoming = allocate(allocator_context, storage->artifact_count * sizeof(*incoming));
    reachable = allocate(allocator_context, storage->artifact_count * sizeof(*reachable));
    if (storage->dependency_count != 0) {
        next = allocate(allocator_context, storage->dependency_count * sizeof(*next));
        targets = allocate(allocator_context, storage->dependency_count * sizeof(*targets));
    }
    if (heads == NULL || queue == NULL || incoming == NULL || reachable == NULL ||
        (storage->dependency_count != 0 && (next == NULL || targets == NULL))) {
        goto finish;
    }
    memset(heads, 0xff, storage->artifact_count * sizeof(*heads));
    memset(incoming, 0, storage->artifact_count * sizeof(*incoming));
    memset(reachable, 0, storage->artifact_count * sizeof(*reachable));

    for (index = 0; index < storage->dependency_count; ++index) {
        const struct kb2_closure_dependency_record *dependency = &storage->dependencies[index];
        size_t consumer = kb2_node_map_find(
            map, storage->artifact_count, dependency->consumer_node_id);
        size_t provider = kb2_node_map_find(
            map, storage->artifact_count, dependency->provider_node_id);

        if (consumer == SIZE_MAX || provider == SIZE_MAX || incoming[provider] == UINT32_MAX) {
            status = KB2_STATUS_INVALID_CONFIGURATION;
            goto finish;
        }
        if (storage->artifacts[consumer].kind == KB2_ARTIFACT_SHARED_PROVIDER &&
            storage->artifacts[provider].kind != KB2_ARTIFACT_SHARED_PROVIDER) {
            status = KB2_STATUS_INVALID_CONFIGURATION;
            goto finish;
        }
        next[index] = heads[consumer];
        targets[index] = provider;
        heads[consumer] = index;
        ++incoming[provider];
    }

    for (index = 0; index < storage->artifact_count; ++index) {
        if (incoming[index] == 0) {
            queue[queue_end++] = index;
        }
    }
    while (queue_begin < queue_end) {
        size_t node = queue[queue_begin++];
        size_t edge;

        ++processed;
        for (edge = heads[node]; edge != SIZE_MAX; edge = next[edge]) {
            size_t provider = targets[edge];

            if (--incoming[provider] == 0) {
                queue[queue_end++] = provider;
            }
        }
    }
    if (processed != storage->artifact_count) {
        status = KB2_STATUS_INVALID_CONFIGURATION;
        goto finish;
    }

    queue_begin = 0;
    queue_end = 0;
    for (index = 0; index < storage->artifact_count; ++index) {
        if (storage->artifacts[index].is_root) {
            reachable[index] = 1;
            queue[queue_end++] = index;
        }
    }
    while (queue_begin < queue_end) {
        size_t node = queue[queue_begin++];
        size_t edge;

        for (edge = heads[node]; edge != SIZE_MAX; edge = next[edge]) {
            size_t provider = targets[edge];

            if (!reachable[provider]) {
                reachable[provider] = 1;
                queue[queue_end++] = provider;
            }
        }
    }
    for (index = 0; index < storage->artifact_count; ++index) {
        if (!reachable[index]) {
            status = KB2_STATUS_INVALID_CONFIGURATION;
            goto finish;
        }
    }
    status = KB2_STATUS_OK;

finish:
    if (targets != NULL) {
        deallocate(allocator_context,
                   targets,
                   storage->dependency_count * sizeof(*targets));
    }
    if (next != NULL) {
        deallocate(allocator_context, next, storage->dependency_count * sizeof(*next));
    }
    if (reachable != NULL) {
        deallocate(allocator_context,
                   reachable,
                   storage->artifact_count * sizeof(*reachable));
    }
    if (incoming != NULL) {
        deallocate(allocator_context,
                   incoming,
                   storage->artifact_count * sizeof(*incoming));
    }
    if (queue != NULL) {
        deallocate(allocator_context, queue, storage->artifact_count * sizeof(*queue));
    }
    if (heads != NULL) {
        deallocate(allocator_context, heads, storage->artifact_count * sizeof(*heads));
    }
    return status;
}

static kb2_status_t kb2_validate_symbols(const struct kb2_closure_storage *storage,
                                         const struct kb2_node_map_entry *map) {
    size_t index;

    for (index = 0; index < storage->export_count; ++index) {
        const struct kb2_closure_export_record *record = &storage->exports[index];

        if (kb2_node_map_find(map, storage->artifact_count, record->node_id) == SIZE_MAX) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    for (index = 0; index < storage->import_count; ++index) {
        const struct kb2_closure_import_record *record = &storage->imports[index];

        if (kb2_node_map_find(map, storage->artifact_count, record->consumer_node_id) ==
            SIZE_MAX) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
        if (record->provider_node_id == 0) {
            continue;
        }
        if (kb2_node_map_find(map, storage->artifact_count, record->provider_node_id) ==
                SIZE_MAX ||
            !kb2_has_dependency(
                storage, record->consumer_node_id, record->provider_node_id)) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
        if (kb2_find_export(storage,
                            record->provider_node_id,
                            record->provider_symbol,
                            record->kind) == NULL &&
            (record->flags & KB2_IMPORT_OPTIONAL) == 0) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    return KB2_STATUS_OK;
}

static const struct kb2_closure_resource_record *
kb2_find_resource(const struct kb2_closure_storage *storage, uint32_t slot_id) {
    size_t index;

    for (index = 0; index < storage->resource_count; ++index) {
        if (storage->resources[index].slot_id == slot_id) {
            return &storage->resources[index];
        }
    }
    return NULL;
}

static kb2_status_t kb2_validate_resources(const struct kb2_closure_storage *storage,
                                           const struct kb2_node_map_entry *map) {
    size_t index;

    for (index = 0; index < storage->resource_binding_count; ++index) {
        const struct kb2_closure_resource_binding_record *binding =
            &storage->resource_bindings[index];

        if (kb2_find_resource(storage, binding->slot_id) == NULL ||
            kb2_node_map_find(map, storage->artifact_count, binding->node_id) == SIZE_MAX) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    for (index = 0; index < storage->resource_count; ++index) {
        const struct kb2_closure_resource_record *resource = &storage->resources[index];
        size_t binding_count = 0;
        size_t binding_index;

        for (binding_index = 0; binding_index < storage->resource_binding_count;
             ++binding_index) {
            if (storage->resource_bindings[binding_index].slot_id == resource->slot_id) {
                ++binding_count;
            }
        }
        if (binding_count == 0 ||
            ((resource->flags & KB2_RESOURCE_SHARED) == 0 && binding_count != 1)) {
            return KB2_STATUS_INVALID_CONFIGURATION;
        }
    }
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_validate_storage(const struct kb2_closure_storage *storage,
                                         kb2_allocate_fn allocate,
                                         kb2_deallocate_fn deallocate,
                                         void *allocator_context) {
    struct kb2_node_map_entry *map = NULL;
    kb2_status_t status;

    status = kb2_validate_artifacts(storage);
    if (status != KB2_STATUS_OK) {
        return status;
    }
    status = kb2_build_node_map(storage, allocate, allocator_context, &map);
    if (status != KB2_STATUS_OK) {
        return status;
    }
    status = kb2_validate_graph(storage, allocate, deallocate, allocator_context, map);
    if (status == KB2_STATUS_OK) {
        status = kb2_validate_symbols(storage, map);
    }
    if (status == KB2_STATUS_OK) {
        status = kb2_validate_resources(storage, map);
    }
    deallocate(allocator_context, map, storage->artifact_count * sizeof(*map));
    return status;
}

static kb2_status_t kb2_clone_array(kb2_allocate_fn allocate,
                                    void *allocator_context,
                                    const void *source,
                                    size_t count,
                                    size_t item_size,
                                    void **destination_out) {
    void *destination;

    *destination_out = NULL;
    if (count == 0) {
        return KB2_STATUS_OK;
    }
    if (count > SIZE_MAX / item_size) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }
    destination = allocate(allocator_context, count * item_size);
    if (destination == NULL) {
        return KB2_STATUS_NO_MEMORY;
    }
    memcpy(destination, source, count * item_size);
    *destination_out = destination;
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_clone(const kb2_closure_t *source,
                               kb2_allocate_fn allocate,
                               kb2_deallocate_fn deallocate,
                               void *allocator_context,
                               kb2_closure_t **closure_out) {
    kb2_closure_t *closure;
    kb2_status_t status;

    if (source == NULL || allocate == NULL || deallocate == NULL || closure_out == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    *closure_out = NULL;
    closure = allocate(allocator_context, sizeof(*closure));
    if (closure == NULL) {
        return KB2_STATUS_NO_MEMORY;
    }
    memset(closure, 0, sizeof(*closure));
    closure->deallocate = deallocate;
    closure->allocator_context = allocator_context;
    memcpy(closure->storage.manifest_digest,
           source->storage.manifest_digest,
           KB2_DIGEST_SIZE);

#define KB2_CLONE_FIELD(field, count_field, capacity_field)                                        \
    do {                                                                                           \
        status = kb2_clone_array(allocate,                                                         \
                                 allocator_context,                                                \
                                 source->storage.field,                                            \
                                 source->storage.count_field,                                      \
                                 sizeof(source->storage.field[0]),                                 \
                                 (void **)&closure->storage.field);                                \
        if (status != KB2_STATUS_OK) {                                                             \
            kb2_closure_destroy(closure);                                                          \
            return status;                                                                         \
        }                                                                                          \
        closure->storage.count_field = source->storage.count_field;                                \
        closure->storage.capacity_field = source->storage.count_field;                             \
    } while (0)

    KB2_CLONE_FIELD(artifacts, artifact_count, artifact_capacity);
    KB2_CLONE_FIELD(dependencies, dependency_count, dependency_capacity);
    KB2_CLONE_FIELD(exports, export_count, export_capacity);
    KB2_CLONE_FIELD(imports, import_count, import_capacity);
    KB2_CLONE_FIELD(resources, resource_count, resource_capacity);
    KB2_CLONE_FIELD(resource_bindings, resource_binding_count, resource_binding_capacity);
#undef KB2_CLONE_FIELD

    kb2_storage_sort(&closure->storage);
    *closure_out = closure;
    return KB2_STATUS_OK;
}

uint32_t kb2_closure_launch_flags(const kb2_closure_t *closure) {
    size_t index;

    if (closure == NULL) {
        return 0;
    }
    for (index = 0; index < closure->storage.resource_count; ++index) {
        if ((closure->storage.resources[index].flags & KB2_RESOURCE_RESET_REQUIRED) != 0) {
            return KB2_LAUNCH_RESET_REQUIRED;
        }
    }
    return 0;
}

kb2_status_t kb2_closure_builder_seal(const kb2_closure_builder_t *builder,
                                      kb2_closure_t **closure_out) {
    struct kb2_closure source;
    kb2_status_t status;

    if (builder == NULL || closure_out == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    *closure_out = NULL;
    status = kb2_validate_storage(&builder->storage,
                                  builder->allocate,
                                  builder->deallocate,
                                  builder->allocator_context);
    if (status != KB2_STATUS_OK) {
        return status;
    }
    memset(&source, 0, sizeof(source));
    source.storage = builder->storage;
    return kb2_closure_clone(&source,
                             builder->allocate,
                             builder->deallocate,
                             builder->allocator_context,
                             closure_out);
}

void kb2_closure_destroy(kb2_closure_t *closure) {
    kb2_deallocate_fn deallocate;
    void *allocator_context;

    if (closure == NULL) {
        return;
    }
    deallocate = closure->deallocate;
    allocator_context = closure->allocator_context;
    kb2_storage_release(&closure->storage, deallocate, allocator_context);
    memset(closure, 0, sizeof(*closure));
    deallocate(allocator_context, closure, sizeof(*closure));
}

kb2_status_t kb2_closure_copy_manifest_digest(const kb2_closure_t *closure,
                                              uint8_t *digest_out,
                                              size_t digest_size) {
    if (closure == NULL || digest_out == NULL || digest_size != KB2_DIGEST_SIZE) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    memcpy(digest_out, closure->storage.manifest_digest, KB2_DIGEST_SIZE);
    return KB2_STATUS_OK;
}

size_t kb2_closure_artifact_count(const kb2_closure_t *closure) {
    return closure == NULL ? 0 : closure->storage.artifact_count;
}

size_t kb2_closure_dependency_count(const kb2_closure_t *closure) {
    return closure == NULL ? 0 : closure->storage.dependency_count;
}

size_t kb2_closure_export_count(const kb2_closure_t *closure) {
    return closure == NULL ? 0 : closure->storage.export_count;
}

size_t kb2_closure_import_count(const kb2_closure_t *closure) {
    return closure == NULL ? 0 : closure->storage.import_count;
}

size_t kb2_closure_resource_count(const kb2_closure_t *closure) {
    return closure == NULL ? 0 : closure->storage.resource_count;
}

size_t kb2_closure_resource_binding_count(const kb2_closure_t *closure) {
    return closure == NULL ? 0 : closure->storage.resource_binding_count;
}

kb2_status_t kb2_closure_artifact(const kb2_closure_t *closure,
                                  size_t index,
                                  uint32_t *node_id_out,
                                  kb2_artifact_kind_t *kind_out,
                                  int *is_root_out,
                                  uint8_t *content_digest_out,
                                  size_t digest_size) {
    const struct kb2_closure_artifact_record *record;

    if (closure == NULL || index >= closure->storage.artifact_count || node_id_out == NULL ||
        kind_out == NULL || is_root_out == NULL || content_digest_out == NULL ||
        digest_size != KB2_DIGEST_SIZE) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    record = &closure->storage.artifacts[index];
    *node_id_out = record->node_id;
    *kind_out = record->kind;
    *is_root_out = record->is_root;
    memcpy(content_digest_out, record->content_digest, KB2_DIGEST_SIZE);
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_resource(const kb2_closure_t *closure,
                                  size_t index,
                                  uint32_t *slot_id_out,
                                  kb2_resource_type_t *type_out,
                                  uint8_t *interface_schema_digest_out,
                                  size_t digest_size,
                                  uint32_t *minimum_count_out,
                                  uint32_t *maximum_count_out,
                                  uint64_t *required_rights_out,
                                  uint64_t *maximum_rights_out,
                                  uint32_t *flags_out) {
    const struct kb2_closure_resource_record *record;

    if (closure == NULL || index >= closure->storage.resource_count || slot_id_out == NULL ||
        type_out == NULL || interface_schema_digest_out == NULL ||
        digest_size != KB2_DIGEST_SIZE || minimum_count_out == NULL || maximum_count_out == NULL ||
        required_rights_out == NULL || maximum_rights_out == NULL || flags_out == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    record = &closure->storage.resources[index];
    *slot_id_out = record->slot_id;
    *type_out = record->type;
    memcpy(interface_schema_digest_out, record->interface_schema_digest, KB2_DIGEST_SIZE);
    *minimum_count_out = record->minimum_count;
    *maximum_count_out = record->maximum_count;
    *required_rights_out = record->required_rights;
    *maximum_rights_out = record->maximum_rights;
    *flags_out = record->flags;
    return KB2_STATUS_OK;
}

kb2_status_t kb2_closure_resource_binding(const kb2_closure_t *closure,
                                          size_t index,
                                          uint32_t *slot_id_out,
                                          uint32_t *node_id_out) {
    const struct kb2_closure_resource_binding_record *record;

    if (closure == NULL || index >= closure->storage.resource_binding_count ||
        slot_id_out == NULL || node_id_out == NULL) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    record = &closure->storage.resource_bindings[index];
    *slot_id_out = record->slot_id;
    *node_id_out = record->node_id;
    return KB2_STATUS_OK;
}
