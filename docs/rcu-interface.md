# RCU interface

## Scope

RCU is core runtime interface 7. It depends on memory, CPU, synchronization,
thread, time, and workqueue. It provides classic RCU and SRCU domains,
read-side tokens, grace periods, deferred callbacks, and callback barriers.

## Domains

`default_domain` returns the borrowed classic domain for the core generation.
`domain_create` creates a classic or SRCU domain owned by the calling binding.
A created domain is visible only to its owner. Domain destroy closes admission,
waits for readers and callbacks, and releases the domain. The default domain is
released by core cleanup. Owners serialize destroy initiation with new domain
operations; releases of already-held tokens remain valid during the drain.

Classic read-side sections are nonblocking and may nest, preempt, and migrate.
SRCU read-side sections may also block. A read token is unique, belongs to its
binding and domain, and is released exactly once by its acquiring execution
thread. Nested tokens may be released in any order.

`read_lock` establishes acquire ordering before protected loads. `read_unlock`
establishes release ordering after protected accesses. A completed grace period
provides a full ordering boundary with every reader it covered.

## Grace periods

Each accepted reader receives a domain read sequence. `synchronize` snapshots
that sequence and waits only for covered readers. Later readers do not extend
the grace period. Calling it while the same execution thread holds a covered
token returns `DEADLOCK`.

`EXPEDITED` requests the provider's lowest-latency grace-period path and has the
same completion and ordering contract. A provider whose normal path already
tracks readers exactly may use that path for both forms.

`quiescent_state` records a classic-domain quiescent point and wakes
grace-period progress. It requires that the caller hold no token for that
domain. SRCU uses explicit token release as its quiescent event.

## Callbacks and barriers

`call` snapshots the read sequence and appends one callback to the domain FIFO.
It returns after registration. The callback starts in provider worker thread
context after all covered readers release their tokens. Callback completion has
release ordering, and callbacks in one domain start in registration order.
Callbacks are nonblocking; blocking RCU operations from an RCU callback return
`DEADLOCK`. Blocking work is submitted to a workqueue.

`barrier` snapshots the domain callback sequence and waits for all covered
callbacks to complete. Later callbacks are outside that barrier. A callback
whose own completion is covered receives `DEADLOCK`; a covered reader held by
the caller is handled identically to `synchronize`.

## Quiesce

RCU quiesce closes reader, callback, and domain admission, completes every
accepted grace period and callback, then stops callback workers. Owned idle
domains remain available for destruction during cleanup.
