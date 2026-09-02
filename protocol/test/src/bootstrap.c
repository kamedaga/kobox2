/* SPDX-License-Identifier: MIT */

#define _GNU_SOURCE

#include <kobox2_test/bootstrap.h>

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define KB2_TEST_BOOTSTRAP_SIZE 128u
#define KB2_TEST_BOOTSTRAP_MAGIC "kb2boot\0"
#define KB2_TEST_BOOTSTRAP_MAGIC_SIZE 8u
#define KB2_TEST_BOOTSTRAP_GENERATION_OFFSET 8u
#define KB2_TEST_BOOTSTRAP_SHARED_SIZE_OFFSET 16u
#define KB2_TEST_BOOTSTRAP_MANIFEST_SIZE_OFFSET 24u
#define KB2_TEST_BOOTSTRAP_DESCRIPTOR_SIZE_OFFSET 32u
#define KB2_TEST_BOOTSTRAP_ARTIFACT_COUNT_OFFSET 36u
#define KB2_TEST_BOOTSTRAP_NOTIFICATION_COUNT_OFFSET 40u
#define KB2_TEST_BOOTSTRAP_DESCRIPTOR_COUNT_OFFSET 44u
#define KB2_TEST_BOOTSTRAP_MANIFEST_DIGEST_OFFSET 48u
#define KB2_TEST_BOOTSTRAP_NOTIFICATION_IDS_OFFSET 80u
#define KB2_TEST_BOOTSTRAP_RESERVED_OFFSET 96u

static void kb2_test_store_u32(uint8_t *destination, uint32_t value) {
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8u);
    destination[2] = (uint8_t)(value >> 16u);
    destination[3] = (uint8_t)(value >> 24u);
}

static uint32_t kb2_test_load_u32(const uint8_t *source) {
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8u) |
           ((uint32_t)source[2] << 16u) | ((uint32_t)source[3] << 24u);
}

static void kb2_test_store_u64(uint8_t *destination, uint64_t value) {
    kb2_test_store_u32(destination, (uint32_t)value);
    kb2_test_store_u32(destination + 4, (uint32_t)(value >> 32u));
}

static uint64_t kb2_test_load_u64(const uint8_t *source) {
    return (uint64_t)kb2_test_load_u32(source) |
           ((uint64_t)kb2_test_load_u32(source + 4) << 32u);
}

static int kb2_test_wait_readable(int file_descriptor, int timeout_milliseconds) {
    struct pollfd descriptor = {
        .fd = file_descriptor,
        .events = POLLIN,
    };
    int result;

    do {
        result = poll(&descriptor, 1, timeout_milliseconds);
    } while (result < 0 && errno == EINTR);
    return result == 1 && (descriptor.revents & POLLIN) != 0;
}

static int kb2_test_notification_ids_valid(const uint32_t *notification_ids) {
    size_t left;
    size_t right;

    for (left = 0; left < KB2_TEST_NOTIFICATION_COUNT; ++left) {
        if (notification_ids[left] == 0) {
            return 0;
        }
        for (right = left + 1; right < KB2_TEST_NOTIFICATION_COUNT; ++right) {
            if (notification_ids[left] == notification_ids[right]) {
                return 0;
            }
        }
    }
    return 1;
}

size_t kb2_test_bootstrap_descriptor_count(const kb2_test_bootstrap_t *bootstrap) {
    if (bootstrap == NULL || bootstrap->artifact_count == 0 ||
        bootstrap->artifact_count > KB2_TEST_MAX_ARTIFACT_COUNT ||
        bootstrap->notification_count != KB2_TEST_NOTIFICATION_COUNT) {
        return 0;
    }
    return KB2_TEST_BASE_TRANSFER_FD_COUNT + bootstrap->artifact_count +
           bootstrap->notification_count;
}

int kb2_test_send_bootstrap(int socket_fd,
                            const kb2_test_bootstrap_t *bootstrap,
                            const int *file_descriptors,
                            size_t file_descriptor_count) {
    uint8_t message[KB2_TEST_BOOTSTRAP_SIZE] = {0};
    uint8_t control[CMSG_SPACE(sizeof(int) * KB2_TEST_MAX_TRANSFER_FD_COUNT)] = {0};
    struct iovec vector = {
        .iov_base = message,
        .iov_len = sizeof(message),
    };
    struct msghdr header = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    struct cmsghdr *control_header;
    size_t index;
    ssize_t sent;

    if (socket_fd < 0 || bootstrap == NULL || file_descriptors == NULL ||
        file_descriptor_count != kb2_test_bootstrap_descriptor_count(bootstrap) ||
        bootstrap->generation == 0 || bootstrap->shared_memory_size == 0 ||
        bootstrap->manifest_size == 0 || bootstrap->channel_descriptor_size == 0 ||
        bootstrap->channel_descriptor_size > bootstrap->shared_memory_size ||
        !kb2_test_notification_ids_valid(bootstrap->notification_ids)) {
        return 0;
    }
    for (index = 0; index < file_descriptor_count; ++index) {
        if (file_descriptors[index] < 0) {
            return 0;
        }
    }
    header.msg_controllen = CMSG_SPACE(sizeof(int) * file_descriptor_count);

    memcpy(message, KB2_TEST_BOOTSTRAP_MAGIC, KB2_TEST_BOOTSTRAP_MAGIC_SIZE);
    kb2_test_store_u64(message + KB2_TEST_BOOTSTRAP_GENERATION_OFFSET, bootstrap->generation);
    kb2_test_store_u64(message + KB2_TEST_BOOTSTRAP_SHARED_SIZE_OFFSET,
                       bootstrap->shared_memory_size);
    kb2_test_store_u64(message + KB2_TEST_BOOTSTRAP_MANIFEST_SIZE_OFFSET,
                       bootstrap->manifest_size);
    kb2_test_store_u32(message + KB2_TEST_BOOTSTRAP_DESCRIPTOR_SIZE_OFFSET,
                       bootstrap->channel_descriptor_size);
    kb2_test_store_u32(message + KB2_TEST_BOOTSTRAP_ARTIFACT_COUNT_OFFSET,
                       bootstrap->artifact_count);
    kb2_test_store_u32(message + KB2_TEST_BOOTSTRAP_NOTIFICATION_COUNT_OFFSET,
                       bootstrap->notification_count);
    kb2_test_store_u32(message + KB2_TEST_BOOTSTRAP_DESCRIPTOR_COUNT_OFFSET,
                       (uint32_t)file_descriptor_count);
    memcpy(message + KB2_TEST_BOOTSTRAP_MANIFEST_DIGEST_OFFSET,
           bootstrap->manifest_digest,
           sizeof(bootstrap->manifest_digest));
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        kb2_test_store_u32(message + KB2_TEST_BOOTSTRAP_NOTIFICATION_IDS_OFFSET + index * 4u,
                           bootstrap->notification_ids[index]);
    }

    control_header = CMSG_FIRSTHDR(&header);
    if (control_header == NULL) {
        return 0;
    }
    control_header->cmsg_level = SOL_SOCKET;
    control_header->cmsg_type = SCM_RIGHTS;
    control_header->cmsg_len = CMSG_LEN(sizeof(int) * file_descriptor_count);
    memcpy(CMSG_DATA(control_header), file_descriptors, sizeof(int) * file_descriptor_count);
    do {
        sent = sendmsg(socket_fd, &header, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    return sent == (ssize_t)sizeof(message);
}

static void kb2_test_close_descriptors(int *file_descriptors, size_t count) {
    size_t index;

    for (index = 0; index < count; ++index) {
        if (file_descriptors[index] >= 0) {
            close(file_descriptors[index]);
            file_descriptors[index] = -1;
        }
    }
}

int kb2_test_receive_bootstrap(int socket_fd,
                               kb2_test_bootstrap_t *bootstrap_out,
                               int *file_descriptors_out,
                               size_t file_descriptor_capacity,
                               size_t *file_descriptor_count_out) {
    uint8_t message[KB2_TEST_BOOTSTRAP_SIZE];
    uint8_t control[CMSG_SPACE(sizeof(int) * KB2_TEST_MAX_TRANSFER_FD_COUNT)] = {0};
    struct iovec vector = {
        .iov_base = message,
        .iov_len = sizeof(message),
    };
    struct msghdr header = {
        .msg_iov = &vector,
        .msg_iovlen = 1,
        .msg_control = control,
        .msg_controllen = sizeof(control),
    };
    struct cmsghdr *control_header;
    size_t descriptor_count;
    size_t descriptor_bytes;
    size_t index;
    ssize_t received;

    if (socket_fd < 0 || bootstrap_out == NULL || file_descriptors_out == NULL ||
        file_descriptor_count_out == NULL || file_descriptor_capacity == 0 ||
        file_descriptor_capacity > KB2_TEST_MAX_TRANSFER_FD_COUNT) {
        return 0;
    }
    for (index = 0; index < file_descriptor_capacity; ++index) {
        file_descriptors_out[index] = -1;
    }
    memset(bootstrap_out, 0, sizeof(*bootstrap_out));
    *file_descriptor_count_out = 0;
    if (!kb2_test_wait_readable(socket_fd, 5000)) {
        return 0;
    }

    do {
        received = recvmsg(socket_fd, &header, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        return 0;
    }

    control_header = CMSG_FIRSTHDR(&header);
    if (control_header == NULL || control_header->cmsg_level != SOL_SOCKET ||
        control_header->cmsg_type != SCM_RIGHTS || control_header->cmsg_len < CMSG_LEN(0)) {
        return 0;
    }
    descriptor_bytes = control_header->cmsg_len - CMSG_LEN(0);
    if (descriptor_bytes % sizeof(int) != 0) {
        return 0;
    }
    descriptor_count = descriptor_bytes / sizeof(int);
    if (descriptor_count > file_descriptor_capacity) {
        return 0;
    }
    memcpy(file_descriptors_out, CMSG_DATA(control_header), descriptor_count * sizeof(int));
    *file_descriptor_count_out = descriptor_count;

    if (received != (ssize_t)sizeof(message) ||
        (header.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
        memcmp(message, KB2_TEST_BOOTSTRAP_MAGIC, KB2_TEST_BOOTSTRAP_MAGIC_SIZE) != 0 ||
        CMSG_NXTHDR(&header, control_header) != NULL) {
        goto invalid;
    }

    bootstrap_out->generation =
        kb2_test_load_u64(message + KB2_TEST_BOOTSTRAP_GENERATION_OFFSET);
    bootstrap_out->shared_memory_size =
        kb2_test_load_u64(message + KB2_TEST_BOOTSTRAP_SHARED_SIZE_OFFSET);
    bootstrap_out->manifest_size =
        kb2_test_load_u64(message + KB2_TEST_BOOTSTRAP_MANIFEST_SIZE_OFFSET);
    bootstrap_out->channel_descriptor_size =
        kb2_test_load_u32(message + KB2_TEST_BOOTSTRAP_DESCRIPTOR_SIZE_OFFSET);
    bootstrap_out->artifact_count =
        kb2_test_load_u32(message + KB2_TEST_BOOTSTRAP_ARTIFACT_COUNT_OFFSET);
    bootstrap_out->notification_count =
        kb2_test_load_u32(message + KB2_TEST_BOOTSTRAP_NOTIFICATION_COUNT_OFFSET);
    memcpy(bootstrap_out->manifest_digest,
           message + KB2_TEST_BOOTSTRAP_MANIFEST_DIGEST_OFFSET,
           sizeof(bootstrap_out->manifest_digest));
    if (bootstrap_out->generation == 0 || bootstrap_out->shared_memory_size == 0 ||
        bootstrap_out->manifest_size == 0 ||
        bootstrap_out->channel_descriptor_size == 0 ||
        bootstrap_out->channel_descriptor_size > bootstrap_out->shared_memory_size ||
        descriptor_count != kb2_test_bootstrap_descriptor_count(bootstrap_out) ||
        descriptor_count !=
            kb2_test_load_u32(message + KB2_TEST_BOOTSTRAP_DESCRIPTOR_COUNT_OFFSET)) {
        goto invalid;
    }
    for (index = 0; index < KB2_TEST_NOTIFICATION_COUNT; ++index) {
        bootstrap_out->notification_ids[index] =
            kb2_test_load_u32(message + KB2_TEST_BOOTSTRAP_NOTIFICATION_IDS_OFFSET + index * 4u);
    }
    if (!kb2_test_notification_ids_valid(bootstrap_out->notification_ids)) {
        goto invalid;
    }
    for (index = KB2_TEST_BOOTSTRAP_RESERVED_OFFSET; index < sizeof(message); ++index) {
        if (message[index] != 0) {
            goto invalid;
        }
    }
    return 1;

invalid:
    kb2_test_close_descriptors(file_descriptors_out, descriptor_count);
    *file_descriptor_count_out = 0;
    return 0;
}
