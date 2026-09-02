/* SPDX-License-Identifier: Apache-2.0 */

#include <kobox2/closure.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);      \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static void *test_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void test_deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static kb2_status_t create_builder(kb2_closure_builder_t **builder_out) {
    uint8_t digest[KB2_DIGEST_SIZE];

    memset(digest, 0x11, sizeof(digest));
    return kb2_closure_builder_create(test_allocate,
                                      test_deallocate,
                                      NULL,
                                      digest,
                                      sizeof(digest),
                                      builder_out);
}

static kb2_status_t add_node(kb2_closure_builder_t *builder,
                             uint32_t node_id,
                             kb2_artifact_kind_t kind,
                             const char *namespace_name,
                             int root) {
    static const char *const lifecycle[] = {"init", "quiesce", "cleanup"};
    uint8_t digest[KB2_DIGEST_SIZE];
    size_t index;
    kb2_status_t status;

    memset(digest, (int)node_id, sizeof(digest));
    status = kb2_closure_builder_add_artifact(builder,
                                              node_id,
                                              kind,
                                              digest,
                                              sizeof(digest),
                                              namespace_name,
                                              strlen(namespace_name));
    if (status != KB2_STATUS_OK) {
        return status;
    }
    status = kb2_closure_builder_set_lifecycle(builder,
                                               node_id,
                                               lifecycle[0],
                                               strlen(lifecycle[0]),
                                               lifecycle[1],
                                               strlen(lifecycle[1]),
                                               lifecycle[2],
                                               strlen(lifecycle[2]));
    if (status != KB2_STATUS_OK) {
        return status;
    }
    for (index = 0; index < sizeof(lifecycle) / sizeof(lifecycle[0]); ++index) {
        status = kb2_closure_builder_add_export(
            builder, node_id, lifecycle[index], strlen(lifecycle[index]), KB2_SYMBOL_FUNCTION);
        if (status != KB2_STATUS_OK) {
            return status;
        }
    }
    return root ? kb2_closure_builder_mark_root(builder, node_id) : KB2_STATUS_OK;
}

static int test_valid_closure(void) {
    kb2_closure_builder_t *builder = NULL;
    kb2_closure_t *closure = NULL;
    uint8_t digest[KB2_DIGEST_SIZE];
    uint32_t node_id;
    uint32_t slot_id;
    uint32_t minimum_count;
    uint32_t maximum_count;
    uint32_t flags;
    uint64_t required_rights;
    uint64_t maximum_rights;
    kb2_artifact_kind_t artifact_kind;
    kb2_resource_type_t resource_type;
    int is_root;

    CHECK(create_builder(&builder) == KB2_STATUS_OK);
    CHECK(add_node(builder, 20, KB2_ARTIFACT_RELOCATABLE_MODULE, "driver", 1) ==
          KB2_STATUS_OK);
    CHECK(add_node(builder, 10, KB2_ARTIFACT_SHARED_PROVIDER, "core", 0) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_export(
              builder, 10, "service", sizeof("service") - 1u, KB2_SYMBOL_FUNCTION) ==
          KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_dependency(builder, 20, 10) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_import(builder,
                                         20,
                                         "service",
                                         sizeof("service") - 1u,
                                         10,
                                         "service",
                                         sizeof("service") - 1u,
                                         KB2_SYMBOL_FUNCTION,
                                         0) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_import(builder,
                                         20,
                                         "runtime_log",
                                         sizeof("runtime_log") - 1u,
                                         0,
                                         "runtime_log",
                                         sizeof("runtime_log") - 1u,
                                         KB2_SYMBOL_FUNCTION,
                                         0) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_resource(builder,
                                           7,
                                           KB2_RESOURCE_DEVICE,
                                           1,
                                           1,
                                           KB2_DEVICE_RIGHT_COMMAND,
                                           KB2_DEVICE_RIGHT_COMMAND | KB2_DEVICE_RIGHT_MAP,
                                           KB2_RESOURCE_REQUIRED |
                                               KB2_RESOURCE_RESET_REQUIRED) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_bind_resource(builder, 7, 20) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_resource(builder,
                                           8,
                                           KB2_RESOURCE_MEMORY,
                                           1,
                                           4,
                                           KB2_MEMORY_RIGHT_READ,
                                           KB2_MEMORY_RIGHT_READ | KB2_MEMORY_RIGHT_WRITE |
                                               KB2_MEMORY_RIGHT_MAP | KB2_MEMORY_RIGHT_DMA,
                                           KB2_RESOURCE_REQUIRED | KB2_RESOURCE_SHARED) ==
          KB2_STATUS_OK);
    CHECK(kb2_closure_builder_bind_resource(builder, 8, 10) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_bind_resource(builder, 8, 20) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_OK);
    kb2_closure_builder_destroy(builder);
    builder = NULL;

    CHECK(kb2_closure_artifact_count(closure) == 2);
    CHECK(kb2_closure_dependency_count(closure) == 1);
    CHECK(kb2_closure_export_count(closure) == 7);
    CHECK(kb2_closure_import_count(closure) == 2);
    CHECK(kb2_closure_resource_count(closure) == 2);
    CHECK(kb2_closure_resource_binding_count(closure) == 3);
    CHECK(kb2_closure_copy_manifest_digest(closure, digest, sizeof(digest)) ==
          KB2_STATUS_OK);
    CHECK(digest[0] == 0x11 && digest[KB2_DIGEST_SIZE - 1] == 0x11);
    CHECK(kb2_closure_artifact(closure,
                               1,
                               &node_id,
                               &artifact_kind,
                               &is_root,
                               digest,
                               sizeof(digest)) == KB2_STATUS_OK);
    CHECK(node_id == 20 && artifact_kind == KB2_ARTIFACT_RELOCATABLE_MODULE && is_root);
    CHECK(kb2_closure_resource(closure,
                               0,
                               &slot_id,
                               &resource_type,
                               &minimum_count,
                               &maximum_count,
                               &required_rights,
                               &maximum_rights,
                               &flags) == KB2_STATUS_OK);
    CHECK(slot_id == 7 && resource_type == KB2_RESOURCE_DEVICE && minimum_count == 1 &&
          maximum_count == 1 && required_rights == KB2_DEVICE_RIGHT_COMMAND &&
          maximum_rights == (KB2_DEVICE_RIGHT_COMMAND | KB2_DEVICE_RIGHT_MAP) &&
          flags == (KB2_RESOURCE_REQUIRED | KB2_RESOURCE_RESET_REQUIRED));
    CHECK(kb2_closure_resource_binding(closure, 2, &slot_id, &node_id) == KB2_STATUS_OK);
    CHECK(slot_id == 8 && node_id == 20);
    kb2_closure_destroy(closure);
    return 0;
}

static int test_cycle_rejected(void) {
    kb2_closure_builder_t *builder = NULL;
    kb2_closure_t *closure = NULL;

    CHECK(create_builder(&builder) == KB2_STATUS_OK);
    CHECK(add_node(builder, 1, KB2_ARTIFACT_RELOCATABLE_MODULE, "root", 1) ==
          KB2_STATUS_OK);
    CHECK(add_node(builder, 2, KB2_ARTIFACT_SHARED_PROVIDER, "provider", 0) ==
          KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_dependency(builder, 1, 2) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_dependency(builder, 2, 1) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_INVALID_CONFIGURATION);
    CHECK(closure == NULL);
    kb2_closure_builder_destroy(builder);
    return 0;
}

static int test_unreachable_node_rejected(void) {
    kb2_closure_builder_t *builder = NULL;
    kb2_closure_t *closure = NULL;

    CHECK(create_builder(&builder) == KB2_STATUS_OK);
    CHECK(add_node(builder, 1, KB2_ARTIFACT_RELOCATABLE_MODULE, "root", 1) ==
          KB2_STATUS_OK);
    CHECK(add_node(builder, 2, KB2_ARTIFACT_SHARED_PROVIDER, "orphan", 0) ==
          KB2_STATUS_OK);
    CHECK(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_INVALID_CONFIGURATION);
    kb2_closure_builder_destroy(builder);
    return 0;
}

static int test_symbol_binding_validation(void) {
    kb2_closure_builder_t *builder = NULL;
    kb2_closure_t *closure = NULL;

    CHECK(create_builder(&builder) == KB2_STATUS_OK);
    CHECK(add_node(builder, 1, KB2_ARTIFACT_RELOCATABLE_MODULE, "root", 1) ==
          KB2_STATUS_OK);
    CHECK(add_node(builder, 2, KB2_ARTIFACT_SHARED_PROVIDER, "provider", 0) ==
          KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_dependency(builder, 1, 2) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_import(builder,
                                         1,
                                         "missing",
                                         sizeof("missing") - 1u,
                                         2,
                                         "missing",
                                         sizeof("missing") - 1u,
                                         KB2_SYMBOL_FUNCTION,
                                         0) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_INVALID_CONFIGURATION);
    kb2_closure_builder_destroy(builder);

    CHECK(create_builder(&builder) == KB2_STATUS_OK);
    CHECK(add_node(builder, 1, KB2_ARTIFACT_RELOCATABLE_MODULE, "root", 1) ==
          KB2_STATUS_OK);
    CHECK(add_node(builder, 2, KB2_ARTIFACT_SHARED_PROVIDER, "provider", 0) ==
          KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_dependency(builder, 1, 2) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_import(builder,
                                         1,
                                         "optional",
                                         sizeof("optional") - 1u,
                                         2,
                                         "optional",
                                         sizeof("optional") - 1u,
                                         KB2_SYMBOL_FUNCTION,
                                         KB2_IMPORT_OPTIONAL) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_OK);
    kb2_closure_destroy(closure);
    kb2_closure_builder_destroy(builder);
    return 0;
}

static int test_resource_validation(void) {
    kb2_closure_builder_t *builder = NULL;
    kb2_closure_t *closure = NULL;

    CHECK(create_builder(&builder) == KB2_STATUS_OK);
    CHECK(add_node(builder, 1, KB2_ARTIFACT_RELOCATABLE_MODULE, "root", 1) ==
          KB2_STATUS_OK);
    CHECK(add_node(builder, 2, KB2_ARTIFACT_SHARED_PROVIDER, "provider", 0) ==
          KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_dependency(builder, 1, 2) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_resource(builder,
                                           1,
                                           KB2_RESOURCE_DEVICE,
                                           1,
                                           1,
                                           KB2_DEVICE_RIGHT_COMMAND,
                                           KB2_DEVICE_RIGHT_COMMAND,
                                           KB2_RESOURCE_REQUIRED) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_bind_resource(builder, 1, 1) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_bind_resource(builder, 1, 2) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_INVALID_CONFIGURATION);
    CHECK(kb2_closure_builder_add_resource(builder,
                                           2,
                                           KB2_RESOURCE_DEVICE,
                                           1,
                                           1,
                                           KB2_DEVICE_RIGHT_COMMAND,
                                           UINT64_C(1) << 40,
                                           KB2_RESOURCE_REQUIRED) ==
          KB2_STATUS_INVALID_CONFIGURATION);
    kb2_closure_builder_destroy(builder);
    return 0;
}

int main(void) {
    CHECK(test_valid_closure() == 0);
    CHECK(test_cycle_rejected() == 0);
    CHECK(test_unreachable_node_rejected() == 0);
    CHECK(test_symbol_binding_validation() == 0);
    CHECK(test_resource_validation() == 0);
    return 0;
}
