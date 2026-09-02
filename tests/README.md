# Tests

Controller tests cover configuration rejection, lifecycle transitions,
generation rollover, host failure, and crash/restart. Protocol tests cover
round trips, bounds, schema mismatch, reserved fields, region rights, and
message envelopes.

On Linux, the test host starts a separate sandbox process and transfers one
shared-memory object and four eventfds over a bootstrap socket. Split
virtqueues carry request completions and lifecycle events without polling. The
tests cover direct and indirect descriptors, `EVENT_IDX`, wrap-around, region
rights, malformed input, quiesce, deterministic process faults, pidfd-confirmed
revocation, reset, reap, and regeneration. Device-specific acceptance tests
remain in the host OS repository.

The GPL sandbox fixture loads `fixture_core.so`, relocates an ET_REL
`fixture_module.ko`, and invokes its initialization and cleanup entry points.
The module exercises allocation, locking, wait/wake, two native threads,
per-CPU state, an RCU grace period, and monotonic time before returning its
result through the request virtqueue.
