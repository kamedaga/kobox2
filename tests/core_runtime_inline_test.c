// SPDX-License-Identifier: Apache-2.0

#include <kobox2/core_runtime.h>

#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression))                                                    \
            return EXIT_FAILURE;                                              \
    } while (0)

static void invalid_compare_order(void)
{
    kb2_core_atomic32_t atomic = { 1 };
    uint32_t expected = 1;

    (void)kb2_core_atomic32_compare_exchange(
        &atomic, &expected, 2,
        KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELEASE,
        KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE);
}

static void invalid_refcount_increment(void)
{
    kb2_core_refcount_t refcount = { 0 };

    kb2_core_refcount_increment(&refcount);
}

static int expect_fault(void (*operation)(void))
{
    pid_t child = fork();
    int status;

    if (child < 0)
        return -1;
    if (!child) {
        operation();
        _exit(EXIT_SUCCESS);
    }
    if (waitpid(child, &status, 0) != child)
        return -1;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGILL ? 0 : -1;
}

int main(void)
{
    uint8_t once8 = 1;
    uint16_t once16 = 2;
    uint32_t once32 = 3;
    uint64_t once64 = 4;
    kb2_core_atomic32_t atomic32 = { 0 };
    kb2_core_atomic64_t atomic64 = { 0 };
    kb2_core_refcount_t refcount = { 0 };
    kb2_core_bitmap_word_t bitmap = 0;
    uint32_t expected;

    kb2_core_write_once_u8(&once8, 11);
    kb2_core_write_once_u16(&once16, 12);
    kb2_core_write_once_u32(&once32, 13);
    kb2_core_write_once_u64(&once64, 14);
    CHECK(kb2_core_read_once_u8(&once8) == 11);
    CHECK(kb2_core_read_once_u16(&once16) == 12);
    CHECK(kb2_core_read_once_u32(&once32) == 13);
    CHECK(kb2_core_read_once_u64(&once64) == 14);

    kb2_core_atomic32_store(
        &atomic32, 10, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELEASE);
    CHECK(kb2_core_atomic32_load(
              &atomic32, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE) == 10);
    CHECK(kb2_core_atomic32_exchange(
              &atomic32, 20,
              KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQ_REL) == 10);
    expected = 20;
    CHECK(kb2_core_atomic32_compare_exchange(
              &atomic32, &expected, 30,
              KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQ_REL,
              KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE));
    expected = 20;
    CHECK(!kb2_core_atomic32_compare_exchange(
               &atomic32, &expected, 40,
               KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQ_REL,
               KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE) &&
          expected == 30);
    CHECK(kb2_core_atomic32_fetch_add(
              &atomic32, 2, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED) == 30);
    CHECK(kb2_core_atomic32_fetch_sub(
              &atomic32, 1, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED) == 32);
    CHECK(kb2_core_atomic32_fetch_and(
              &atomic32, 0x1f,
              KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED) == 31);
    CHECK(kb2_core_atomic32_fetch_or(
              &atomic32, 0x20,
              KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED) == 31);
    CHECK(kb2_core_atomic32_fetch_xor(
              &atomic32, 0x10,
              KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_SEQ_CST) == 63);

    kb2_core_atomic64_store(
        &atomic64, UINT64_C(0x100000000),
        KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED);
    CHECK(kb2_core_atomic64_fetch_add(
              &atomic64, 1,
              KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQ_REL) ==
          UINT64_C(0x100000000));
    CHECK(kb2_core_atomic64_load(
              &atomic64, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_SEQ_CST) ==
          UINT64_C(0x100000001));

    kb2_core_bit_set(
        &bitmap, 7, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELEASE);
    CHECK(kb2_core_bit_test(
        &bitmap, 7, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQUIRE));
    CHECK(kb2_core_bit_test_and_set(
        &bitmap, 7, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQ_REL));
    CHECK(kb2_core_bit_test_and_clear(
        &bitmap, 7, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_ACQ_REL));
    kb2_core_bit_set(
        &bitmap, 63, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED);
    kb2_core_bit_clear(
        &bitmap, 63, KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_RELAXED);
    CHECK(!bitmap);

    kb2_core_refcount_set(&refcount, 1);
    CHECK(kb2_core_refcount_read(&refcount) == 1);
    kb2_core_refcount_increment(&refcount);
    CHECK(kb2_core_refcount_increment_not_zero(&refcount));
    CHECK(!kb2_core_refcount_decrement_and_test(&refcount));
    CHECK(!kb2_core_refcount_decrement_and_test(&refcount));
    CHECK(kb2_core_refcount_decrement_and_test(&refcount));
    CHECK(!kb2_core_refcount_increment_not_zero(&refcount));

    kb2_core_fence(KB2_CORE_RUNTIME_SYNC_MEMORY_ORDER_SEQ_CST);
    CHECK(!expect_fault(invalid_compare_order));
    CHECK(!expect_fault(invalid_refcount_increment));
    return EXIT_SUCCESS;
}
