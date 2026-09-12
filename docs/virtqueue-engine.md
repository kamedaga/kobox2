# Shared virtqueue engine

`protocol/include/kobox2/virtqueue.h` and `protocol/src/virtqueue.c` implement
the split-ring state machine shared by controller-side clients and separate
GPL sandbox processes. All shared implementation files are MIT licensed.
The older `protocol/test/.../split_virtqueue` fixture remains test-only.
The wire schema is unchanged; these are unnumbered `dev` implementation APIs.

## Boundaries and ownership

- The engine is portable C11. It has no allocator, syscall, native handle,
  Linux structure, or implicit process-global state.
- The OS backend supplies pinned mappings and authenticated, immutable private
  channel/queue/registration models. `kb2_protocol_channel_validate` validates
  that model without serializing it again.
- The mapped-memory backend resolves transport addresses, checks local mapping
  rights, and restricts payloads to host-authorized, disjoint lane arenas.
  It rejects overlapping native and transport mappings. The host must also
  exclude aliases of the same backing memory: distinct virtual addresses alone
  cannot prove that two VMOs or registrations do not alias.
- `protocol/arch/x86_64/virtqueue_atomic.c` supplies aligned, lock-free u16
  acquire/release operations and the full store-load fence for EVENT_IDX.
  Architecture operations are explicitly supplied to the mapped backend.
  This is coherent CPU shared memory, not hardware DMA or MMIO.
- Each endpoint has its own channel and queue objects. One local owner operates
  each queue. Only the channel fault flag may be changed concurrently.
  Faulting stops subsequent admissions; the host must quiesce already-running
  operations before unmapping. A fault flag is not capability revocation.

Each queue borrows a caller-owned slot array, descriptor-owner array, and
private chain workspaces. DRIVER publishes available entries and consumes
used entries. DEVICE snapshots available chains and publishes used entries.
Neither endpoint may mutate or free a chain it owns.

DRIVER ownership ends at `release`, after `take_used` and all response copies.
DEVICE ownership ends after successful used publication. An I/O failure,
including failure to read EVENT_IDX after publication, retains ownership and
faults the channel. It must not be retried as a fresh publication. Teardown is
the host's responsibility; there is no in-place queue reset API.

## Validation and notification

The engine checks indices, bounded acyclic chains, read-before-write ordering,
registration rights, arena permissions, ring metadata exclusion, active
descriptor ownership, writable payload overlap, and reported used lengths.
Unused entries in an indirect table remain covered by its full table lease.
Direct prefixes followed by indirect tables are supported; indirect-reference
WRITE and NEXT-clear next fields are ignored as required by
[Virtio 1.0, indirect descriptors](https://docs.oasis-open.org/virtio/virtio/v1.0/virtio-v1.0.html).
Nested indirect tables and INDIRECT with NEXT are rejected.

An invalid local publication returns INVALID before shared writes. Invalid
peer input faults the whole local channel. First backend I/O failure returns
IO; later operations return FAULTED. Wrong generation returns STALE without
accessing shared memory. Copy operations can partially modify their destination
before an I/O failure; their output must not be consumed after failure.

Descriptor snapshots are private, but payload bytes remain untrusted. The
caller copies a complete request and validates its envelope, generation,
correlation, command, session and spans before executing subsystem operations.
The engine does not implement GPU sessions, envelope validation, control-fault
delivery, notification endpoint creation, or generation revocation/reset.

Publication writes data, release-publishes the index, performs a full fence,
then reads the peer event threshold. `arm` publishes the consumer threshold,
performs the full fence, and rechecks the producer index. A caller may sleep
only after an empty recheck. Wakes may be coalesced, duplicated, or already
consumed; ring progress is authoritative. Out-of-order used IDs and u16 index
wrap are supported.

The bounded overlap checks favor explicit ownership over throughput: some
checks are quadratic in chain length and scan the configured queue slots.
Hosts must select resource limits accordingly; maximum-size performance has
not been established.

## Verification

With `KB2_BUILD_SANDBOX=OFF` and `KB2_ENABLE_SANITIZERS=ON`, both GCC and Clang
pass all seven CTest tests, including the new engine and process tests.

`virtqueue_engine_test.c` covers SG copies, direct/indirect/mixed chains,
unused-table leases, malformed chains and completions, peer mutation after
snapshot, local rejection without writes, ownership retention, channel-wide
faults, callback failures before/after publication, EVENT_IDX arm races and
65,536-index wrap.

`virtqueue_process_test.c` uses independent processes, different virtual
mapping addresses, memfd and eventfd, and the actual x86_64 backend. Each of
two fresh generations exchanges 68,000 requests in batches with direct,
indirect and mixed chains, reversed completion order, coalesced/redundant
wakes and index wrap. This proves the software transport path, not GPU
device reset, native host capability revocation or client-death recovery.
