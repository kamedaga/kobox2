# Time interface

## Scope

Time is core runtime interface 5. It depends on synchronization and thread. It
provides three clocks, absolute sleep, busy delay, and owned one-shot or
periodic timers.

## Clocks and deadlines

`MONOTONIC` advances while the system is running. `BOOTTIME` also includes
suspend. `REALTIME` is the adjustable wall clock. Values are unsigned
nanoseconds; overflow is reported as `EXHAUSTED`.

Sleep, busy delay, and timer deadlines are absolute in their selected clock.
A deadline of zero or a deadline already reached completes immediately.
Realtime deadlines follow clock adjustment: a forward step may expire them and
a backward step postpones them.

`sleep_until` is a thread-context wait. With `INTERRUPTIBLE`, stop resolves to
`CANCELED` and interrupt resolves to `INTERRUPTED`; reaching the deadline wins
a simultaneous race. Core quiesce cancels every sleep. `busy_wait_until` keeps
the caller running and is valid in atomic context.

## Timer placement and callback context

A timer belongs to its creating binding and generation. `PINNED` captures the
calling logical CPU at creation. An unpinned timer receives an online logical
CPU on each arm.

`ATOMIC` callbacks execute in softirq context on the assigned CPU and use only
nonblocking operations. `THREAD` callbacks execute in provider-managed thread
context on that CPU. `thread.current` exposes a borrowed handle in thread
callbacks. Callbacks are serialized per timer.

A deferrable timer does not establish the next CPU wake by itself. Once due, it
runs at the next provider scheduling activity on its assigned CPU. Other timers
retain their normal deadline behavior.

## Arm and periodic delivery

`timer_arm` atomically replaces the pending arm and queued delivery with a new
arm generation. A zero period selects one-shot delivery. A periodic timer keeps
its phase from the supplied absolute deadline.

`expiration_count` is the exact number of expirations represented by one
callback. Expirations accumulated before dispatch or while a callback is
running are coalesced. Counts larger than `UINT64_MAX` are delivered across
successive callbacks without losing expirations.

Arm or cancel from a running callback controls subsequent delivery; the current
callback always returns normally. A newly armed generation never receives an
expiration from the replaced generation.

## Cancel, destroy, and quiesce

`timer_cancel` removes the pending arm and queued delivery without waiting. Its
result is true when either existed. A running callback continues.
`timer_cancel_sync` performs the same cancellation and waits for that callback
to return. Calling synchronous cancel or destroy from the same timer callback
returns `DEADLOCK`.

`timer_remaining` reports zero without a pending arm and otherwise reports the
distance to its next deadline, clamped to zero. `timer_is_pending` includes an
armed deadline or queued delivery and excludes a callback already running.

Destroy synchronously drains the timer and releases its handle. The owner
serializes destroy with every other handle operation. Time quiesce rejects new
sleep, create, and arm operations, cancels sleepers and queued timers, then
waits for running callbacks and provider execution threads to finish.
