# Linux provider runtime

## Provider graph

The Linux closure uses three shared providers:

```text
core/primitive.so -> device-pci.so -> drm.so
```

`device-pci.so` depends on `core/primitive.so`. `drm.so` depends on both.
Each provider is one closure node with `dev` identity and explicit init,
quiesce, and cleanup entries.

## Lifecycle

Provider state is:

```text
BOUND -> INITIALIZING -> ACTIVE -> QUIESCING -> QUIESCED -> CLEANING -> CLEAN
```

The loader initializes providers in dependency order and quiesces and cleans
them in reverse order. Every entry receives the same immutable module context.
An init entry rolls back its partial state before returning failure.

Each provider owns a generated init table containing its retained Linux
initializers. Entries are ordered by Linux init level, linked section order,
and symbol name. The table digest is part of the provider inventory.

## Core memory arena

`core/primitive.so` receives one closure-shared memory resource with `READ`,
`WRITE`, and `MAP` rights. Its interface binding yields one page-aligned mapped
range that remains valid through core cleanup. The core memory slot ID is `1`.

The arena has 4096-byte pages. Allocator metadata occupies its aligned prefix;
the remaining complete pages form a buddy allocator. Allocation and release
use page orders, are safe across provider threads, and maintain exact free and
allocated page counts. An allocation records its head and order so release can
validate ownership and coalesce buddies. Arena destruction succeeds after every
allocated page has been returned.

The grant and mapping contract is defined by
[memory-arena-interface.md](./memory-arena-interface.md).
The complete core service contract is defined by
[core-runtime.md](./core-runtime.md).

Core init establishes the arena before Linux page, slab, per-CPU, thread, time,
lock, workqueue, and RCU initialization. Core cleanup drains users and releases
the arena state after dependent providers and modules are clean.

RCU tracks explicit read tokens by sequence. One unbound provider worker runs
eligible callbacks in each domain FIFO and exposes a borrowed thread handle for
the callback owner's node. Eventfd notification wakes grace-period and callback
progress without polling.

## Logical CPUs

The closure loader fixes a nonzero logical CPU count in every immutable module
context. IDs are dense from zero, remain stable for the generation, and are all
possible and online. The loader thread starts on logical CPU zero; provider
threads carry their assigned logical CPU ID.

Preemption, migration, local IRQ, and bottom-half nesting is thread-local and
binding-scoped. A binding remains busy while any nesting is active. Per-CPU
allocations contain one independently aligned object for every logical CPU and
remain owned by their creating CPU binding.

## Synchronization

The synchronization interface initializes after CPU state. Object state and
FIFO waiter queues live in the core arena. Spin ownership uses the CPU
preemption counter. Sleepable objects use private x86-64 Linux futex syscalls
with absolute monotonic deadlines; no public thread-interface dependency or
libc synchronization import is introduced.

Quiesce closes every object and wakes queued waiters with `CANCELED`. Cleanup
then requires modules to destroy every object before the arena is released.
The complete contract is defined by
[synchronization.md](./synchronization.md).

## Threads

The thread interface uses native Linux process threads. Creation has a start
handshake so name, native affinity, priority, TLS execution identity, and an
optional parked entry gate are established before module code runs. Logical
CPU masks map to the process CPU set captured for the closure generation.

Private futex registrations connect thread interrupt and stop state to park and
interruptible synchronization waits. Quiesce sends both states, wakes all
blocking points, and reaps every created thread. The complete contract is
defined by [thread-interface.md](./thread-interface.md).

## Time and timers

Linux maps the three core clocks directly to native clock IDs. Absolute sleep
and timer scheduling preserve realtime adjustment and boottime suspend
semantics. Timer dispatch is partitioned by logical CPU; atomic callbacks use
softirq context and thread callbacks use provider-managed native threads. The
complete contract is defined by [time-interface.md](./time-interface.md).

## Workqueues

Linux workqueues use native queue-owned execution domains. Bound domains are
affined to logical CPUs; unbound domains use the closure CPU set. A monotonic
dispatcher promotes delayed work, and native workers enforce active limits,
ordered execution, and reclaim capacity. The complete contract is defined by
[workqueue-interface.md](./workqueue-interface.md).

## Provider initialization

`device-pci.so` initializes retained IRQ, PCI, and IOMMU entries after core is
active. `drm.so` initializes retained dma-buf and video entries after core and
device-pci are active. Root modules initialize after all providers are active.

`READY` requires every provider and root module to be active. Quiesce completion
requires provider work, callbacks, and references owned by the closure to be
drained.
