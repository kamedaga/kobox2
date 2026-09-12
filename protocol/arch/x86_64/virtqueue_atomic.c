/* SPDX-License-Identifier: MIT */
#include <kobox2/virtqueue_x86_64.h>

#if !defined(__x86_64__) || (!defined(__GNUC__) && !defined(__clang__))
#error "This shared-memory backend requires x86_64 and GCC-compatible atomics"
#endif

_Static_assert(__GCC_ATOMIC_SHORT_LOCK_FREE == 2, "Interprocess u16 atomics must be lock-free");

static uint16_t load_acquire(const void *address) {
    return __atomic_load_n((const uint16_t *)address, __ATOMIC_ACQUIRE);
}

static void store_release(void *address, uint16_t value) {
    __atomic_store_n((uint16_t *)address, value, __ATOMIC_RELEASE);
}

static void full_fence(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

const kb2_vq_atomic_ops_t kb2_vq_x86_64_atomics = {
    .load_acquire = load_acquire,
    .store_release = store_release,
    .fence = full_fence,
};
