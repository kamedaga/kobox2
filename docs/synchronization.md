# Synchronization

## Scope

Synchronization is interface 3 of the core runtime. It depends on memory and
CPU. Its object operations are spin, mutex, rwlock, semaphore, event, and
completion. Atomic, bitmap, refcount, and barrier operations are compiler
inline primitives and do not require a binding.

## Inline primitives

`atomic32`, `atomic64`, `refcount`, and `bitmap_word` have fixed LP64 layouts.
Read-once and write-once functions cover naturally aligned 8, 16, 32, and
64-bit values. Bitmap bit zero is the least-significant bit.

Load accepts relaxed, acquire, and sequentially consistent ordering. Store
accepts relaxed, release, and sequentially consistent ordering. Read-modify-
write and fences accept every declared ordering. Compare-exchange failure
ordering is relaxed, acquire, or sequentially consistent and does not exceed
the success ordering. An invalid ordering faults the closure.

Refcount zero is acquired only by `increment_not_zero`. Underflow, increment
from zero, and overflow saturate the value and fault the closure. A decrement
to zero has acquire-release ordering; other refcount operations are relaxed.

## Admission and wait resolution

Waiters enter each object in FIFO order. A wake resolves an available object
predicate first, then a thread stop, an interrupt, and an expired deadline.
Spurious host wakes are retried internally. Deadlines are absolute monotonic nanoseconds;
zero selects an infinite wait. Try operations return `OK` with a false result
when the object is unavailable.

Blocking waits are valid only in thread context with preemption, migration,
local IRQ, and bottom-half nesting at zero. The provider implements them with
a private park/wake backend below the public thread interface.

## Locks

Spin, mutex, and rwlock are non-recursive. Recursion, read-to-write upgrade,
and write-to-read downgrade return `DEADLOCK`. Every acquired lock is released
by its acquiring thread; another thread returns `OWNER`. Owner identity is a
process-lifetime monotonic thread ID and is never derived from a reusable host
thread address.

A held spin disables preemption and never sleeps. Mutex ownership is exclusive.
Rwlock admission consists of FIFO reader phases separated by one writer: all
consecutive readers preceding the next writer enter together, while readers
behind a writer do not bypass it. Each read owner is tracked independently.

## Counters and notifications

Semaphore down consumes one permit. Up grants permits directly to queued
waiters in FIFO order, then adds the remainder to the stored count. A zero up
count is invalid; a remainder exceeding the configured maximum returns
`EXHAUSTED` without changing the object.

An auto-reset event stores one coalesced signal or grants it to one waiter. A
manual-reset event remains signaled, releases all waiters, and admits future
waiters until reset.

Completion stores individual completion counts. `complete` grants one waiter
or increments the count. `complete_all` sets a latch and releases all current
and future waiters. `reinit` clears the count and latch and returns `BUSY` while
waiters exist.

## Lifetime

Objects belong to their creating binding and generation. Destroy is serialized
by the owner and returns `BUSY` while a lock is held or waiters exist. A sync
binding remains busy until every owned object is destroyed.

Core quiesce marks every synchronization object closing, rejects new waits,
and completes queued waits with `CANCELED`. Unlock, signal, reset, reinit, and
destroy remain available for cleanup. Cleanup completes after every object is
destroyed; otherwise closure teardown terminates the sandbox process.
