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

Start establishes fresh resources and a new generation before the sandbox
becomes running. Stop and restart order revocation, device reset when present,
process termination, resource release, and the next start. Results from another
generation are rejected.

## Interface

- A launch description contains opaque manifest and profile digests, resource
  limits, capabilities, and channel descriptions.
- Events report action completion, sandbox handshake, process exit, protocol
  fault, and host stop or restart requests.
- Actions identify their type, generation, token, and bounded arguments. Native
  handles are resolved by the host adapter.
- Status uses `kb2_status_t`; host errors are translated at the adapter.

Public controller types are opaque. The caller owns each object's lifetime and
serializes access to it. The library retains no caller pointers, uses only an
explicit allocator, and represents host operations as output actions.

## Development contract

The controller API, host contract, transport, and role protocols are one `dev`
interface. They have no ABI version number or compatibility guarantee. A build
uses matching source revisions and schema digests across all components.

The first ABI number is assigned only by an explicit freeze after the
conformance fixture passes.
