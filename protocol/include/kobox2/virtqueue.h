/* SPDX-License-Identifier: MIT */
#ifndef KOBOX2_VIRTQUEUE_H
#define KOBOX2_VIRTQUEUE_H

#include <kobox2/protocol.h>
#include <stdatomic.h>

typedef enum kb2_vq_status {
    KB2_VQ_OK,
    KB2_VQ_EMPTY,
    KB2_VQ_FULL,
    KB2_VQ_INVALID,
    KB2_VQ_STALE,
    KB2_VQ_FAULTED,
    KB2_VQ_IO
} kb2_vq_status_t;

typedef enum kb2_vq_side {
    KB2_VQ_DRIVER, /* available producer / used consumer */
    KB2_VQ_DEVICE  /* available consumer / used producer */
} kb2_vq_side_t;

/* Transport addresses, not native addresses. The backend owns registrations
 * and mappings through teardown. read snapshots potentially hostile bytes;
 * failed I/O faults the channel and must never be retried as a publication.
 * Atomic methods read/write host-valued u16 in little-endian shared storage.
 * fence is a full store-load barrier, required by EVENT_IDX's wake handshake.
 * Callback success is zero. No callback may reenter this queue.
 */
typedef struct kb2_vq_memory {
    void *context;
    int (*read)(void *, uint64_t, void *, size_t);
    int (*write)(void *, uint64_t, const void *, size_t);
    int (*load_acquire)(void *, uint64_t, uint16_t *);
    int (*store_release)(void *, uint64_t, uint16_t);
    void (*fence)(void *);
    /* Host-authorized payload arenas, excluding the channel header and other
     * lanes' storage. Return nonzero to allow. The policy is private, immutable,
     * and identical at both endpoints; it must isolate writable lane arenas.
     * The engine also checks registration rights and every queue's metadata.
     */
    int (*payload_allowed)(void *, uint32_t queue_id, uint64_t, uint64_t, uint32_t rights);
} kb2_vq_memory_t;

/* Private channel state. Initialize once before sharing among local lanes.
 * Model arrays are immutable private copies, not views of peer memory. The
 * channel model's memory is established by the host's authenticated transfer.
 */
typedef struct kb2_vq_channel {
    kb2_protocol_channel_t identity;
    const kb2_protocol_queue_t *queues;
    size_t queue_count;
    const kb2_protocol_region_t *regions;
    size_t region_count;
    atomic_uint faulted;
} kb2_vq_channel_t;

typedef struct kb2_vq_segment {
    uint64_t address;
    uint32_t length;
    uint16_t descriptor; /* private direct/indirect chain index */
    uint16_t writable;
} kb2_vq_segment_t;

/* Caller-owned private chain storage. DRIVER fills segments/head/indirect
 * before publish; DEVICE receives snapshots into its supplied segment array.
 * Do not mutate, share across queues, or free an owned chain. Ownership ends
 * at DEVICE complete, or DRIVER release after take_used/copy_response.
 */
typedef struct kb2_vq_chain {
    kb2_vq_segment_t *segments;
    size_t capacity, count;
    uint16_t head;
    uint64_t indirect;            /* table transport address; meaningful only when length != 0 */
    uint32_t indirect_length;     /* full table lease in bytes, including unused entries */
    uint16_t indirect_descriptor; /* main-table descriptor referencing the indirect table */
    size_t indirect_first; /* first segment in the indirect table; earlier segments are direct */
    uint64_t readable, writable, written;
    uint32_t used_length;
    unsigned int state; /* private implementation state; initially zero */
} kb2_vq_chain_t;

/* One local owner per queue, distinct from the remote peer's queue object.
 * slots and descriptor_owner each have queue_size entries, initially free.
 * No allocation or OS/architecture assumptions in this state machine.
 */
typedef struct kb2_vq {
    kb2_vq_channel_t *channel;
    kb2_protocol_queue_t queue;
    kb2_vq_memory_t memory;
    kb2_vq_side_t side;
    kb2_vq_chain_t **slots;
    uint16_t *descriptor_owner;
    uint16_t produced, consumed;
    uint32_t owned;
} kb2_vq_t;

kb2_vq_status_t kb2_vq_channel_init(kb2_vq_channel_t *out,
                                    const kb2_protocol_channel_t *identity,
                                    const kb2_protocol_queue_t *queues,
                                    size_t queue_count,
                                    const kb2_protocol_region_t *regions,
                                    size_t region_count);
kb2_vq_status_t kb2_vq_bind(kb2_vq_t *out,
                            kb2_vq_channel_t *channel,
                            uint32_t queue_id,
                            kb2_vq_side_t side,
                            const kb2_vq_memory_t *memory,
                            kb2_vq_chain_t **slots,
                            uint16_t *descriptor_owner,
                            size_t capacity);
void kb2_vq_channel_fault(kb2_vq_channel_t *channel);

kb2_vq_status_t
kb2_vq_publish(kb2_vq_t *queue, uint64_t generation, kb2_vq_chain_t *chain, int *notify);
kb2_vq_status_t kb2_vq_take_available(kb2_vq_t *queue, uint64_t generation, kb2_vq_chain_t *chain);
kb2_vq_status_t kb2_vq_copy_request(kb2_vq_t *queue,
                                    uint64_t generation,
                                    kb2_vq_chain_t *chain,
                                    uint64_t offset,
                                    void *out,
                                    size_t size);
/* Append output sequentially; complete publishes exactly the bytes written. */
kb2_vq_status_t kb2_vq_write_response(
    kb2_vq_t *queue, uint64_t generation, kb2_vq_chain_t *chain, const void *bytes, size_t size);
kb2_vq_status_t
kb2_vq_complete(kb2_vq_t *queue, uint64_t generation, kb2_vq_chain_t *chain, int *notify);
kb2_vq_status_t kb2_vq_take_used(kb2_vq_t *queue, uint64_t generation, kb2_vq_chain_t **chain_out);
kb2_vq_status_t kb2_vq_copy_response(kb2_vq_t *queue,
                                     uint64_t generation,
                                     kb2_vq_chain_t *chain,
                                     uint64_t offset,
                                     void *out,
                                     size_t size);
kb2_vq_status_t kb2_vq_release(kb2_vq_t *queue, uint64_t generation, kb2_vq_chain_t *chain);
/* Before sleeping: publish the event index, full barrier, then recheck ring
 * progress. If ready != 0, drain instead of sleeping. Notifications may repeat.
 */
kb2_vq_status_t kb2_vq_arm(kb2_vq_t *queue, uint64_t generation, int *ready);

#endif
