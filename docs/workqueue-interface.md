# Workqueue interface

## Scope

Workqueue is core runtime interface 6. It depends on memory, CPU,
synchronization, thread, and time. It provides owned execution queues, reusable
work items, absolute delayed submission, cancellation, and epoch flushes.

## Queues and placement

A bound queue has one execution domain per logical CPU. An explicit CPU ID
selects that domain; `CPU_ANY` captures the submitting CPU. `UNBOUND` creates
one closure-wide domain and requires `CPU_ANY`.

`maximum_active` limits simultaneous normal callbacks in each domain. Zero
selects a provider-managed positive limit. `ORDERED` requires `UNBOUND` and an
explicit limit of one, is exclusive with `MEMORY_RECLAIM`, and starts eligible
callbacks in ready-transition order.

`HIGH_PRIORITY` uses a distinct priority-zero worker pool. `MEMORY_RECLAIM`
adds one reserved forward-progress worker to each domain outside the normal
active limit. `FREEZABLE` selects cancellation of pending work during core
quiesce; other queues drain accepted work.

Queue names are copied by the core and contain at most 63 bytes. Workers are
native provider threads. A callback runs in thread context in its selected
domain, and `thread.current` returns its borrowed worker handle.

## Work state and submission

A work item belongs to its creating binding and follows:

```text
IDLE -> READY | DELAYED -> RUNNING -> IDLE
                         \-> RUNNING + successor -> READY | DELAYED
```

One pending successor may coexist with a running callback. Callbacks are
serialized per work. `submit` creates an immediate successor. `submit_at` uses
an absolute monotonic deadline; zero or a reached deadline is immediate. Both
return false when a successor is already pending.

`reschedule_at` creates a delayed successor when none exists and returns false.
When one exists, it atomically replaces its deadline and CPU placement, retains
its submission epochs, and returns true. Pending or running work remains
associated with one queue; another queue returns `BUSY`.

## Cancel and flush

`cancel` removes a pending successor and returns whether one existed. A running
callback continues. `cancel_sync` also waits for that callback to return.
Calling synchronous cancel, work destroy, `flush_work`, or `flush_queue` from a
callback that the wait covers returns `DEADLOCK`.

Each accepted successor receives a work sequence and queue submission epoch.
`flush_work` snapshots the work sequence and waits for every covered callback.
`flush_queue` snapshots the queue epoch and waits for every covered callback.
Later submissions are outside that flush. Rescheduling retains the covered
epochs, so it cannot escape an existing flush.

`is_pending` reports the successor state and excludes a callback that is only
running. Work destroy cancels its successor, drains its callback, and releases
the handle. Queue destroy drains every accepted item, stops its execution
domain, and releases the queue. Owners serialize destroy with handle use.

## Quiesce

Workqueue quiesce rejects creation and submission. Pending work on freezable
queues is canceled. Other accepted work becomes eligible at its specified
deadline and drains normally. Quiesce then waits for all callbacks, stops every
worker and dispatcher, and leaves owned idle handles available for cleanup.
