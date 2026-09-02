/* SPDX-License-Identifier: Apache-2.0 */

#ifndef KOBOX2_CLOSURE_H
#define KOBOX2_CLOSURE_H

#include <kobox2/controller.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KB2_CLOSURE_NAME_MAX 127u

typedef struct kb2_closure_builder kb2_closure_builder_t;

typedef enum kb2_artifact_kind {
    KB2_ARTIFACT_SHARED_PROVIDER = 0,
    KB2_ARTIFACT_RELOCATABLE_MODULE,
} kb2_artifact_kind_t;

typedef enum kb2_symbol_kind {
    KB2_SYMBOL_FUNCTION = 0,
    KB2_SYMBOL_OBJECT,
} kb2_symbol_kind_t;

typedef enum kb2_import_flag {
    KB2_IMPORT_OPTIONAL = 1u << 0,
} kb2_import_flag_t;

typedef enum kb2_resource_type {
    KB2_RESOURCE_MEMORY = 0,
    KB2_RESOURCE_DEVICE,
    KB2_RESOURCE_STORAGE,
    KB2_RESOURCE_NOTIFICATION,
    KB2_RESOURCE_CHANNEL,
} kb2_resource_type_t;

typedef enum kb2_resource_flag {
    KB2_RESOURCE_REQUIRED = 1u << 0,
    KB2_RESOURCE_SHARED = 1u << 1,
    KB2_RESOURCE_RESET_REQUIRED = 1u << 2,
} kb2_resource_flag_t;

#define KB2_MEMORY_RIGHT_READ (UINT64_C(1) << 0)
#define KB2_MEMORY_RIGHT_WRITE (UINT64_C(1) << 1)
#define KB2_MEMORY_RIGHT_MAP (UINT64_C(1) << 2)
#define KB2_MEMORY_RIGHT_DMA (UINT64_C(1) << 3)

#define KB2_DEVICE_RIGHT_COMMAND (UINT64_C(1) << 0)
#define KB2_DEVICE_RIGHT_MAP (UINT64_C(1) << 1)
#define KB2_DEVICE_RIGHT_DMA (UINT64_C(1) << 2)

#define KB2_STORAGE_RIGHT_READ_BLOCKS (UINT64_C(1) << 0)
#define KB2_STORAGE_RIGHT_WRITE_BLOCKS (UINT64_C(1) << 1)
#define KB2_STORAGE_RIGHT_FLUSH (UINT64_C(1) << 2)
#define KB2_STORAGE_RIGHT_DISCARD (UINT64_C(1) << 3)

#define KB2_NOTIFICATION_RIGHT_WAIT (UINT64_C(1) << 0)
#define KB2_NOTIFICATION_RIGHT_SIGNAL (UINT64_C(1) << 1)

#define KB2_CHANNEL_RIGHT_SEND (UINT64_C(1) << 0)
#define KB2_CHANNEL_RIGHT_RECEIVE (UINT64_C(1) << 1)

kb2_status_t kb2_closure_builder_create(kb2_allocate_fn allocate,
                                        kb2_deallocate_fn deallocate,
                                        void *allocator_context,
                                        const uint8_t *manifest_digest,
                                        size_t digest_size,
                                        kb2_closure_builder_t **builder_out);
void kb2_closure_builder_destroy(kb2_closure_builder_t *builder);

kb2_status_t kb2_closure_builder_add_artifact(kb2_closure_builder_t *builder,
                                              uint32_t node_id,
                                              kb2_artifact_kind_t kind,
                                              const uint8_t *content_digest,
                                              size_t digest_size,
                                              const char *namespace_name,
                                              size_t namespace_length);
kb2_status_t kb2_closure_builder_set_lifecycle(kb2_closure_builder_t *builder,
                                               uint32_t node_id,
                                               const char *init_symbol,
                                               size_t init_length,
                                               const char *quiesce_symbol,
                                               size_t quiesce_length,
                                               const char *cleanup_symbol,
                                               size_t cleanup_length);
kb2_status_t kb2_closure_builder_mark_root(kb2_closure_builder_t *builder, uint32_t node_id);
kb2_status_t kb2_closure_builder_add_dependency(kb2_closure_builder_t *builder,
                                                uint32_t consumer_node_id,
                                                uint32_t provider_node_id);
kb2_status_t kb2_closure_builder_add_export(kb2_closure_builder_t *builder,
                                            uint32_t node_id,
                                            const char *symbol,
                                            size_t symbol_length,
                                            kb2_symbol_kind_t kind);
kb2_status_t kb2_closure_builder_add_import(kb2_closure_builder_t *builder,
                                            uint32_t consumer_node_id,
                                            const char *consumer_symbol,
                                            size_t consumer_symbol_length,
                                            uint32_t provider_node_id,
                                            const char *provider_symbol,
                                            size_t provider_symbol_length,
                                            kb2_symbol_kind_t kind,
                                            uint32_t flags);
kb2_status_t kb2_closure_builder_add_resource(kb2_closure_builder_t *builder,
                                              uint32_t slot_id,
                                              kb2_resource_type_t type,
                                              uint32_t minimum_count,
                                              uint32_t maximum_count,
                                              uint64_t required_rights,
                                              uint64_t maximum_rights,
                                              uint32_t flags);
kb2_status_t kb2_closure_builder_bind_resource(kb2_closure_builder_t *builder,
                                               uint32_t slot_id,
                                               uint32_t node_id);

/* Sealing validates the complete graph and creates an independent immutable copy. */
kb2_status_t kb2_closure_builder_seal(const kb2_closure_builder_t *builder,
                                      kb2_closure_t **closure_out);
void kb2_closure_destroy(kb2_closure_t *closure);

kb2_status_t kb2_closure_copy_manifest_digest(const kb2_closure_t *closure,
                                              uint8_t *digest_out,
                                              size_t digest_size);
size_t kb2_closure_artifact_count(const kb2_closure_t *closure);
size_t kb2_closure_dependency_count(const kb2_closure_t *closure);
size_t kb2_closure_export_count(const kb2_closure_t *closure);
size_t kb2_closure_import_count(const kb2_closure_t *closure);
size_t kb2_closure_resource_count(const kb2_closure_t *closure);
size_t kb2_closure_resource_binding_count(const kb2_closure_t *closure);

kb2_status_t kb2_closure_artifact(const kb2_closure_t *closure,
                                  size_t index,
                                  uint32_t *node_id_out,
                                  kb2_artifact_kind_t *kind_out,
                                  int *is_root_out,
                                  uint8_t *content_digest_out,
                                  size_t digest_size);
kb2_status_t kb2_closure_resource(const kb2_closure_t *closure,
                                  size_t index,
                                  uint32_t *slot_id_out,
                                  kb2_resource_type_t *type_out,
                                  uint32_t *minimum_count_out,
                                  uint32_t *maximum_count_out,
                                  uint64_t *required_rights_out,
                                  uint64_t *maximum_rights_out,
                                  uint32_t *flags_out);
kb2_status_t kb2_closure_resource_binding(const kb2_closure_t *closure,
                                          size_t index,
                                          uint32_t *slot_id_out,
                                          uint32_t *node_id_out);

#ifdef __cplusplus
}
#endif

#endif
