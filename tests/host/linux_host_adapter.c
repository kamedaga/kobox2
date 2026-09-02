/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "linux_host_adapter.h"

#include <kobox2/closure.h>
#include <kobox2/closure_manifest.h>
#include <kobox2/protocol.h>
#include <kobox2/resource_grant.h>
#include <kobox2/sha256.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/pidfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define KB2_TEST_MINIMUM_SHARED_MEMORY_SIZE 16384u
#define KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS 5000

static void kb2_test_close(int *file_descriptor) {
    if (*file_descriptor >= 0) {
        close(*file_descriptor);
        *file_descriptor = -1;
    }
}

static void kb2_test_host_release_local_resources(kb2_test_host_t *host) {
    size_t index;

    if (host->shared_memory != MAP_FAILED && host->shared_memory != NULL) {
        munmap(host->shared_memory, host->shared_memory_size);
    }
    host->shared_memory = MAP_FAILED;
    host->shared_memory_size = 0;
    host->channel_descriptor_size = 0;
    kb2_test_close(&host->shared_memory_fd);
    kb2_test_close(&host->manifest_fd);
    kb2_test_close(&host->grant_fd);
    for (index = 0; index < KB2_TEST_ARTIFACT_COUNT; ++index) {
        kb2_test_close(&host->artifact_fds[index]);
        host->artifact_sizes[index] = 0;
        memset(host->artifact_digests[index], 0, sizeof(host->artifact_digests[index]));
    }
    host->manifest_size = 0;
    memset(host->manifest_digest, 0, sizeof(host->manifest_digest));
    host->grant_size = 0;
    memset(host->grant_digest, 0, sizeof(host->grant_digest));
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        kb2_test_close(&host->notification_fds[index]);
        host->notification_ids[index] = 0;
    }
    kb2_test_close(&host->bootstrap_socket);
    kb2_test_close(&host->process_fd);
    host->generation = 0;
    host->resource_set_id = 0;
    host->resources_transferred = 0;
    host->resources_revoked = 0;
    host->abnormal_exit_allowed = 0;
    host->process_exited = 0;
}

static int kb2_test_wait_process(pid_t process_id,
                                 int *status_out,
                                 int timeout_milliseconds) {
    pid_t result;
    int wait_milliseconds;

    while (timeout_milliseconds >= 0) {
        do {
            result = waitpid(process_id, status_out, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result == process_id) {
            return 1;
        }
        if (result < 0 || timeout_milliseconds == 0) {
            return 0;
        }
        wait_milliseconds = timeout_milliseconds < 10 ? timeout_milliseconds : 10;
        do {
            result = poll(NULL, 0, wait_milliseconds);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            return 0;
        }
        timeout_milliseconds -= wait_milliseconds;
    }
    return 0;
}

static int kb2_test_wait_process_fd(int process_fd, int timeout_milliseconds) {
    struct pollfd descriptor = {
        .fd = process_fd,
        .events = POLLIN,
    };
    int result;

    do {
        result = poll(&descriptor, 1, timeout_milliseconds);
    } while (result < 0 && errno == EINTR);
    return result == 1 && (descriptor.revents & POLLIN) != 0;
}

static int kb2_test_write_all(int descriptor, const void *data, size_t size) {
    const uint8_t *bytes = data;

    while (size != 0) {
        ssize_t written;

        do {
            written = write(descriptor, bytes, size);
        } while (written < 0 && errno == EINTR);
        if (written <= 0) {
            return 0;
        }
        bytes += (size_t)written;
        size -= (size_t)written;
    }
    return 1;
}

static int kb2_test_copy_artifact(const char *path,
                                  const char *name,
                                  int *descriptor_out,
                                  uint64_t *size_out,
                                  uint8_t digest_out[KB2_SHA256_DIGEST_SIZE]) {
    kb2_sha256_context_t digest_context;
    struct stat status;
    uint8_t buffer[16384];
    int source = -1;
    int destination = -1;
    int result = 0;

    source = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source < 0 || fstat(source, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_size <= 0 || (uint64_t)status.st_size > SIZE_MAX) {
        goto finish;
    }
    destination = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (destination < 0 || ftruncate(destination, status.st_size) != 0) {
        goto finish;
    }
    kb2_sha256_initialize(&digest_context);
    for (;;) {
        ssize_t bytes_read;

        do {
            bytes_read = read(source, buffer, sizeof(buffer));
        } while (bytes_read < 0 && errno == EINTR);
        if (bytes_read < 0) {
            goto finish;
        }
        if (bytes_read == 0) {
            break;
        }
        kb2_sha256_update(&digest_context, buffer, (size_t)bytes_read);
        if (!kb2_test_write_all(destination, buffer, (size_t)bytes_read)) {
            goto finish;
        }
    }
    kb2_sha256_finish(&digest_context, digest_out);
    if (lseek(destination, 0, SEEK_SET) != 0 ||
        fcntl(destination,
              F_ADD_SEALS,
              F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
        goto finish;
    }
    *descriptor_out = destination;
    *size_out = (uint64_t)status.st_size;
    destination = -1;
    result = 1;

finish:
    if (source >= 0) {
        close(source);
    }
    if (destination >= 0) {
        close(destination);
    }
    return result;
}

#define KB2_TEST_STRING(value) {(value), (uint32_t)(sizeof(value) - 1u)}

static int kb2_test_make_manifest(kb2_test_host_t *host) {
    kb2_closure_manifest_artifact_t artifacts[KB2_TEST_ARTIFACT_COUNT] = {
        {
            .node_id = 1,
            .kind = KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
            .namespace_name = KB2_TEST_STRING("fixture_core"),
            .init_symbol = KB2_TEST_STRING("kobox_fixture_core_init"),
            .quiesce_symbol = KB2_TEST_STRING("kobox_fixture_core_quiesce"),
            .cleanup_symbol = KB2_TEST_STRING("kobox_fixture_core_cleanup"),
        },
        {
            .node_id = 2,
            .kind = KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE,
            .namespace_name = KB2_TEST_STRING("fixture_provider"),
            .init_symbol = KB2_TEST_STRING("kobox_fixture_provider_init"),
            .quiesce_symbol = KB2_TEST_STRING("kobox_fixture_provider_quiesce"),
            .cleanup_symbol = KB2_TEST_STRING("kobox_fixture_provider_cleanup"),
        },
        {
            .node_id = 3,
            .kind = KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE,
            .flags = KB2_CLOSURE_ARTIFACT_FLAG_ROOT,
            .namespace_name = KB2_TEST_STRING("fixture_consumer"),
            .init_symbol = KB2_TEST_STRING("kobox_fixture_consumer_init"),
            .quiesce_symbol = KB2_TEST_STRING("kobox_fixture_consumer_quiesce"),
            .cleanup_symbol = KB2_TEST_STRING("kobox_fixture_consumer_cleanup"),
        },
    };
    const kb2_closure_manifest_dependency_t dependencies[] = {{2, 1}, {3, 2}};
    const kb2_closure_manifest_symbol_t exports[] = {
        {1, KB2_CLOSURE_SYMBOL_FUNCTION, KB2_TEST_STRING("kobox_fixture_core_cleanup")},
        {1, KB2_CLOSURE_SYMBOL_FUNCTION, KB2_TEST_STRING("kobox_fixture_core_init")},
        {1,
         KB2_CLOSURE_SYMBOL_OBJECT,
         KB2_TEST_STRING("kobox_fixture_core_operations")},
        {1, KB2_CLOSURE_SYMBOL_FUNCTION, KB2_TEST_STRING("kobox_fixture_core_quiesce")},
        {1,
         KB2_CLOSURE_SYMBOL_FUNCTION,
         KB2_TEST_STRING("kobox_fixture_lifecycle_snapshot")},
        {2, KB2_CLOSURE_SYMBOL_FUNCTION, KB2_TEST_STRING("kobox_fixture_provider_add")},
        {2,
         KB2_CLOSURE_SYMBOL_FUNCTION,
         KB2_TEST_STRING("kobox_fixture_provider_cleanup")},
        {2,
         KB2_CLOSURE_SYMBOL_FUNCTION,
         KB2_TEST_STRING("kobox_fixture_provider_init")},
        {2,
         KB2_CLOSURE_SYMBOL_FUNCTION,
         KB2_TEST_STRING("kobox_fixture_provider_quiesce")},
        {3,
         KB2_CLOSURE_SYMBOL_FUNCTION,
         KB2_TEST_STRING("kobox_fixture_consumer_cleanup")},
        {3,
         KB2_CLOSURE_SYMBOL_FUNCTION,
         KB2_TEST_STRING("kobox_fixture_consumer_init")},
        {3,
         KB2_CLOSURE_SYMBOL_FUNCTION,
         KB2_TEST_STRING("kobox_fixture_consumer_quiesce")},
        {3, KB2_CLOSURE_SYMBOL_FUNCTION, KB2_TEST_STRING("kobox_fixture_consumer_run")},
    };
    const kb2_closure_manifest_import_t imports[] = {
        {3,
         2,
         KB2_CLOSURE_SYMBOL_FUNCTION,
         0,
         KB2_TEST_STRING("kobox_fixture_provider_add"),
         KB2_TEST_STRING("kobox_fixture_provider_add")},
    };
    kb2_closure_manifest_resource_t resources[] = {
        {
            .slot_id = 1,
            .type = KB2_RESOURCE_CHANNEL,
            .minimum_count = 1,
            .maximum_count = 1,
            .required_rights = KB2_CHANNEL_RIGHT_SEND | KB2_CHANNEL_RIGHT_RECEIVE,
            .maximum_rights = KB2_CHANNEL_RIGHT_SEND | KB2_CHANNEL_RIGHT_RECEIVE,
            .flags = KB2_RESOURCE_REQUIRED | KB2_RESOURCE_RESET_REQUIRED,
        },
    };
    const kb2_closure_manifest_binding_t bindings[] = {{1, 3}};
    const kb2_closure_manifest_source_t source = {
        .artifacts = artifacts,
        .artifact_count = sizeof(artifacts) / sizeof(artifacts[0]),
        .dependencies = dependencies,
        .dependency_count = sizeof(dependencies) / sizeof(dependencies[0]),
        .exports = exports,
        .export_count = sizeof(exports) / sizeof(exports[0]),
        .imports = imports,
        .import_count = sizeof(imports) / sizeof(imports[0]),
        .resources = resources,
        .resource_count = sizeof(resources) / sizeof(resources[0]),
        .bindings = bindings,
        .binding_count = sizeof(bindings) / sizeof(bindings[0]),
    };
    uint8_t *encoded = NULL;
    kb2_closure_manifest_t decoded;
    size_t encoded_size;
    size_t actual_size;
    size_t index;
    int descriptor = -1;
    int result = 0;

    for (index = 0; index < KB2_TEST_ARTIFACT_COUNT; ++index) {
        artifacts[index].content_size = host->artifact_sizes[index];
        memcpy(artifacts[index].content_digest,
               host->artifact_digests[index],
               sizeof(artifacts[index].content_digest));
    }
    if (kb2_protocol_copy_schema_digest(resources[0].interface_schema_digest,
                                        sizeof(resources[0].interface_schema_digest)) !=
            KB2_PROTOCOL_OK ||
        kb2_closure_manifest_encoded_size(&source, &encoded_size) != KB2_PROTOCOL_OK) {
        goto finish;
    }
    encoded = malloc(encoded_size);
    if (encoded == NULL ||
        kb2_closure_manifest_encode(encoded, encoded_size, &actual_size, &source) !=
            KB2_PROTOCOL_OK ||
        actual_size != encoded_size ||
        kb2_closure_manifest_decode(encoded, encoded_size, &decoded) != KB2_PROTOCOL_OK ||
        kb2_closure_manifest_artifact_count(&decoded) != KB2_TEST_ARTIFACT_COUNT) {
        goto finish;
    }
    kb2_sha256(encoded, encoded_size, host->manifest_digest);
    descriptor = memfd_create("kobox2-test-manifest", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0 || ftruncate(descriptor, (off_t)encoded_size) != 0 ||
        !kb2_test_write_all(descriptor, encoded, encoded_size) ||
        lseek(descriptor, 0, SEEK_SET) != 0 ||
        fcntl(descriptor,
              F_ADD_SEALS,
              F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
        goto finish;
    }
    host->manifest_fd = descriptor;
    host->manifest_size = encoded_size;
    descriptor = -1;
    result = 1;

finish:
    free(encoded);
    if (descriptor >= 0) {
        close(descriptor);
    }
    return result;
}

static int kb2_test_make_closure_package(kb2_test_host_t *host) {
    size_t index;

    for (index = 0; index < KB2_TEST_ARTIFACT_COUNT; ++index) {
        static const char *const names[KB2_TEST_ARTIFACT_COUNT] = {
            "kobox2-test-core",
            "kobox2-test-provider",
            "kobox2-test-consumer",
        };

        if (!kb2_test_copy_artifact(host->artifact_paths[index],
                                    names[index],
                                    &host->artifact_fds[index],
                                    &host->artifact_sizes[index],
                                    host->artifact_digests[index])) {
            return 0;
        }
    }
    return kb2_test_make_manifest(host);
}

static int kb2_test_make_grant(kb2_test_host_t *host) {
    kb2_resource_grant_slot_source_t slot = {
        .slot_id = 1,
        .resource_type = KB2_CLOSURE_RESOURCE_CHANNEL,
        .state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
    };
    const kb2_resource_grant_object_source_t object = {
        .slot_id = 1,
        .granted_rights =
            KB2_CLOSURE_CHANNEL_RIGHT_SEND | KB2_CLOSURE_CHANNEL_RIGHT_RECEIVE,
    };
    kb2_resource_grant_object_source_t generation_object = object;
    kb2_resource_grant_handle_binding_t binding = {
        .role = KB2_PROTOCOL_NATIVE_HANDLE_ROLE_MEMORY,
        .transfer_handle_index = 0,
    };
    kb2_resource_grant_source_t source = {
        .generation = host->generation,
        .slots = &slot,
        .slot_count = 1,
        .objects = &generation_object,
        .object_count = 1,
        .handle_bindings = &binding,
        .handle_binding_count = 1,
    };
    kb2_resource_grant_t decoded;
    uint8_t *encoded = NULL;
    size_t encoded_size;
    size_t actual_size;
    int descriptor = -1;
    int result = 0;

    generation_object.object_id = host->generation;
    binding.object_id = generation_object.object_id;
    memcpy(source.closure_manifest_digest,
           host->manifest_digest,
           sizeof(source.closure_manifest_digest));
    if (kb2_protocol_copy_schema_digest(slot.interface_schema_digest,
                                        sizeof(slot.interface_schema_digest)) !=
            KB2_PROTOCOL_OK ||
        kb2_resource_grant_encoded_size(&source, &encoded_size) != KB2_PROTOCOL_OK) {
        goto finish;
    }
    encoded = malloc(encoded_size);
    if (encoded == NULL ||
        kb2_resource_grant_encode(
            encoded, encoded_size, &actual_size, &source) != KB2_PROTOCOL_OK ||
        actual_size != encoded_size ||
        kb2_resource_grant_decode(encoded, encoded_size, &decoded) != KB2_PROTOCOL_OK) {
        goto finish;
    }
    kb2_sha256(encoded, encoded_size, host->grant_digest);
    descriptor = memfd_create("kobox2-test-grant", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (descriptor < 0 || ftruncate(descriptor, (off_t)encoded_size) != 0 ||
        !kb2_test_write_all(descriptor, encoded, encoded_size) ||
        lseek(descriptor, 0, SEEK_SET) != 0 ||
        fcntl(descriptor,
              F_ADD_SEALS,
              F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
        goto finish;
    }
    host->grant_fd = descriptor;
    host->grant_size = encoded_size;
    descriptor = -1;
    result = 1;

finish:
    free(encoded);
    if (descriptor >= 0) {
        close(descriptor);
    }
    return result;
}

int kb2_test_host_initialize(kb2_test_host_t *host,
                             const char *sandbox_path,
                             const char *core_path,
                             const char *provider_path,
                             const char *consumer_path) {
    size_t index;

    if (host == NULL || sandbox_path == NULL || sandbox_path[0] == '\0' || core_path == NULL ||
        core_path[0] == '\0' || provider_path == NULL || provider_path[0] == '\0' ||
        consumer_path == NULL || consumer_path[0] == '\0') {
        return 0;
    }
    memset(host, 0, sizeof(*host));
    host->sandbox_path = sandbox_path;
    host->artifact_paths[0] = core_path;
    host->artifact_paths[1] = provider_path;
    host->artifact_paths[2] = consumer_path;
    host->shared_memory = MAP_FAILED;
    host->process_id = -1;
    host->process_fd = -1;
    host->bootstrap_socket = -1;
    host->shared_memory_fd = -1;
    host->manifest_fd = -1;
    host->grant_fd = -1;
    for (index = 0; index < KB2_TEST_ARTIFACT_COUNT; ++index) {
        host->artifact_fds[index] = -1;
    }
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        host->notification_fds[index] = -1;
    }
    if (!kb2_test_make_closure_package(host)) {
        kb2_test_host_release_local_resources(host);
        return 0;
    }
    return 1;
}

void kb2_test_host_destroy(kb2_test_host_t *host) {
    int status;

    if (host == NULL) {
        return;
    }
    if (host->process_id > 0) {
        kill(host->process_id, SIGKILL);
        kb2_test_wait_process(
            host->process_id, &status, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS);
        host->process_id = -1;
    }
    kb2_test_host_release_local_resources(host);
    memset(host, 0, sizeof(*host));
}

static kb2_status_t kb2_test_host_make_channel(kb2_test_host_t *host,
                                               const kb2_action_t *action) {
    kb2_protocol_channel_t channel = {
        .feature_bits = KB2_PROTOCOL_TRANSPORT_FEATURES_REQUIRED,
        .generation = kb2_action_generation(action),
        .protocol_id = KB2_TEST_PROTOCOL_ID,
        .flags = KB2_PROTOCOL_CHANNEL_FLAG_MANAGEMENT,
    };
    size_t event_index;
    uint64_t outstanding;
    size_t index;
    int notify;

    memset(host->queues, 0, sizeof(host->queues));
    host->regions[0] = (kb2_protocol_region_t){
        .region_id = 1,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ | KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .transport_base = KB2_TEST_TRANSPORT_BASE,
        .length = KB2_TEST_EVENT_BUFFER_OFFSET,
    };
    host->regions[1] = (kb2_protocol_region_t){
        .region_id = 2,
        .rights = KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .transport_base = KB2_TEST_TRANSPORT_BASE + KB2_TEST_EVENT_BUFFER_OFFSET,
        .length = 0x1000,
    };
    host->regions[2] = (kb2_protocol_region_t){
        .region_id = 3,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ,
        .transport_base = KB2_TEST_TRANSPORT_BASE + KB2_TEST_REQUEST_BUFFER_OFFSET,
        .length = 0x1000,
    };
    host->regions[3] = (kb2_protocol_region_t){
        .region_id = 4,
        .rights = KB2_PROTOCOL_REGION_RIGHT_WRITE,
        .transport_base = KB2_TEST_TRANSPORT_BASE + KB2_TEST_RESPONSE_BUFFER_OFFSET,
        .length = 0x1000,
    };
    host->regions[4] = (kb2_protocol_region_t){
        .region_id = 5,
        .rights = KB2_PROTOCOL_REGION_RIGHT_READ,
        .transport_base = KB2_TEST_TRANSPORT_BASE + KB2_TEST_INDIRECT_TABLE_OFFSET,
        .length = 0x1000,
    };
    if (kb2_action_limit(action, KB2_LIMIT_OUTSTANDING_REQUEST_COUNT, &outstanding) !=
            KB2_STATUS_OK ||
        outstanding == 0 || outstanding > 16) {
        return KB2_STATUS_RESOURCE_DENIED;
    }
    if (host->next_channel_id == UINT64_MAX ||
        host->next_notification_id > UINT32_MAX - KB2_TEST_NOTIFICATION_COUNT) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }
    channel.channel_id = ++host->next_channel_id;
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        host->notification_ids[index] = ++host->next_notification_id;
    }

    host->queues[0].queue_id = 1;
    host->queues[0].role = KB2_PROTOCOL_QUEUE_ROLE_EVENT;
    host->queues[0].queue_size = KB2_TEST_QUEUE_SIZE;
    host->queues[0].descriptor_address = KB2_TEST_TRANSPORT_BASE + 0x1000;
    host->queues[0].available_address = KB2_TEST_TRANSPORT_BASE + 0x1100;
    host->queues[0].used_address = KB2_TEST_TRANSPORT_BASE + 0x1140;
    host->queues[0].available_notification_id = host->notification_ids[0];
    host->queues[0].used_notification_id = host->notification_ids[1];
    host->queues[0].max_chain_length = KB2_TEST_QUEUE_SIZE;
    host->queues[0].max_indirect_length = 128;
    host->queues[0].max_outstanding = (uint32_t)outstanding;

    host->queues[1].queue_id = 2;
    host->queues[1].role = KB2_PROTOCOL_QUEUE_ROLE_REQUEST;
    host->queues[1].queue_size = KB2_TEST_QUEUE_SIZE;
    host->queues[1].descriptor_address = KB2_TEST_TRANSPORT_BASE + 0x2000;
    host->queues[1].available_address = KB2_TEST_TRANSPORT_BASE + 0x2100;
    host->queues[1].used_address = KB2_TEST_TRANSPORT_BASE + 0x2140;
    host->queues[1].available_notification_id = host->notification_ids[2];
    host->queues[1].used_notification_id = host->notification_ids[3];
    host->queues[1].max_chain_length = KB2_TEST_QUEUE_SIZE;
    host->queues[1].max_indirect_length = 128;
    host->queues[1].max_outstanding = (uint32_t)outstanding;

    if (kb2_protocol_channel_encode(host->shared_memory,
                                    host->shared_memory_size,
                                    &host->channel_descriptor_size,
                                    &channel,
                                    host->queues,
                                    2,
                                    host->regions,
                                    KB2_TEST_REGION_COUNT) != KB2_PROTOCOL_OK ||
        kb2_test_vq_bind(&host->event_queue,
                         host->shared_memory,
                         host->shared_memory_size,
                         &host->queues[0],
                         host->regions,
                         KB2_TEST_REGION_COUNT,
                         1) != KB2_TEST_VQ_OK ||
        kb2_test_vq_bind(&host->request_queue,
                         host->shared_memory,
                         host->shared_memory_size,
                         &host->queues[1],
                         host->regions,
                         KB2_TEST_REGION_COUNT,
                         1) != KB2_TEST_VQ_OK) {
        return KB2_STATUS_HOST_FAILURE;
    }
    for (event_index = 0; event_index < KB2_TEST_QUEUE_SIZE; ++event_index) {
        uint64_t address = KB2_TEST_TRANSPORT_BASE + KB2_TEST_EVENT_BUFFER_OFFSET +
                           event_index * KB2_TEST_EVENT_BUFFER_STRIDE;

        if (kb2_test_vq_set_descriptor(&host->event_queue,
                                       (uint16_t)event_index,
                                       address,
                                       KB2_TEST_EVENT_BUFFER_STRIDE,
                                       KB2_TEST_VQ_DESCRIPTOR_FLAG_WRITE,
                                       0) != KB2_TEST_VQ_OK ||
            kb2_test_vq_publish(&host->event_queue, (uint16_t)event_index, &notify) !=
                KB2_TEST_VQ_OK) {
            return KB2_STATUS_HOST_FAILURE;
        }
    }
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_allocate(kb2_test_host_t *host,
                                           const kb2_action_t *action,
                                           uint64_t *resource_set_id_out) {
    const kb2_closure_t *closure = kb2_action_closure(action);
    uint8_t configured_digest[KB2_DIGEST_SIZE];
    uint8_t protocol_digest[KB2_DIGEST_SIZE];
    uint8_t interface_digest[KB2_DIGEST_SIZE];
    kb2_resource_type_t resource_type;
    uint64_t required_rights;
    uint64_t maximum_rights;
    uint64_t channel_count;
    uint64_t queue_count;
    uint64_t shared_memory_size;
    uint32_t slot_id;
    uint32_t minimum_count;
    uint32_t maximum_count;
    uint32_t resource_flags;
    uint32_t artifact_node_id;
    kb2_artifact_kind_t artifact_kind;
    int artifact_is_root;
    uint8_t artifact_digest[KB2_DIGEST_SIZE];
    size_t index;
    int failure_error;
    kb2_status_t status;

    if (host->resource_set_id != 0 || host->shared_memory != MAP_FAILED ||
        kb2_action_resource_set_id(action) != 0 || kb2_action_sandbox_id(action) != 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    if (host->manifest_fd < 0 && !kb2_test_make_closure_package(host)) {
        kb2_test_host_release_local_resources(host);
        return KB2_STATUS_HOST_FAILURE;
    }
    if (kb2_protocol_copy_schema_digest(protocol_digest, sizeof(protocol_digest)) !=
            KB2_PROTOCOL_OK ||
        closure == NULL || kb2_closure_resource_count(closure) != 1 ||
        kb2_closure_artifact_count(closure) != KB2_TEST_ARTIFACT_COUNT ||
        kb2_closure_resource(closure,
                             0,
                             &slot_id,
                             &resource_type,
                             interface_digest,
                             sizeof(interface_digest),
                             &minimum_count,
                             &maximum_count,
                             &required_rights,
                             &maximum_rights,
                             &resource_flags) != KB2_STATUS_OK ||
        slot_id != 1 || resource_type != KB2_RESOURCE_CHANNEL || minimum_count != 1 ||
        maximum_count != 1 ||
        required_rights != (KB2_CHANNEL_RIGHT_SEND | KB2_CHANNEL_RIGHT_RECEIVE) ||
        maximum_rights != required_rights ||
        memcmp(interface_digest, protocol_digest, sizeof(interface_digest)) != 0 ||
        (resource_flags & KB2_RESOURCE_REQUIRED) == 0 ||
        ((resource_flags & KB2_RESOURCE_RESET_REQUIRED) != 0) !=
            ((kb2_action_launch_flags(action) & KB2_LAUNCH_RESET_REQUIRED) != 0)) {
        return KB2_STATUS_RESOURCE_DENIED;
    }
    if (kb2_action_copy_digest(action,
                               KB2_DIGEST_MANIFEST,
                               configured_digest,
                               sizeof(configured_digest)) != KB2_STATUS_OK ||
        memcmp(configured_digest, host->manifest_digest, sizeof(configured_digest)) != 0) {
        return KB2_STATUS_RESOURCE_DENIED;
    }
    for (index = 0; index < KB2_TEST_ARTIFACT_COUNT; ++index) {
        if (kb2_closure_artifact(closure,
                                 index,
                                 &artifact_node_id,
                                 &artifact_kind,
                                 &artifact_is_root,
                                 artifact_digest,
                                 sizeof(artifact_digest)) != KB2_STATUS_OK ||
            artifact_node_id != index + 1u ||
            artifact_kind != (index == 0 ? KB2_ARTIFACT_SHARED_PROVIDER
                                         : KB2_ARTIFACT_RELOCATABLE_MODULE) ||
            artifact_is_root != (index == 2) ||
            memcmp(artifact_digest,
                   host->artifact_digests[index],
                   sizeof(artifact_digest)) != 0) {
            return KB2_STATUS_RESOURCE_DENIED;
        }
    }
    if (kb2_action_limit(action, KB2_LIMIT_SHARED_MEMORY_BYTES, &shared_memory_size) !=
            KB2_STATUS_OK ||
        kb2_action_limit(action, KB2_LIMIT_CHANNEL_COUNT, &channel_count) != KB2_STATUS_OK ||
        kb2_action_limit(action, KB2_LIMIT_QUEUE_COUNT, &queue_count) != KB2_STATUS_OK ||
        shared_memory_size < KB2_TEST_MINIMUM_SHARED_MEMORY_SIZE ||
        shared_memory_size > (uint64_t)SIZE_MAX || shared_memory_size > (uint64_t)INT64_MAX ||
        channel_count != 1 || queue_count != 2) {
        return KB2_STATUS_RESOURCE_DENIED;
    }
    if (kb2_action_copy_digest(action,
                               KB2_DIGEST_CHANNEL_SET,
                               configured_digest,
                               sizeof(configured_digest)) != KB2_STATUS_OK ||
        memcmp(configured_digest, protocol_digest, sizeof(configured_digest)) != 0) {
        return KB2_STATUS_RESOURCE_DENIED;
    }
    if (host->next_resource_set_id == UINT64_MAX) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }

    host->shared_memory_fd = memfd_create("kobox2-test-channel", MFD_CLOEXEC);
    if (host->shared_memory_fd < 0) {
        return errno == EMFILE || errno == ENFILE ? KB2_STATUS_RESOURCE_EXHAUSTED
                                                  : KB2_STATUS_HOST_FAILURE;
    }
    if (ftruncate(host->shared_memory_fd, (off_t)shared_memory_size) != 0) {
        failure_error = errno;
        kb2_test_host_release_local_resources(host);
        return failure_error == ENOSPC ? KB2_STATUS_RESOURCE_EXHAUSTED
                                       : KB2_STATUS_HOST_FAILURE;
    }
    host->shared_memory = mmap(NULL,
                               (size_t)shared_memory_size,
                               PROT_READ | PROT_WRITE,
                               MAP_SHARED,
                               host->shared_memory_fd,
                               0);
    if (host->shared_memory == MAP_FAILED) {
        failure_error = errno;
        kb2_test_host_release_local_resources(host);
        return failure_error == ENOMEM ? KB2_STATUS_RESOURCE_EXHAUSTED
                                       : KB2_STATUS_HOST_FAILURE;
    }
    host->shared_memory_size = (size_t)shared_memory_size;
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        host->notification_fds[index] = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (host->notification_fds[index] < 0) {
            failure_error = errno;
            kb2_test_host_release_local_resources(host);
            return failure_error == EMFILE || failure_error == ENFILE
                       ? KB2_STATUS_RESOURCE_EXHAUSTED
                       : KB2_STATUS_HOST_FAILURE;
        }
    }

    status = kb2_test_host_make_channel(host, action);
    if (status != KB2_STATUS_OK) {
        kb2_test_host_release_local_resources(host);
        return status;
    }
    host->generation = kb2_action_generation(action);
    if (!kb2_test_make_grant(host)) {
        kb2_test_host_release_local_resources(host);
        return KB2_STATUS_HOST_FAILURE;
    }
    host->resource_set_id = ++host->next_resource_set_id;
    *resource_set_id_out = host->resource_set_id;
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_launch(kb2_test_host_t *host,
                                         const kb2_action_t *action,
                                         uint64_t *sandbox_id_out) {
    posix_spawn_file_actions_t file_actions;
    char *arguments[2];
    int sockets[2] = {-1, -1};
    int spawn_status;
    int actions_status;

    if (host->next_sandbox_id == UINT64_MAX) {
        return KB2_STATUS_RESOURCE_EXHAUSTED;
    }
    if (host->resource_set_id == 0 || host->process_id > 0 ||
        kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
        return errno == EMFILE || errno == ENFILE ? KB2_STATUS_RESOURCE_EXHAUSTED
                                                  : KB2_STATUS_HOST_FAILURE;
    }
    actions_status = posix_spawn_file_actions_init(&file_actions);
    if (actions_status != 0) {
        close(sockets[0]);
        close(sockets[1]);
        return KB2_STATUS_HOST_FAILURE;
    }
    actions_status = posix_spawn_file_actions_addclose(&file_actions, sockets[0]);
    if (actions_status == 0) {
        actions_status =
            posix_spawn_file_actions_adddup2(&file_actions, sockets[1], KB2_TEST_BOOTSTRAP_FD);
    }
    if (actions_status == 0 && sockets[1] != KB2_TEST_BOOTSTRAP_FD) {
        actions_status = posix_spawn_file_actions_addclose(&file_actions, sockets[1]);
    }
    if (actions_status != 0) {
        posix_spawn_file_actions_destroy(&file_actions);
        close(sockets[0]);
        close(sockets[1]);
        return KB2_STATUS_HOST_FAILURE;
    }

    arguments[0] = (char *)host->sandbox_path;
    arguments[1] = NULL;
    spawn_status =
        posix_spawn(&host->process_id, host->sandbox_path, &file_actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&file_actions);
    close(sockets[1]);
    if (spawn_status != 0) {
        close(sockets[0]);
        host->process_id = -1;
        return spawn_status == EAGAIN ? KB2_STATUS_RESOURCE_EXHAUSTED
                                      : KB2_STATUS_HOST_FAILURE;
    }

    host->process_fd = pidfd_open(host->process_id, 0);
    if (host->process_fd < 0) {
        int process_status;

        kill(host->process_id, SIGKILL);
        kb2_test_wait_process(
            host->process_id, &process_status, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS);
        close(sockets[0]);
        host->process_id = -1;
        return KB2_STATUS_HOST_FAILURE;
    }

    host->bootstrap_socket = sockets[0];
    host->sandbox_id = ++host->next_sandbox_id;
    *sandbox_id_out = host->sandbox_id;
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_transfer(kb2_test_host_t *host,
                                           const kb2_action_t *action) {
    kb2_test_bootstrap_t bootstrap = {
        .generation = host->generation,
        .shared_memory_size = host->shared_memory_size,
        .manifest_size = host->manifest_size,
        .grant_size = host->grant_size,
        .channel_descriptor_size = host->channel_descriptor_size,
        .artifact_count = KB2_TEST_ARTIFACT_COUNT,
        .resource_handle_count = 1,
        .notification_count = KB2_TEST_NOTIFICATION_COUNT,
    };
    int file_descriptors[KB2_TEST_MAX_TRANSFER_FD_COUNT];
    size_t descriptor_count;
    size_t index;

    if (host->bootstrap_socket < 0 || host->process_id <= 0 || host->resources_transferred ||
        kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != host->sandbox_id) {
        return KB2_STATUS_HOST_FAILURE;
    }
    memcpy(bootstrap.manifest_digest,
           host->manifest_digest,
           sizeof(bootstrap.manifest_digest));
    memcpy(bootstrap.grant_digest, host->grant_digest, sizeof(bootstrap.grant_digest));
    file_descriptors[0] = host->shared_memory_fd;
    file_descriptors[1] = host->manifest_fd;
    file_descriptors[2] = host->grant_fd;
    for (index = 0; index < KB2_TEST_ARTIFACT_COUNT; ++index) {
        file_descriptors[KB2_TEST_BASE_TRANSFER_FD_COUNT + index] = host->artifact_fds[index];
    }
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        bootstrap.notification_ids[index] = host->notification_ids[index];
        file_descriptors[KB2_TEST_BASE_TRANSFER_FD_COUNT + KB2_TEST_ARTIFACT_COUNT +
                         bootstrap.resource_handle_count + index] =
            host->notification_fds[index];
    }
    file_descriptors[KB2_TEST_BASE_TRANSFER_FD_COUNT + KB2_TEST_ARTIFACT_COUNT] =
        host->shared_memory_fd;
    descriptor_count = kb2_test_bootstrap_descriptor_count(&bootstrap);
    if (!kb2_test_send_bootstrap(host->bootstrap_socket,
                                 &bootstrap,
                                 file_descriptors,
                                 descriptor_count)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->resources_transferred = 1;
    if (!kb2_test_host_receive_ready(host)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    kb2_test_close(&host->bootstrap_socket);
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_quiesce(kb2_test_host_t *host,
                                          const kb2_action_t *action) {
    if (!host->resources_transferred || host->resources_revoked ||
        host->abnormal_exit_allowed || host->process_id <= 0 || host->process_fd < 0 ||
        kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != host->sandbox_id ||
        !kb2_test_host_request_quiesce(host) ||
        !kb2_test_wait_process_fd(host->process_fd, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->process_exited = 1;
    kb2_test_close(&host->bootstrap_socket);
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_revoke(kb2_test_host_t *host, const kb2_action_t *action) {
    if (!host->resources_transferred || host->resources_revoked || host->process_id <= 0 ||
        host->process_fd < 0 || kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    if (!host->process_exited &&
        !kb2_test_wait_process_fd(host->process_fd, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->process_exited = 1;
    host->resources_revoked = 1;
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_reap(kb2_test_host_t *host) {
    int process_status;

    if (!host->process_exited || host->process_id <= 0 ||
        !kb2_test_wait_process(
            host->process_id, &process_status, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    if (!host->abnormal_exit_allowed &&
        (!WIFEXITED(process_status) || WEXITSTATUS(process_status) != 0)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->process_id = -1;
    host->process_exited = 0;
    kb2_test_close(&host->process_fd);
    kb2_test_close(&host->bootstrap_socket);
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_reset(kb2_test_host_t *host, const kb2_action_t *action) {
    uint64_t value;
    size_t index;
    ssize_t bytes;

    if (!host->resources_revoked || kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    memset((uint8_t *)host->shared_memory + host->channel_descriptor_size,
           0,
           host->shared_memory_size - host->channel_descriptor_size);
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        do {
            bytes = read(host->notification_fds[index], &value, sizeof(value));
        } while (bytes == (ssize_t)sizeof(value) || (bytes < 0 && errno == EINTR));
        if (bytes < 0 && errno != EAGAIN) {
            return KB2_STATUS_HOST_FAILURE;
        }
    }
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_terminate(kb2_test_host_t *host,
                                            const kb2_action_t *action) {
    if (host->process_id <= 0 || host->process_fd < 0 || host->process_exited ||
        kb2_action_resource_set_id(action) != 0 ||
        kb2_action_sandbox_id(action) != host->sandbox_id) {
        return KB2_STATUS_HOST_FAILURE;
    }
    if (kill(host->process_id, SIGKILL) != 0 && errno != ESRCH) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->abnormal_exit_allowed = 1;
    if (!kb2_test_wait_process_fd(host->process_fd, KB2_TEST_PROCESS_TIMEOUT_MILLISECONDS)) {
        return KB2_STATUS_HOST_FAILURE;
    }
    host->process_exited = 1;
    return KB2_STATUS_OK;
}

static kb2_status_t kb2_test_host_reap_action(kb2_test_host_t *host,
                                              const kb2_action_t *action) {
    kb2_status_t status;

    if (!host->resources_revoked || !host->process_exited ||
        kb2_action_resource_set_id(action) != 0 ||
        kb2_action_sandbox_id(action) != host->sandbox_id) {
        return KB2_STATUS_HOST_FAILURE;
    }
    status = kb2_test_host_reap(host);
    if (status == KB2_STATUS_OK) {
        host->sandbox_id = 0;
    }
    return status;
}

static kb2_status_t kb2_test_host_release(kb2_test_host_t *host,
                                          const kb2_action_t *action) {
    if (host->process_id > 0 || kb2_action_resource_set_id(action) != host->resource_set_id ||
        kb2_action_sandbox_id(action) != 0) {
        return KB2_STATUS_HOST_FAILURE;
    }
    kb2_test_host_release_local_resources(host);
    return KB2_STATUS_OK;
}

kb2_status_t kb2_test_host_execute(kb2_test_host_t *host,
                                   const kb2_action_t *action,
                                   uint64_t *resource_set_id_out,
                                   uint64_t *sandbox_id_out) {
    if (host == NULL || action == NULL || resource_set_id_out == NULL || sandbox_id_out == NULL ||
        kb2_action_generation(action) == 0) {
        return KB2_STATUS_INVALID_ARGUMENT;
    }
    *resource_set_id_out = 0;
    *sandbox_id_out = 0;
    if (kb2_action_type(action) != KB2_ACTION_ALLOCATE_RESOURCES &&
        kb2_action_generation(action) != host->generation) {
        return KB2_STATUS_HOST_FAILURE;
    }

    switch (kb2_action_type(action)) {
    case KB2_ACTION_ALLOCATE_RESOURCES:
        return kb2_test_host_allocate(host, action, resource_set_id_out);
    case KB2_ACTION_LAUNCH_SANDBOX:
        return kb2_test_host_launch(host, action, sandbox_id_out);
    case KB2_ACTION_TRANSFER_RESOURCES:
        return kb2_test_host_transfer(host, action);
    case KB2_ACTION_QUIESCE_SANDBOX:
        return kb2_test_host_quiesce(host, action);
    case KB2_ACTION_REVOKE_RESOURCES:
        return kb2_test_host_revoke(host, action);
    case KB2_ACTION_RESET_RESOURCES:
        return kb2_test_host_reset(host, action);
    case KB2_ACTION_TERMINATE_SANDBOX:
        return kb2_test_host_terminate(host, action);
    case KB2_ACTION_REAP_SANDBOX:
        return kb2_test_host_reap_action(host, action);
    case KB2_ACTION_RELEASE_RESOURCES:
        return kb2_test_host_release(host, action);
    case KB2_ACTION_NONE:
    default:
        return KB2_STATUS_INVALID_ARGUMENT;
    }
}

uint64_t kb2_test_host_resource_set_id(const kb2_test_host_t *host) {
    return host == NULL ? 0 : host->resource_set_id;
}

uint64_t kb2_test_host_sandbox_id(const kb2_test_host_t *host) {
    return host == NULL ? 0 : host->sandbox_id;
}

int kb2_test_host_is_released(const kb2_test_host_t *host) {
    return host != NULL && host->resource_set_id == 0 && host->sandbox_id == 0 &&
           host->process_id <= 0 && host->shared_memory == MAP_FAILED &&
           host->shared_memory_fd < 0 && host->manifest_fd < 0 && host->grant_fd < 0;
}
