# Tests

Controller tests cover configuration rejection, lifecycle transitions,
generation rollover, host failure, and crash/restart. Protocol tests cover
round trips, bounds, schema mismatch, reserved fields, region rights, and
message envelopes.

Closure tests cover DAG reachability, cycles, explicit symbol binding,
resource rights, sharing, sealing, and immutable inspection.

On Linux, the test host starts a separate sandbox process and transfers one
shared-memory object, one canonical closure manifest, one canonical resource
grant set, its immutable artifact objects, one resource handle, and four
eventfds over a bootstrap socket. The sandbox verifies both schemas, both
digests, the manifest/grant binding, every artifact digest, and the resource
handle mapping before reporting `READY`. Split
virtqueues carry request completions and lifecycle events without polling. The
tests cover direct and indirect descriptors, `EVENT_IDX`, wrap-around, region
rights, malformed input, quiesce, deterministic process faults, pidfd-confirmed
exit, revocation, reset, reap, and regeneration. Device-specific acceptance tests
remain in the host OS repository.

The GPL sandbox conformance target consists of `fixture_core.so`,
`fixture_provider.ko`, and `fixture_consumer.ko`. Tests cover dependency and
symbol resolution, resource visibility and rights, native threads, per-CPU
state, RCU, time, reverse lifecycle order, init rollback, the exact export set,
stale generations, and fault/restart.
