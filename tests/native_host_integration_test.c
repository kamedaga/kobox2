/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE

#include <kobox2/closure.h>
#include <kobox2/closure_manifest.h>
#include <kobox2/pci_function_layout.h>
#include <kobox2/iommu_domain_layout.h>
#include <kobox2/resource_grant.h>
#include <kobox2/sha256.h>
#include <kobox2_test/bootstrap.h>
#include <kobox2_test/management.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define ARTIFACTS 7u
#define DEVICE_SIZE 8192u
#define PCI_DEVICE_SIZE 24576u

enum failure { NONE, RIGHTS, STALE, WRONG_FD, INIT_FAILURE, SPAWN, TRANSFER, KILL,
               MINIMAL, EXTENDED, WRONG_NAME, PCI, PCI_RIGHTS, PCI_STALE, PCI_WRONG_FD,
               PCI_KILL, PCI_WRONG_VIEW, DMA, DMA_RIGHTS, DMA_STALE, DMA_WRONG_FD, DMA_KILL,
               DMA_UNMAP };

struct native_host {
    const char *executable;
    kb2_controller_t *controller;
    kb2_closure_manifest_artifact_t artifacts[ARTIFACTS];
    int artifact_fds[ARTIFACTS];
    unsigned int artifact_count;
    int wrong_name;
    int pci;
    int with_dma;
    int wrong_view;
    size_t device_size;
    int manifest, grant, device, outside, socket, pidfd, log;
    int iommu;
    int dma_pidfd, dma_clean;
    pid_t dma_pid;
    uint8_t manifest_digest[32], grant_digest[32];
    uint64_t generation, resource_id, sandbox_id;
    pid_t pid;
    int allocated, revoked, reset, exited, exit_code;
    unsigned int resets, releases;
    enum failure failure;
};

static struct native_host *diagnostics;

static void check(int valid, unsigned int line) {
    if (!valid) {
        char output[65536];
        struct native_host *host = diagnostics;
        ssize_t length = host && host->log >= 0 ? pread(host->log, output, sizeof(output), 0) : -1;

        if (length > 0) {
            (void)write(STDERR_FILENO, output, (size_t)length);
        }
        fprintf(stderr, "native controller check failed at %u: %s\n", line, strerror(errno));
        if (host && host->pid > 0) {
            (void)kill(host->pid, SIGKILL);
            (void)waitpid(host->pid, NULL, 0);
        }
        if (host && host->dma_pid > 0) {
            (void)kill(host->dma_pid, SIGKILL);
            (void)waitpid(host->dma_pid, NULL, 0);
        }
        exit(1);
    }
}
#define CHECK(expression) check(!!(expression), __LINE__)

static void *allocate(void *context, size_t size) {
    (void)context;
    return malloc(size);
}

static void deallocate(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static int immutable(const void *data, size_t size, uint8_t digest[32]) {
    int fd = memfd_create("native-controller-package", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    void *sealed;
    CHECK(fd >= 0 && write(fd, data, size) == (ssize_t)size);
    CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK));
    sealed = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(sealed != MAP_FAILED);
    kb2_sha256(sealed, size, digest);
    CHECK(!munmap(sealed, size));
    return fd;
}

static void copy_artifact(struct native_host *host, unsigned int index, const char *path) {
    struct stat info;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    void *bytes;

    CHECK(fd >= 0 && !fstat(fd, &info) && info.st_size > 0 && info.st_size <= (128L << 20));
    bytes = mmap(NULL, (size_t)info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(bytes != MAP_FAILED);
    host->artifact_fds[index] = immutable(bytes, (size_t)info.st_size,
                                          host->artifacts[index].content_digest);
    host->artifacts[index].content_size = (uint64_t)info.st_size;
    CHECK(!munmap(bytes, (size_t)info.st_size));
    close(fd);
}

static void configure(struct native_host *host, char **paths) {
    const char *names[ARTIFACTS] = {
        "core", "i2c_core", "drm_panel_orientation_quirks", "drm", "drm_shmem_helper", "resource_test"
    };
    kb2_closure_manifest_dependency_t dependencies[ARTIFACTS - 1];
    kb2_closure_manifest_resource_t resource = {
        .slot_id = 1, .type = KB2_CLOSURE_RESOURCE_DEVICE, .minimum_count = 1, .maximum_count = 1,
        .required_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
        .maximum_rights = KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
        .flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED | KB2_CLOSURE_RESOURCE_FLAG_RESET_REQUIRED,
        .interface_schema_digest = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES,
    };
    kb2_closure_manifest_binding_t binding = {
        .slot_id = 1, .node_id = host->pci && !host->wrong_view ? 1 : host->artifact_count
    };
    kb2_closure_manifest_resource_t resource_list[2];
    kb2_closure_manifest_binding_t binding_list[2];
    kb2_closure_manifest_source_t source = {
        .artifacts = host->artifacts, .artifact_count = host->artifact_count,
        .dependencies = dependencies, .dependency_count = host->artifact_count - 1,
        .resources = &resource, .resource_count = 1, .bindings = &binding, .binding_count = 1,
    };
    kb2_closure_builder_t *builder;
    kb2_closure_t *closure;
    uint8_t *bytes;
    size_t size, written;
    unsigned int index;

    if (host->with_dma) {
        resource_list[0] = resource;
        resource_list[1] = (kb2_closure_manifest_resource_t) {
            .slot_id = 2, .type = KB2_CLOSURE_RESOURCE_DEVICE,
            .minimum_count = 1, .maximum_count = 1,
            .required_rights = KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS,
            .maximum_rights = KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS,
            .flags = KB2_CLOSURE_RESOURCE_FLAG_REQUIRED | KB2_CLOSURE_RESOURCE_FLAG_RESET_REQUIRED,
            .interface_schema_digest = KB2_IOMMU_DOMAIN_SCHEMA_SHA256_BYTES,
        };
        binding_list[0] = binding;
        binding_list[1] = (kb2_closure_manifest_binding_t) {.slot_id = 2, .node_id = 1};
        source.resources = resource_list;
        source.resource_count = 2;
        source.bindings = binding_list;
        source.binding_count = 2;
    }
    if (host->artifact_count == 2) {
        names[1] = host->pci ? "drm_panel_orientation_quirks" : "resource_test";
    } else if (host->artifact_count == 7) {
        names[5] = "lifetime_test";
        names[6] = "resource_test";
    }
    if (host->wrong_name) {
        names[host->artifact_count - 1] = "wrong_resource_name";
    }
    for (index = 0; index < host->artifact_count; index++) {
        host->artifacts[index] = (kb2_closure_manifest_artifact_t) {
            .node_id = index + 1,
            .kind = index ? KB2_CLOSURE_ARTIFACT_RELOCATABLE_MODULE : KB2_CLOSURE_ARTIFACT_SHARED_PROVIDER,
            .flags = KB2_CLOSURE_ARTIFACT_FLAG_NATIVE_LINUX |
                     (index == host->artifact_count - 1 ? KB2_CLOSURE_ARTIFACT_FLAG_ROOT : 0),
            .namespace_name = {.data = names[index], .length = strlen(names[index])},
        };
        copy_artifact(host, index,
                      paths[host->artifact_count == 2 && index ? (host->pci ? 2 : 5) : index]);
        if (index) {
            dependencies[index - 1] = (kb2_closure_manifest_dependency_t) {index + 1, index};
        }
    }
    CHECK(kb2_closure_manifest_encoded_size(&source, &size) == KB2_PROTOCOL_OK);
    bytes = malloc(size);
    CHECK(bytes && kb2_closure_manifest_encode(bytes, size, &written, &source) == KB2_PROTOCOL_OK);
    host->manifest = immutable(bytes, size, host->manifest_digest);
    free(bytes);
    CHECK(kb2_controller_create(allocate, deallocate, NULL, &host->controller) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_create(allocate, deallocate, NULL, host->manifest_digest, 32,
                                     &builder) == KB2_STATUS_OK);
    for (index = 0; index < host->artifact_count; index++) {
        CHECK(kb2_closure_builder_add_artifact(builder, index + 1,
            index ? KB2_ARTIFACT_RELOCATABLE_MODULE : KB2_ARTIFACT_SHARED_PROVIDER,
            host->artifacts[index].content_digest, 32, names[index], strlen(names[index])) == KB2_STATUS_OK);
        CHECK(kb2_closure_builder_set_native_lifecycle(builder, index + 1) == KB2_STATUS_OK);
        if (index) {
            CHECK(kb2_closure_builder_add_dependency(builder, index + 1, index) == KB2_STATUS_OK);
        }
    }
    CHECK(kb2_closure_builder_mark_root(builder, host->artifact_count) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_add_resource(builder, 1, KB2_RESOURCE_DEVICE,
        resource.interface_schema_digest, 32, 1, 1, resource.required_rights, resource.maximum_rights,
        KB2_RESOURCE_REQUIRED | KB2_RESOURCE_RESET_REQUIRED) == KB2_STATUS_OK);
    CHECK(kb2_closure_builder_bind_resource(builder, 1, binding.node_id) == KB2_STATUS_OK);
    if (host->with_dma) {
        CHECK(kb2_closure_builder_add_resource(builder, 2, KB2_RESOURCE_DEVICE,
            resource_list[1].interface_schema_digest, 32, 1, 1,
            KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS, KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS,
            KB2_RESOURCE_REQUIRED | KB2_RESOURCE_RESET_REQUIRED) == KB2_STATUS_OK);
        CHECK(kb2_closure_builder_bind_resource(builder, 2, 1) == KB2_STATUS_OK);
    }
    CHECK(kb2_closure_builder_seal(builder, &closure) == KB2_STATUS_OK);
    CHECK(kb2_controller_set_closure(host->controller, closure) == KB2_STATUS_OK);
    kb2_closure_destroy(closure);
    kb2_closure_builder_destroy(builder);
    for (index = KB2_DIGEST_PROFILE; index <= KB2_DIGEST_CHANNEL_SET; index++) {
        CHECK(kb2_controller_set_digest(host->controller, (kb2_digest_kind_t)index,
                                        host->manifest_digest, 32) == KB2_STATUS_OK);
    }
    for (index = 0; index <= KB2_LIMIT_OUTSTANDING_REQUEST_COUNT; index++) {
        CHECK(kb2_controller_set_limit(host->controller, (kb2_limit_kind_t)index,
                                       index == KB2_LIMIT_QUEUE_COUNT ? 2 : 1) == KB2_STATUS_OK);
    }
}

static int create_device(size_t size) {
    const uint8_t identity[4] = {0xf4, 0x1a, 0x50, 0x10};
    int fd = memfd_create("owned-pci-conformance", MFD_CLOEXEC | MFD_ALLOW_SEALING);

    CHECK(fd >= 0 && !ftruncate(fd, (off_t)size));
    CHECK(pwrite(fd, identity, sizeof(identity), 0) == sizeof(identity));
    CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_GROW | F_SEAL_SHRINK));
    return fd;
}

static void make_grant(struct native_host *host) {
    kb2_resource_grant_slot_source_t slot = {
        .slot_id = 1, .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
        .state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
        .interface_schema_digest = KB2_PCI_FUNCTION_SCHEMA_SHA256_BYTES,
    };
    kb2_resource_grant_object_source_t object = {
        .slot_id = 1, .object_id = host->generation + 100,
        .granted_rights = host->failure == RIGHTS && !host->with_dma ? KB2_CLOSURE_DEVICE_RIGHT_COMMAND :
                                                  KB2_PCI_FUNCTION_REQUIRED_RIGHTS,
    };
    kb2_resource_grant_handle_binding_t handle = {
        .object_id = object.object_id, .role = KB2_PCI_FUNCTION_NATIVE_HANDLE_ROLE_DEVICE,
        .transfer_handle_index = 0,
    };
    kb2_resource_grant_source_t source = {
        .generation = host->generation - (host->failure == STALE),
        .slots = &slot, .slot_count = 1, .objects = &object, .object_count = 1,
        .handle_bindings = &handle, .handle_binding_count = 1,
    };
    kb2_resource_grant_slot_source_t slots[2];
    kb2_resource_grant_object_source_t objects[2];
    kb2_resource_grant_handle_binding_t handles[2];
    uint8_t *bytes;
    size_t size, written;

    if (host->with_dma) {
        slots[0] = slot;
        slots[1] = (kb2_resource_grant_slot_source_t) {
            .slot_id = 2, .resource_type = KB2_CLOSURE_RESOURCE_DEVICE,
            .state = KB2_RESOURCE_GRANT_SLOT_PRESENT,
            .interface_schema_digest = KB2_IOMMU_DOMAIN_SCHEMA_SHA256_BYTES,
        };
        objects[0] = object;
        objects[1] = (kb2_resource_grant_object_source_t) {
            .slot_id = 2, .object_id = host->generation + 101,
            .granted_rights = host->failure == RIGHTS ? KB2_CLOSURE_DEVICE_RIGHT_COMMAND :
                                                      KB2_IOMMU_DOMAIN_REQUIRED_RIGHTS,
        };
        handles[0] = handle;
        handles[1] = (kb2_resource_grant_handle_binding_t) {
            .object_id = objects[1].object_id, .role = KB2_IOMMU_DOMAIN_NATIVE_HANDLE_ROLE_DOMAIN,
            .transfer_handle_index = 1,
        };
        source.slots = slots;
        source.slot_count = 2;
        source.objects = objects;
        source.object_count = 2;
        source.handle_bindings = handles;
        source.handle_binding_count = 2;
    }
    memcpy(source.closure_manifest_digest, host->manifest_digest, 32);
    CHECK(kb2_resource_grant_encoded_size(&source, &size) == KB2_PROTOCOL_OK);
    bytes = malloc(size);
    CHECK(bytes && kb2_resource_grant_encode(bytes, size, &written, &source) == KB2_PROTOCOL_OK);
    host->grant = immutable(bytes, size, host->grant_digest);
    free(bytes);
}

static void hex_digest(char output[65], const uint8_t bytes[32]) {
    const char *digits = "0123456789abcdef";
    unsigned int index;
    for (index = 0; index < 32; index++) {
        output[index * 2] = digits[bytes[index] >> 4];
        output[index * 2 + 1] = digits[bytes[index] & 15];
    }
    output[64] = 0;
}

static int launch(struct native_host *host) {
    posix_spawn_file_actions_t actions;
    char generation[32], object[32], parent[32], manifest[65], grant[65];
    char *arguments[] = {(char *)host->executable, generation, object, manifest, grant, parent,
                         host->with_dma ? "pci-dma" : host->pci ? "pci" :
                         host->failure == INIT_FAILURE ? "fail-init" : "normal", NULL};
    int sockets[2], child_socket, anchor, dma_anchor = -1, result;

    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets));
    /* Duplicate above the destination slots before spawn actions, even if
     * the caller happened to allocate one of those slot numbers itself.
     */
    child_socket = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
    anchor = fcntl(host->device, F_DUPFD_CLOEXEC, 10);
    CHECK(child_socket >= 0 && anchor >= 0);
    CHECK(!posix_spawn_file_actions_init(&actions));
    CHECK(!posix_spawn_file_actions_adddup2(&actions, child_socket, KB2_TEST_BOOTSTRAP_FD));
    CHECK(!posix_spawn_file_actions_adddup2(&actions, anchor, KB2_TEST_NATIVE_OWNER_FD));
    if (host->with_dma) {
        dma_anchor = fcntl(host->iommu, F_DUPFD_CLOEXEC, 10);
        CHECK(dma_anchor >= 0);
        CHECK(!posix_spawn_file_actions_adddup2(&actions, dma_anchor, KB2_TEST_NATIVE_IOMMU_OWNER_FD));
    }
    CHECK(!posix_spawn_file_actions_adddup2(&actions, host->log, STDERR_FILENO));
    snprintf(generation, sizeof(generation), "%llu", (unsigned long long)host->generation);
    snprintf(object, sizeof(object), "%llu", (unsigned long long)host->generation + 100);
    snprintf(parent, sizeof(parent), "%llu", (unsigned long long)getpid());
    hex_digest(manifest, host->manifest_digest);
    hex_digest(grant, host->grant_digest);
    result = posix_spawn(&host->pid, host->failure == SPAWN ? "/dev/null/native-sandbox" :
                         host->executable, &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(child_socket);
    close(anchor);
    if (dma_anchor >= 0) {
        close(dma_anchor);
    }
    close(sockets[1]);
    if (result) {
        close(sockets[0]);
        host->pid = 0;
        return 0;
    }
    host->socket = sockets[0];
    host->pidfd = (int)syscall(SYS_pidfd_open, host->pid, 0);
    CHECK(host->pidfd >= 0);
    return 1;
}

static void launch_dma_device(struct native_host *host) {
    posix_spawn_file_actions_t actions;
    char parent[32];
    char *arguments[] = {(char *)host->executable,
                        host->failure == DMA_UNMAP ? "--dma-engine-fail-unmap" : "--dma-engine",
                        parent, NULL};
    int sockets[2], child;
    struct timeval timeout = {.tv_sec = 5};

    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets));
    CHECK(!setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    CHECK(!setsockopt(sockets[0], SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
    child = fcntl(sockets[1], F_DUPFD_CLOEXEC, 10);
    CHECK(child >= 0 && !posix_spawn_file_actions_init(&actions));
    CHECK(!posix_spawn_file_actions_adddup2(&actions, child, KB2_TEST_BOOTSTRAP_FD));
    CHECK(!posix_spawn_file_actions_adddup2(&actions, host->log, STDERR_FILENO));
    snprintf(parent, sizeof(parent), "%llu", (unsigned long long)getpid());
    CHECK(!posix_spawn(&host->dma_pid, host->executable, &actions, NULL, arguments, environ));
    posix_spawn_file_actions_destroy(&actions);
    close(child);
    close(sockets[1]);
    host->iommu = sockets[0];
    host->dma_pidfd = (int)syscall(SYS_pidfd_open, host->dma_pid, 0);
    CHECK(host->dma_pidfd >= 0);
}

static int observe_exit(struct native_host *host, int timeout) {
    struct pollfd process = {.fd = host->pidfd, .events = POLLIN};
    siginfo_t info = {0};
    int result;
    if (host->exited) {
        return 1;
    }
    do {
        result = poll(&process, 1, timeout);
    } while (result < 0 && errno == EINTR);
    if (result != 1 || !(process.revents & POLLIN)) {
        return 0;
    }
    CHECK(!waitid(P_PID, (id_t)host->pid, &info, WEXITED | WNOWAIT));
    host->exit_code = info.si_code == CLD_EXITED ? info.si_status : 128 + info.si_status;
    host->exited = 1;
    return 1;
}

static void observe_dma_panic(struct native_host *host) {
    char output[65536];
    unsigned int attempt;

    for (attempt = 0; attempt < 600; attempt++) {
        ssize_t length = pread(host->log, output, sizeof(output) - 1, 0);

        CHECK(length >= 0);
        output[length] = 0;
        if (strstr(output, "DMA panic observed: page_references=2 online_cpus=1")) {
            break;
        }
        CHECK(!host->exited);
        (void)observe_exit(host, 50);
    }
    CHECK(attempt < 600);
    /* The notifier proves the fatal Linux path. Only then does host policy
     * terminate the sandbox; PROCESS_EXIT must report a real observed exit.
     */
    CHECK(!observe_exit(host, 0));
    CHECK(!kill(host->pid, SIGKILL) && observe_exit(host, 30000));
    CHECK(host->exit_code == 128 + SIGKILL);
    {
        ssize_t length = pread(host->log, output, sizeof(output) - 1, 0);
        CHECK(length >= 0);
        output[length] = 0;
    }
    CHECK(strstr(output, "DMA invalidation denied: maps=1 unmaps=0 retained=1"));
    CHECK(!strstr(output, "DMA continued after failed invalidation"));
    CHECK(!strstr(output, "DMA Gate: status=0"));
    printf("DMA failed invalidation: upstream panic observed, owner and DMA pin retained\n");
}

static int send_stop(struct native_host *host, uint64_t generation) {
    uint8_t packet[KB2_TEST_MESSAGE_SIZE];
    CHECK(kb2_test_message_encode(packet, sizeof(packet), KB2_TEST_REQUEST_QUIESCE,
                                  0, generation, 2, 0));
    return send(host->socket, packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet);
}

static int receive_event(struct native_host *host, uint32_t expected) {
    struct pollfd wait = {.fd = host->socket, .events = POLLIN};
    uint8_t packet[KB2_TEST_MESSAGE_SIZE];
    uint64_t correlation, value;
    uint32_t opcode;
    int result;
    do {
        result = poll(&wait, 1, 30000);
    } while (result < 0 && errno == EINTR);
    if (result != 1 || recv(host->socket, packet, sizeof(packet), MSG_TRUNC) != sizeof(packet)) {
        return 0;
    }
    return kb2_test_message_decode(packet, sizeof(packet), KB2_TEST_MESSAGE_FLAG_EVENT,
                                    host->generation, &opcode, &correlation, &value) &&
           opcode == expected && correlation && !value;
}

static kb2_status_t execute(struct native_host *host, const kb2_action_t *action,
                            uint64_t *resource, uint64_t *sandbox) {
    kb2_action_type_t type = kb2_action_type(action);
    uint64_t generation = kb2_action_generation(action);
    unsigned int index;
    *resource = *sandbox = 0;

    CHECK(generation && (type == KB2_ACTION_ALLOCATE_RESOURCES || generation == host->generation));
    if (type != KB2_ACTION_ALLOCATE_RESOURCES) {
        int process_only = type == KB2_ACTION_TERMINATE_SANDBOX || type == KB2_ACTION_REAP_SANDBOX;
        int process_target = process_only || type == KB2_ACTION_TRANSFER_RESOURCES ||
                             type == KB2_ACTION_QUIESCE_SANDBOX;
        CHECK(kb2_action_resource_set_id(action) == (process_only ? 0 : host->resource_id));
        CHECK(kb2_action_sandbox_id(action) == (process_target ? host->sandbox_id : 0));
    }
    switch (type) {
    case KB2_ACTION_ALLOCATE_RESOURCES: {
        uint8_t digest[32];
        CHECK(!host->allocated && !host->pid);
        CHECK(kb2_closure_uses_native_lifecycle(kb2_action_closure(action)));
        CHECK(kb2_action_launch_flags(action) & KB2_LAUNCH_RESET_REQUIRED);
        CHECK(kb2_action_copy_digest(action, KB2_DIGEST_MANIFEST, digest, 32) == KB2_STATUS_OK);
        CHECK(!memcmp(digest, host->manifest_digest, 32));
        host->generation = generation;
        host->resource_id = generation + 1000;
        host->revoked = host->reset = host->exited = 0;
        host->dma_clean = 0;
        host->device = create_device(host->device_size);
        host->log = memfd_create("native-controller-log", MFD_CLOEXEC);
        CHECK(host->log >= 0);
        if (host->with_dma) {
            launch_dma_device(host);
        }
        make_grant(host);
        host->allocated = 1;
        *resource = host->resource_id;
        break;
    }
    case KB2_ACTION_LAUNCH_SANDBOX:
        if (!launch(host)) {
            return KB2_STATUS_HOST_FAILURE;
        }
        host->sandbox_id = generation + 2000;
        *sandbox = host->sandbox_id;
        break;
    case KB2_ACTION_TRANSFER_RESOURCES: {
        int fds[ARTIFACTS + 4] = {host->manifest, host->grant};
        for (index = 0; index < host->artifact_count; index++) {
            fds[index + 2] = host->artifact_fds[index];
        }
        fds[host->artifact_count + 2] = host->failure == WRONG_FD && !host->with_dma ?
                                      host->outside : host->device;
        if (host->with_dma) {
            fds[host->artifact_count + 3] = host->failure == WRONG_FD ? host->outside : host->iommu;
        }
        if (host->failure == TRANSFER) {
            close(host->socket);
            host->socket = -1;
        }
        if (!kb2_test_send_handles(host->socket, fds, host->artifact_count + 3 + host->with_dma)) {
            return KB2_STATUS_HOST_FAILURE;
        }
        break;
    }
    case KB2_ACTION_QUIESCE_SANDBOX: {
        uint32_t marker;
        char output[65536];
        ssize_t length;
        if (!send_stop(host, generation) || !observe_exit(host, 30000) || host->exit_code) {
            return KB2_STATUS_HOST_FAILURE;
        }
        CHECK(pread(host->device, &marker, sizeof(marker), host->pci ? 8192 : 4096) == sizeof(marker));
        CHECK(marker == (host->pci ? 0x51c0ffeeu : 0x72657332u));
        length = pread(host->log, output, sizeof(output) - 1, 0);
        CHECK(length >= 0);
        output[length] = 0;
        {
            char expected[96];
            snprintf(expected, sizeof(expected), "loaded=%u unloaded=%u cleanup=0",
                     host->artifact_count - 1, host->artifact_count - 1);
            CHECK(strstr(output, expected));
        }
        CHECK(strstr(output, "Native resource cleanup: imports=1 releases=1"));
        if (host->pci) {
            CHECK(strstr(output, "PCI enumeration: status=0 scans=2 caps=10 removals=2 maps=12 revoked=10 cache=1"));
        }
        if (host->with_dma) {
            CHECK(strstr(output, "DMA Gate: status=0 cases=8"));
            CHECK(strstr(output, "cpu_mask=3 rounds=16 sole_pins=32"));
            /* Small kmalloc, four-page kmalloc, and a dedicated slab add six mappings. */
            CHECK(strstr(output, "Native DMA cleanup: maps=61 unmaps=61 revoked=61"));
            host->dma_clean = 1;
        }
        break;
    }
    case KB2_ACTION_TERMINATE_SANDBOX:
        if (!observe_exit(host, 0)) {
            CHECK(!kill(host->pid, SIGKILL));
            CHECK(observe_exit(host, 30000));
        }
        break;
    case KB2_ACTION_REVOKE_RESOURCES:
        CHECK(host->allocated && (!host->pid || observe_exit(host, 0)));
        if (host->socket >= 0) {
            close(host->socket);
            host->socket = -1;
        }
        if (host->with_dma) {
            int status;
            pid_t waited = waitpid(host->dma_pid, &status, WNOHANG);
            CHECK(waited == 0 || waited == host->dma_pid);
            if (!waited) {
                if (host->dma_clean) {
                    struct pollfd process = {.fd = host->dma_pidfd, .events = POLLIN};
                    CHECK(poll(&process, 1, 5000) == 1 && (process.revents & POLLIN));
                } else {
                    CHECK(!kill(host->dma_pid, SIGKILL));
                }
                CHECK(waitpid(host->dma_pid, &status, 0) == host->dma_pid);
            }
            if (host->dma_clean) {
                CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
            }
            host->dma_pid = 0;
            close(host->dma_pidfd);
            host->dma_pidfd = -1;
            close(host->iommu);
            host->iommu = -1;
        }
        host->revoked = 1;
        break;
    case KB2_ACTION_RESET_RESOURCES: {
        uint8_t empty[PCI_DEVICE_SIZE] = {0xf4, 0x1a, 0x50, 0x10}, outside[PCI_DEVICE_SIZE];
        CHECK(host->revoked && (!host->pid || host->exited));
        CHECK(pwrite(host->device, empty, host->device_size, 0) == (ssize_t)host->device_size);
        empty[0x80] = 0xa5;
        CHECK(pread(host->outside, outside, host->device_size, 0) == (ssize_t)host->device_size);
        CHECK(!memcmp(outside, empty, host->device_size));
        host->reset = 1;
        host->resets++;
        break;
    }
    case KB2_ACTION_REAP_SANDBOX:
        CHECK(host->exited && waitpid(host->pid, NULL, 0) == host->pid);
        close(host->pidfd);
        host->pidfd = -1;
        host->pid = 0;
        host->sandbox_id = 0;
        break;
    case KB2_ACTION_RELEASE_RESOURCES:
        CHECK(host->allocated && host->revoked && host->reset && !host->pid);
        close(host->grant);
        close(host->device);
        close(host->log);
        host->grant = host->device = host->log = -1;
        host->allocated = 0;
        host->resource_id = 0;
        host->releases++;
        break;
    default:
        CHECK(0);
    }
    return KB2_STATUS_OK;
}

static int drive(struct native_host *host) {
    const kb2_action_t *action;
    while ((action = kb2_controller_pending_action(host->controller))) {
        uint64_t generation = kb2_action_generation(action), token = kb2_action_token(action);
        uint64_t resource, sandbox;
        kb2_status_t result = execute(host, action, &resource, &sandbox);
        CHECK(kb2_controller_complete_action(host->controller, generation, token,
                                              result, resource, sandbox) == result);
        if (result != KB2_STATUS_OK) {
            return 0;
        }
    }
    return 1;
}

static void running(struct native_host *host) {
    uint32_t before, after;
    unsigned int attempt;
    CHECK(receive_event(host, KB2_TEST_EVENT_READY));
    CHECK(kb2_controller_report_ready(host->controller, host->generation) == KB2_STATUS_OK);
    CHECK(kb2_controller_state(host->controller) == KB2_STATE_RUNNING);
    if (host->pci) {
        CHECK(pread(host->device, &before, sizeof(before), 8192) == sizeof(before));
        CHECK(before == 0x51c0ffeeu && !observe_exit(host, 100));
        CHECK(pread(host->device, &before, sizeof(before), 0x5100) == sizeof(before));
        CHECK(before == 0xabcdef02u);
        CHECK(pread(host->device, &before, sizeof(before), 0x5300) == sizeof(before));
        CHECK(before == 0xabcdef03u);
    } else {
        CHECK(pread(host->device, &before, sizeof(before), 4096) == sizeof(before));
        CHECK(before == 0x72657331u);
        CHECK(pread(host->device, &before, sizeof(before), 4104) == sizeof(before));
        for (attempt = 0; attempt < 30; attempt++) {
            CHECK(!observe_exit(host, 100));
            CHECK(pread(host->device, &after, sizeof(after), 4104) == sizeof(after));
            if (after > before) {
                break;
            }
        }
        CHECK(after > before);
    }
    if (host->generation > 1) {
        CHECK(send_stop(host, host->generation - 1));
        CHECK(receive_event(host, KB2_TEST_EVENT_FAULT));
        CHECK(!observe_exit(host, 0));
        CHECK(kb2_controller_report_ready(host->controller, host->generation - 1) ==
              KB2_STATUS_STALE_GENERATION);
    }
}

int main(int argc, char **argv) {
    static const char *const failures[] = {
        "normal", "rights", "stale", "wrong-fd", "init-failure", "spawn", "transfer", "kill",
        "minimal", "extended", "wrong-name", "pci", "pci-rights", "pci-stale", "pci-wrong-fd",
        "pci-kill", "pci-wrong-view", "dma", "dma-rights", "dma-stale", "dma-wrong-fd", "dma-kill",
        "dma-unmap-failure"
    };
    struct native_host host = {.manifest = -1, .grant = -1, .device = -1, .outside = -1,
                              .socket = -1, .pidfd = -1, .log = -1, .iommu = -1, .dma_pidfd = -1};
    enum failure failure = NONE;
    unsigned int index;

    CHECK(argc == 9 || argc == 10);
    diagnostics = &host;
    host.executable = argv[1];
    for (index = 0; index < sizeof(failures) / sizeof(*failures); index++) {
        if (!strcmp(argv[argc - 1], failures[index])) {
            failure = (enum failure)index;
            break;
        }
    }
    CHECK(index < sizeof(failures) / sizeof(*failures));
    CHECK((failure == EXTENDED) == (argc == 10));
    host.pci = failure >= PCI;
    host.with_dma = failure >= DMA;
    host.wrong_view = failure == PCI_WRONG_VIEW;
    host.device_size = host.pci ? PCI_DEVICE_SIZE : DEVICE_SIZE;
    host.artifact_count = failure == MINIMAL || host.pci ? 2 : failure == EXTENDED ? 7 : 6;
    host.wrong_name = failure == WRONG_NAME;
    if (host.with_dma) {
        static const enum failure dma_failures[] = {NONE, RIGHTS, STALE, WRONG_FD, KILL, DMA_UNMAP};
        failure = dma_failures[failure - DMA];
    } else if (host.pci) {
        static const enum failure pci_failures[] = {NONE, RIGHTS, STALE, WRONG_FD, KILL, NONE};
        failure = pci_failures[failure - PCI];
    } else if (failure == MINIMAL || failure == EXTENDED) {
        failure = NONE;
    }
    configure(&host, argv + 2);
    host.outside = create_device(host.device_size);
    {
        uint8_t sentinel = 0xa5;
        CHECK(pwrite(host.outside, &sentinel, sizeof(sentinel), 0x80) == sizeof(sentinel));
    }
    CHECK(kb2_controller_start(host.controller) == KB2_STATUS_OK && drive(&host));
    if (host.wrong_view) {
        uint32_t marker;
        char output[65536];
        ssize_t length;

        CHECK(!receive_event(&host, KB2_TEST_EVENT_READY));
        CHECK(observe_exit(&host, 30000) && host.exit_code == 68);
        CHECK(pread(host.device, &marker, sizeof(marker), 8192) == sizeof(marker) && !marker);
        length = pread(host.log, output, sizeof(output) - 1, 0);
        CHECK(length >= 0);
        output[length] = 0;
        CHECK(strstr(output, "Native PCI grant rejected:") && !strstr(output, "PCI enumeration:"));
        CHECK(kb2_controller_report_fault(host.controller, host.generation,
                                          KB2_FAULT_PROCESS_EXIT, 68) == KB2_STATUS_OK);
        CHECK(kb2_controller_stop(host.controller) == KB2_STATUS_OK && drive(&host));
        goto finished;
    }
    if (host.wrong_name) {
        uint32_t marker;
        char output[65536];
        ssize_t length;
        CHECK(!receive_event(&host, KB2_TEST_EVENT_READY));
        CHECK(observe_exit(&host, 30000) && host.exit_code == 1);
        CHECK(pread(host.device, &marker, sizeof(marker), 4096) == sizeof(marker) && !marker);
        CHECK(pread(host.device, &marker, sizeof(marker), 0x40) == sizeof(marker) && !marker);
        length = pread(host.log, output, sizeof(output) - 1, 0);
        CHECK(length >= 0);
        output[length] = 0;
        CHECK(strstr(output, "status=-129 loaded=4 unloaded=4 cleanup=0"));
        CHECK(strstr(output, "Native resource cleanup: imports=1 releases=1"));
        CHECK(kb2_controller_report_fault(host.controller, host.generation,
                                          KB2_FAULT_PROCESS_EXIT, 1) == KB2_STATUS_OK);
        CHECK(kb2_controller_stop(host.controller) == KB2_STATUS_OK && drive(&host));
        goto finished;
    }
    running(&host);
    CHECK(host.generation == 1);
    if (failure == KILL) {
        CHECK(!kill(host.pid, SIGKILL) && observe_exit(&host, 30000));
        CHECK(kb2_controller_report_fault(host.controller, host.generation,
                                          KB2_FAULT_PROCESS_EXIT, 137) == KB2_STATUS_OK);
    } else {
        host.failure = failure;
    }
    CHECK(kb2_controller_restart(host.controller) == KB2_STATUS_OK);
    if (failure == NONE || failure == KILL) {
        CHECK(drive(&host));
    } else {
        int started = drive(&host);
        if (failure == SPAWN || failure == TRANSFER) {
            CHECK(!started && kb2_controller_state(host.controller) == KB2_STATE_FAULTED);
        } else if (failure == DMA_UNMAP) {
            CHECK(started);
            observe_dma_panic(&host);
            CHECK(kb2_controller_report_fault(host.controller, host.generation,
                KB2_FAULT_PROCESS_EXIT, (uint64_t)host.exit_code) == KB2_STATUS_OK);
        } else {
            CHECK(started && !receive_event(&host, KB2_TEST_EVENT_READY));
            CHECK(observe_exit(&host, 30000) && host.exit_code);
            CHECK(host.exit_code == (failure == INIT_FAILURE ? 1 : failure == WRONG_FD ? 67 : 66));
            CHECK(kb2_controller_report_fault(host.controller, host.generation,
                KB2_FAULT_PROCESS_EXIT, (uint64_t)host.exit_code) == KB2_STATUS_OK);
        }
        CHECK(host.generation == 2);
        host.failure = NONE;
        CHECK(kb2_controller_restart(host.controller) == KB2_STATUS_OK && drive(&host));
        CHECK(host.generation == 3);
    }
    running(&host);
    CHECK(kb2_controller_stop(host.controller) == KB2_STATUS_OK && drive(&host));
finished:
    CHECK(kb2_controller_state(host.controller) == KB2_STATE_IDLE && !host.allocated && !host.pid);
    CHECK(host.resets == host.generation && host.releases == host.generation);
    kb2_controller_destroy(host.controller);
    for (index = 0; index < host.artifact_count; index++) {
        close(host.artifact_fds[index]);
    }
    close(host.manifest);
    close(host.outside);
    printf("Native controller boot/stop/restart: %s passed, generations=%llu\n",
           argv[argc - 1], (unsigned long long)host.generation);
    return 0;
}
