# Thread interface

## Scope

Thread is core runtime interface 4. It depends on memory, CPU, and
synchronization. It provides native concurrent execution, lifecycle control,
cooperative stop, interruption, parking, naming, logical-CPU affinity, and
priority.

## Handle state

A created thread follows this state machine:

```text
STARTING -> RUNNING -> EXITED -> JOINED
                    \-> DETACHED
```

`create` returns after native creation, name, affinity, priority, and the entry
gate are established. `START_PARKED` holds the entry behind the park gate until
an unpark permit is consumed. The entry return value is the join exit status.

One successful join reaps a joinable thread. A timed-out join leaves it
joinable. Join and detach are mutually exclusive and the owner serializes them
with every other operation on that handle. Self-join returns `DEADLOCK`. Detach transfers
reaping to the core; its binding remains busy until the entry has returned.

`current` returns the borrowed handle of the provider-managed calling thread.
Created handles belong to their creating binding and generation.

## Stop, interrupt, and park

Stop and interrupt are independent sticky bits. `request_stop` sets stop and
wakes a park or interruptible blocking point. The target observes it with
`stop_requested`. `interrupt` sets interrupt and wakes an interruptible sync,
time, or park wait. Only the target clears its interrupt bit.

`park` is a self operation. It first resolves stop, interrupt, an unpark permit,
a pending wake, then the absolute monotonic deadline. Stop returns `CANCELED`,
interrupt returns `INTERRUPTED`, and expiry returns `TIMED_OUT`.

`unpark` stores one coalesced permit and wakes a current park. The next park
consumes that permit. `wake` completes only a park already in progress and is
not retained. Spurious native wakes are retried internally.

The private execution record used by park is also registered by interruptible
synchronization waits. Their resolution order is predicate, stop, interrupt,
then deadline. This keeps the public dependency direction thread to
synchronization while allowing interrupt and stop delivery without lost wakes.

## Scheduling attributes

Names are copied by the core and contain at most 63 bytes; a shorter native
diagnostic name may mirror them. Stack size zero
selects the provider default. An explicit stack is at least 64 KiB and is a
multiple of 4096 bytes.

A CPU mask is expressed in dense logical CPU IDs. A null mask with zero words
selects all online logical CPUs; an explicit mask has the exact generation CPU
word count, contains at least one CPU, and has no out-of-range bits. Affinity is
installed before entry and may later be replaced atomically.

Priority is an integer from 0 through 39. Zero is highest, 20 is default, and
39 is lowest. `HIGH_PRIORITY` selects priority zero during creation. Provider
authority is required when raising priority.

## Exit and quiesce

A thread exits with balanced CPU nesting and without an owned spin, mutex, or
rwlock. A violation faults the closure.

Thread quiesce rejects creation, requests stop, interrupts and wakes every
created thread, then waits for every entry to return and reaps all handles.
Cleanup releases provider thread records after synchronization users have
drained. A thread that does not cooperate causes closure teardown to terminate
the sandbox process.
