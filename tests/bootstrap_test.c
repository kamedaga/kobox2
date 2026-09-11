/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE

#include <kobox2_test/bootstrap.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "bootstrap check failed at line %u\n", __LINE__); \
        return 1; \
    } \
} while (0)

static int fd_count(void) {
    DIR *directory = opendir("/proc/self/fd");
    struct dirent *entry;
    int count = 0;

    if (!directory) {
        return -1;
    }
    while ((entry = readdir(directory))) {
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
            count++;
        }
    }
    closedir(directory);
    return count;
}

int main(void) {
    kb2_test_bootstrap_t sent = {
        .generation = 17, .shared_memory_size = 4096,
        .manifest_size = 128, .grant_size = 128,
        .channel_descriptor_size = 64, .artifact_count = 1,
        .notification_count = 4, .notification_ids = {1, 2, 3, 4},
    };
    kb2_test_bootstrap_t received;
    int sockets[2], descriptors[KB2_TEST_MAX_TRANSFER_FD_COUNT];
    int output[KB2_TEST_MAX_TRANSFER_FD_COUNT];
    int source, before, baseline = fd_count();
    size_t index, count, capacity;
    const size_t expected = kb2_test_bootstrap_descriptor_count(&sent);

    CHECK(baseline >= 0);
    CHECK(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets));
    source = open("/dev/null", O_RDONLY | O_CLOEXEC);
    CHECK(source >= 0);
    for (index = 0; index < expected; index++) {
        descriptors[index] = source;
    }
    before = fd_count();
    /* recvmsg installs all rights, even if the caller has less capacity. */
    for (capacity = 1; capacity < expected; capacity++) {
        CHECK(kb2_test_send_bootstrap(sockets[0], &sent, descriptors, expected));
        CHECK(!kb2_test_receive_bootstrap(sockets[1], &received, output, capacity, &count));
        CHECK(!count && !received.generation && fd_count() == before);
        for (index = 0; index < capacity; index++) {
            CHECK(output[index] == -1);
        }
    }
    /* SO_PASSCRED adds an unexpected ancillary record before SCM_RIGHTS. */
    {
        int enabled = 1;

        CHECK(!setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)));
        CHECK(kb2_test_send_bootstrap(sockets[0], &sent, descriptors, expected));
        CHECK(!kb2_test_receive_bootstrap(sockets[1], &received, output, expected, &count));
        CHECK(!count && fd_count() == before);
        enabled = 0;
        CHECK(!setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)));
    }
    CHECK(kb2_test_send_bootstrap(sockets[0], &sent, descriptors, expected));
    CHECK(kb2_test_receive_bootstrap(sockets[1], &received, output, expected, &count));
    CHECK(count == expected && received.generation == sent.generation);
    CHECK(fd_count() == before + (int)expected);
    for (index = 0; index < count; index++) {
        CHECK(fcntl(output[index], F_GETFD) & FD_CLOEXEC);
        close(output[index]);
    }
    /* The native transfer has no synthetic channel descriptors, but must
     * preserve exactly the same FD ownership on every rejection path.
     */
    for (capacity = 1; capacity < expected; capacity++) {
        CHECK(kb2_test_send_handles(sockets[0], descriptors, expected));
        CHECK(!kb2_test_receive_handles(sockets[1], output, capacity, &count));
        CHECK(!count && fd_count() == before);
        for (index = 0; index < capacity; index++) {
            CHECK(output[index] == -1);
        }
    }
    {
        int enabled = 1;
        CHECK(!setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)));
        CHECK(kb2_test_send_handles(sockets[0], descriptors, expected));
        CHECK(!kb2_test_receive_handles(sockets[1], output, expected, &count));
        CHECK(!count && fd_count() == before);
        enabled = 0;
        CHECK(!setsockopt(sockets[1], SOL_SOCKET, SO_PASSCRED, &enabled, sizeof(enabled)));
    }
    CHECK(kb2_test_send_handles(sockets[0], descriptors, expected));
    CHECK(kb2_test_receive_handles(sockets[1], output, expected, &count));
    CHECK(count == expected && fd_count() == before + (int)expected);
    for (index = 0; index < count; index++) {
        CHECK(fcntl(output[index], F_GETFD) & FD_CLOEXEC);
        close(output[index]);
    }
    close(source);
    close(sockets[0]);
    close(sockets[1]);
    CHECK(fd_count() == baseline);
    return 0;
}
