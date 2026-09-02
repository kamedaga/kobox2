/* SPDX-License-Identifier: Apache-2.0 */

#include "test_closure.h"

#include <kobox2/closure.h>
#include <kobox2/protocol.h>

#include <stdlib.h>
#include <string.h>

static void *test_allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void test_deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

int kb2_test_configure_closure_with_reset(kb2_controller_t *controller, int reset_required) {
    kb2_closure_builder_t *builder = NULL;
    kb2_closure_t *closure = NULL;
    uint8_t digest[KB2_DIGEST_SIZE];
    uint8_t interface_digest[KB2_DIGEST_SIZE];
    int result = 0;

    if (kb2_protocol_copy_schema_digest(interface_digest, sizeof(interface_digest)) !=
        KB2_PROTOCOL_OK) {
        goto finish;
    }
    memset(digest, 0x41, sizeof(digest));
    if (kb2_closure_builder_create(test_allocate,
                                   test_deallocate,
                                   NULL,
                                   digest,
                                   sizeof(digest),
                                   &builder) != KB2_STATUS_OK) {
        goto finish;
    }
    memset(digest, 0x42, sizeof(digest));
    if (kb2_closure_builder_add_artifact(builder,
                                         1,
                                         KB2_ARTIFACT_RELOCATABLE_MODULE,
                                         digest,
                                         sizeof(digest),
                                         "fixture",
                                         sizeof("fixture") - 1u) != KB2_STATUS_OK ||
        kb2_closure_builder_set_lifecycle(builder,
                                          1,
                                          "fixture_init",
                                          sizeof("fixture_init") - 1u,
                                          "fixture_quiesce",
                                          sizeof("fixture_quiesce") - 1u,
                                          "fixture_cleanup",
                                          sizeof("fixture_cleanup") - 1u) != KB2_STATUS_OK ||
        kb2_closure_builder_add_export(builder,
                                       1,
                                       "fixture_init",
                                       sizeof("fixture_init") - 1u,
                                       KB2_SYMBOL_FUNCTION) != KB2_STATUS_OK ||
        kb2_closure_builder_add_export(builder,
                                       1,
                                       "fixture_quiesce",
                                       sizeof("fixture_quiesce") - 1u,
                                       KB2_SYMBOL_FUNCTION) != KB2_STATUS_OK ||
        kb2_closure_builder_add_export(builder,
                                       1,
                                       "fixture_cleanup",
                                       sizeof("fixture_cleanup") - 1u,
                                       KB2_SYMBOL_FUNCTION) != KB2_STATUS_OK ||
        kb2_closure_builder_mark_root(builder, 1) != KB2_STATUS_OK ||
        kb2_closure_builder_add_resource(
            builder,
            1,
            KB2_RESOURCE_CHANNEL,
            interface_digest,
            sizeof(interface_digest),
            1,
            1,
            KB2_CHANNEL_RIGHT_SEND | KB2_CHANNEL_RIGHT_RECEIVE,
            KB2_CHANNEL_RIGHT_SEND | KB2_CHANNEL_RIGHT_RECEIVE,
            KB2_RESOURCE_REQUIRED |
                (reset_required ? KB2_RESOURCE_RESET_REQUIRED : 0u)) != KB2_STATUS_OK ||
        kb2_closure_builder_bind_resource(builder, 1, 1) != KB2_STATUS_OK ||
        kb2_closure_builder_seal(builder, &closure) != KB2_STATUS_OK ||
        kb2_controller_set_closure(controller, closure) != KB2_STATUS_OK) {
        goto finish;
    }
    result = 1;

finish:
    kb2_closure_destroy(closure);
    kb2_closure_builder_destroy(builder);
    return result;
}

int kb2_test_configure_closure(kb2_controller_t *controller) {
    return kb2_test_configure_closure_with_reset(controller, 1);
}

static int add_fixture_export(kb2_closure_builder_t *builder,
                              uint32_t node_id,
                              const char *name) {
    return kb2_closure_builder_add_export(
               builder, node_id, name, strlen(name), KB2_SYMBOL_FUNCTION) == KB2_STATUS_OK;
}

int kb2_test_configure_fixture_closure(kb2_controller_t *controller,
                                       const uint8_t manifest_digest[KB2_DIGEST_SIZE],
                                       const uint8_t core_digest[KB2_DIGEST_SIZE],
                                       const uint8_t provider_digest[KB2_DIGEST_SIZE],
                                       const uint8_t consumer_digest[KB2_DIGEST_SIZE]) {
    kb2_closure_builder_t *builder = NULL;
    kb2_closure_t *closure = NULL;
    uint8_t interface_digest[KB2_DIGEST_SIZE];
    int result = 0;

    if (controller == NULL || manifest_digest == NULL || core_digest == NULL ||
        provider_digest == NULL || consumer_digest == NULL ||
        kb2_protocol_copy_schema_digest(interface_digest, sizeof(interface_digest)) !=
            KB2_PROTOCOL_OK ||
        kb2_closure_builder_create(test_allocate,
                                   test_deallocate,
                                   NULL,
                                   manifest_digest,
                                   KB2_DIGEST_SIZE,
                                   &builder) != KB2_STATUS_OK) {
        goto finish;
    }
    if (kb2_closure_builder_add_artifact(builder,
                                         1,
                                         KB2_ARTIFACT_SHARED_PROVIDER,
                                         core_digest,
                                         KB2_DIGEST_SIZE,
                                         "fixture_core",
                                         sizeof("fixture_core") - 1u) != KB2_STATUS_OK ||
        kb2_closure_builder_set_lifecycle(builder,
                                          1,
                                          "kobox_fixture_core_init",
                                          sizeof("kobox_fixture_core_init") - 1u,
                                          "kobox_fixture_core_quiesce",
                                          sizeof("kobox_fixture_core_quiesce") - 1u,
                                          "kobox_fixture_core_cleanup",
                                          sizeof("kobox_fixture_core_cleanup") - 1u) !=
            KB2_STATUS_OK ||
        kb2_closure_builder_add_artifact(builder,
                                         2,
                                         KB2_ARTIFACT_RELOCATABLE_MODULE,
                                         provider_digest,
                                         KB2_DIGEST_SIZE,
                                         "fixture_provider",
                                         sizeof("fixture_provider") - 1u) != KB2_STATUS_OK ||
        kb2_closure_builder_set_lifecycle(builder,
                                          2,
                                          "kobox_fixture_provider_init",
                                          sizeof("kobox_fixture_provider_init") - 1u,
                                          "kobox_fixture_provider_quiesce",
                                          sizeof("kobox_fixture_provider_quiesce") - 1u,
                                          "kobox_fixture_provider_cleanup",
                                          sizeof("kobox_fixture_provider_cleanup") - 1u) !=
            KB2_STATUS_OK ||
        kb2_closure_builder_add_artifact(builder,
                                         3,
                                         KB2_ARTIFACT_RELOCATABLE_MODULE,
                                         consumer_digest,
                                         KB2_DIGEST_SIZE,
                                         "fixture_consumer",
                                         sizeof("fixture_consumer") - 1u) != KB2_STATUS_OK ||
        kb2_closure_builder_set_lifecycle(builder,
                                          3,
                                          "kobox_fixture_consumer_init",
                                          sizeof("kobox_fixture_consumer_init") - 1u,
                                          "kobox_fixture_consumer_quiesce",
                                          sizeof("kobox_fixture_consumer_quiesce") - 1u,
                                          "kobox_fixture_consumer_cleanup",
                                          sizeof("kobox_fixture_consumer_cleanup") - 1u) !=
            KB2_STATUS_OK ||
        kb2_closure_builder_mark_root(builder, 3) != KB2_STATUS_OK ||
        kb2_closure_builder_add_dependency(builder, 2, 1) != KB2_STATUS_OK ||
        kb2_closure_builder_add_dependency(builder, 3, 2) != KB2_STATUS_OK ||
        !add_fixture_export(builder, 1, "kobox_fixture_core_cleanup") ||
        !add_fixture_export(builder, 1, "kobox_fixture_core_init") ||
        !add_fixture_export(builder, 1, "kobox_fixture_lifecycle_snapshot") ||
        kb2_closure_builder_add_export(
            builder,
            1,
            "kobox_fixture_core_operations",
            sizeof("kobox_fixture_core_operations") - 1u,
            KB2_SYMBOL_OBJECT) != KB2_STATUS_OK ||
        !add_fixture_export(builder, 1, "kobox_fixture_core_quiesce") ||
        !add_fixture_export(builder, 2, "kobox_fixture_provider_add") ||
        !add_fixture_export(builder, 2, "kobox_fixture_provider_cleanup") ||
        !add_fixture_export(builder, 2, "kobox_fixture_provider_init") ||
        !add_fixture_export(builder, 2, "kobox_fixture_provider_quiesce") ||
        !add_fixture_export(builder, 3, "kobox_fixture_consumer_cleanup") ||
        !add_fixture_export(builder, 3, "kobox_fixture_consumer_init") ||
        !add_fixture_export(builder, 3, "kobox_fixture_consumer_quiesce") ||
        !add_fixture_export(builder, 3, "kobox_fixture_consumer_run") ||
        kb2_closure_builder_add_import(builder,
                                       3,
                                       "kobox_fixture_provider_add",
                                       sizeof("kobox_fixture_provider_add") - 1u,
                                       2,
                                       "kobox_fixture_provider_add",
                                       sizeof("kobox_fixture_provider_add") - 1u,
                                       KB2_SYMBOL_FUNCTION,
                                       0) != KB2_STATUS_OK ||
        kb2_closure_builder_add_resource(builder,
                                         1,
                                         KB2_RESOURCE_CHANNEL,
                                         interface_digest,
                                         sizeof(interface_digest),
                                         1,
                                         1,
                                         KB2_CHANNEL_RIGHT_SEND | KB2_CHANNEL_RIGHT_RECEIVE,
                                         KB2_CHANNEL_RIGHT_SEND | KB2_CHANNEL_RIGHT_RECEIVE,
                                         KB2_RESOURCE_REQUIRED | KB2_RESOURCE_RESET_REQUIRED) !=
            KB2_STATUS_OK ||
        kb2_closure_builder_bind_resource(builder, 1, 3) != KB2_STATUS_OK ||
        kb2_closure_builder_seal(builder, &closure) != KB2_STATUS_OK ||
        kb2_controller_set_closure(controller, closure) != KB2_STATUS_OK) {
        goto finish;
    }
    result = 1;

finish:
    kb2_closure_destroy(closure);
    kb2_closure_builder_destroy(builder);
    return result;
}
