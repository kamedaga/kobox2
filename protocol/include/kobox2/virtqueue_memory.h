/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_VIRTQUEUE_MEMORY_H
#define KOBOX2_VIRTQUEUE_MEMORY_H

#include <kobox2/virtqueue.h>

/* Private, pinned mappings supplied by the OS backend. Local access rights
 * describe the actual mapping, not the remote consumer's region rights.
 */
typedef struct kb2_vq_mapping {
    uint64_t transport_base;
    size_t length;
    void *address;
    uint32_t local_rights;
} kb2_vq_mapping_t;

/* The host assigns disjoint arenas to lanes. Do not include channel headers,
 * ring metadata, other lanes' payloads, or unrelated host-private memory.
 * These are private authorizations, never accepted from a request descriptor.
 */
typedef struct kb2_vq_arena {
    uint32_t queue_id;
    uint32_t rights;
    uint64_t transport_base;
    uint64_t length;
} kb2_vq_arena_t;

/* Architecture implementation: aligned u16, little-endian storage. The full
 * fence orders stores before subsequent loads, including EVENT_IDX rechecks.
 * Only coherent CPU shared memory is supported; these are not MMIO operations.
 */
typedef struct kb2_vq_atomic_ops {
    uint16_t (*load_acquire)(const void *);
    void (*store_release)(void *, uint16_t);
    void (*fence)(void);
} kb2_vq_atomic_ops_t;

typedef struct kb2_vq_mapped_memory {
    const kb2_vq_mapping_t *mappings;
    size_t mapping_count;
    const kb2_vq_arena_t *arenas;
    size_t arena_count;
    kb2_vq_atomic_ops_t atomics;
} kb2_vq_mapped_memory_t;

/* Arrays and mappings remain immutable and live until the host has quiesced
 * all users. Revocation/unmapping is a host lifecycle operation, not a queue
 * reset. Initialization rejects overlapping transport/native mappings/arenas.
 */
kb2_vq_status_t kb2_vq_mapped_memory_init(kb2_vq_mapped_memory_t *out,
                                          const kb2_vq_mapping_t *mappings,
                                          size_t mapping_count,
                                          const kb2_vq_arena_t *arenas,
                                          size_t arena_count,
                                          const kb2_vq_atomic_ops_t *atomics);
kb2_vq_memory_t kb2_vq_mapped_memory_ops(kb2_vq_mapped_memory_t *memory);

#endif
