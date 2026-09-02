# Controller library

## Contract

`libkobox2` is a portable C11 library linked by a host service. One controller
object manages one sandbox instance. It validates launch descriptions, advances
the lifecycle, and emits ordered host actions.

The library performs no I/O, creates no threads, and contains no Linux or host
OS semantics. The host executes actions and returns their results.

## State machine

Each call supplies one command or event. A successful call commits one
deterministic transition and may produce actions. Invalid input leaves the
controller unchanged.

At most one action is outstanding. The host reports its completion before
delivering another command or event; failed execution is an explicit result.

Start establishes fresh resources and a new generation before the sandbox
becomes running. Normal stop and restart quiesce the closure and confirm process
exit before revocation, reset, reap, resource release, and the next start. Fault
cleanup terminates the process before the same resource cleanup. Results from
another generation are rejected.

## Interface

- A launch description contains an immutable validated closure, profile and
  channel digests, and resource limits. Reset flags are derived from closure
  resource policy.
- Events report action completion, sandbox handshake, process exit, protocol
  fault, and host stop or restart requests.
- Actions identify their type, generation, token, and bounded arguments. Native
  handles are resolved by the host adapter.
- Status uses `kb2_status_t`; host errors are translated at the adapter.

Public controller types are opaque. The caller owns each object's lifetime and
serializes access to it. The library copies configuration data; its only retained
caller references are the explicit allocator callbacks and context. Host
operations are output actions.

## Development contract

The controller API, host contract, transport, and role protocols are one `dev`
interface. They have no ABI version number or compatibility guarantee. A build
uses matching source revisions and schema digests across all components.

An ABI number is assigned only by an explicit freeze after the conformance
fixture passes.

The action contract is specified in [host-actions.md](./host-actions.md).
The closure contract is specified in [module-closure.md](./module-closure.md).
