# Tests

Controller tests cover configuration rejection, lifecycle transitions,
generation rollover, host failure, and crash/restart. Protocol tests cover
round trips, bounds, schema mismatch, reserved fields, region rights, and
message envelopes.

Closure tests cover DAG reachability, cycles, explicit symbol binding,
resource rights, sharing, sealing, and immutable inspection.

On Linux, the test host starts a separate sandbox process and transfers one
shared-memory object, one canonical closure manifest, its immutable artifact
objects, and four eventfds over a bootstrap socket. The sandbox verifies the
manifest and every artifact digest before reporting `READY`. Split
virtqueues carry request completions and lifecycle events without polling. The
tests cover direct and indirect descriptors, `EVENT_IDX`, wrap-around, region
rights, malformed input, quiesce, deterministic process faults, pidfd-confirmed
exit, revocation, reset, reap, and regeneration. Device-specific acceptance tests
remain in the host OS repository.

The GPL sandbox fixture follows the manifest dependency order, loads
`fixture_core.so` and relocates an ET_REL `fixture_module.ko` from transferred
objects, resolves the declared import from the explicit provider namespace,
and invokes init, quiesce, and cleanup in graph order.
The module exercises allocation, locking, wait/wake, two native threads,
per-CPU state, an RCU grace period, and monotonic time before returning its
result through the request virtqueue.
