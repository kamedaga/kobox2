# Core runtime

## Identity

The core runtime is one `dev` ABI family. Its identity and complete operation
catalog are the canonical SHA-256 of `protocol/schema/core_runtime.json`.

`core_operations` is a directory with `bind` and `unbind`. `bind` receives the
module context, interface ID, and exact family digest, then returns a typed
operation table and an opaque binding object. Every interface call receives
that binding object.

## Interfaces

| ID | Interface | Direct dependencies | Operations |
| --- | --- | --- | --- |
| 1 | memory | — | pages, aligned allocation, reallocation, caches, statistics |
| 2 | CPU | memory | topology, execution context, preemption, migration, local IRQ/BH state, per-CPU storage |
| 3 | synchronization | memory, CPU | atomic, refcount, bitmap, barriers, spin, mutex, rwlock, semaphore, event, completion |
| 4 | thread | memory, CPU, synchronization | current, create, join, detach, stop, interrupt, park, wake, affinity, priority |
| 5 | time | synchronization, thread | clocks, sleep, busy delay, one-shot and periodic timers |
| 6 | workqueue | memory, CPU, synchronization, thread, time | queues, work, delayed submit and reschedule, cancel, flush |
| 7 | RCU | memory, CPU, synchronization, thread, time, workqueue | classic RCU, SRCU, callbacks, grace periods, barriers |

The schema fixes every operation ID, input, output, flag, blocking property,
callback context, and status. Absolute wait deadlines use the monotonic clock;
zero selects an infinite wait.

## Semantics

Page allocation uses 4096-byte pages and orders. Byte and cache allocation use
power-of-two alignment. Failed reallocation preserves the original allocation.
Atomic memory requests complete without sleeping.

CPU IDs are dense and stable for a closure generation. Preemption, migration,
local IRQ, and bottom-half state is balanced per thread. IRQ delivery observes
the logical CPU mask. Spin operations never sleep. Mutex, rwlock, semaphore,
event, and completion waits support try, interruptible, and absolute-deadline
forms. Spin ownership holds preemption disabled. Locks are released by their
acquiring thread. Events support auto-reset and manual-reset; completions are
counting objects with a `complete_all` latch cleared by reinitialization.
Atomic32, atomic64, refcount, bitmap, and memory-barrier primitives have fixed
LP64 layouts and explicit memory ordering. Refcounts saturate and fault on
overflow; zero is acquired only through `increment_not_zero`.
Synchronization waiters use FIFO admission. Rwlocks form reader phases around
writers. Predicate success wins a race with interruption or deadline expiry.
Core quiesce cancels queued waits through a private park/wake backend.

Thread stop and interrupt are independent sticky states. Unpark carries one
coalesced permit; wake targets only a current park. Core quiesce requests stop,
interrupts and wakes parked threads, and joins every tracked thread. The full
contract is defined by [thread-interface.md](./thread-interface.md). Timer
synchronous cancel completes after its callback returns; callbacks are
serialized per timer. The full contract is defined by
[time-interface.md](./time-interface.md). Work callbacks are serialized per
work, and flush covers submissions preceding its flush epoch. The complete
contract is defined by [workqueue-interface.md](./workqueue-interface.md).
RCU grace periods snapshot readers, and callback barriers snapshot callback
registration epochs. The complete contract is defined by
[rcu-interface.md](./rcu-interface.md).

## Ownership and lifetime

A binding belongs to one module node and generation. Created objects and
registered callbacks belong to that binding. `thread.current`,
`rcu.default_domain`, and `cpu.percpu_address` return borrowed objects. Shared
use requires an explicitly shared object owned by the closure.

Each node binds interfaces in dependency order and unbinds them in reverse
dependency order.

Normal module cleanup releases every owned object and then unbinds. Operation
tables remain valid through core cleanup. CPU topology remains stable for the
generation.

## Lifecycle and fault

Initialization follows interface ID order. Quiesce and cleanup use reverse
order. Core cleanup completes after all bindings, threads, timers, work,
callbacks, synchronization objects, caches, and allocations are released.

Partial init failure rolls back initialized interfaces in reverse order.
Ownership violations, stale callbacks, and allocator corruption fault the
closure. Quiesce failure or a busy cleanup completes by terminating the
sandbox process and revoking its resources.
