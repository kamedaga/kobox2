# Module closure

## Unit of execution

A closure is the immutable content of one sandbox process and one generation.
It contains one or more root modules, every transitive module and provider,
explicit symbol bindings, resource slots, and lifecycle entries. Changing any
part of the closure creates a new generation.

The manifest digest is SHA-256 over the canonical packaged manifest. Artifact
digests, graph records, symbol bindings, lifecycle entries, resource slots, and
roots are covered by that digest. The controller receives the digest with the
decoded description; the host and sandbox verify it when transferring the
packaged manifest and artifact objects.

All closure interfaces use the identity `dev`. They have no ABI version or
compatibility promise before an explicit freeze.

## Manifest graph

Every artifact record contains:

- a nonzero node ID unique within the closure;
- the artifact kind, shared provider or relocatable module;
- an immutable content digest;
- a namespace name;
- explicit init, quiesce, and cleanup entry symbols; and
- a root marker for entry modules.

Dependency records form a directed acyclic graph from a consumer to its direct
providers. Every non-root node is reachable from a root. A provider shared by
multiple consumers is loaded and initialized once. Independent nodes are
ordered by node ID, making load and lifecycle order reproducible. Shared
providers depend only on other shared providers.

The sealed description stores artifacts by node ID, dependencies by consumer
and provider ID, symbols by owner and name, resources by slot ID, and bindings
by slot and node ID.

The packaged manifest is an immutable bounded object transferred as a resource.
It identifies artifacts by node ID and digest rather than by a host path or
native handle.

## Process transfer

The canonical manifest is a bounded little-endian binary object defined by the
shared MIT schema. Its header contains `dev` identity, the exact schema digest,
total size, and table ranges. Fixed-size tables describe artifacts,
dependencies, exports, imports, resources, and resource bindings. Names occupy
one trailing byte table and are referenced by bounded offset and length.

Records use the sealed ordering defined above. The manifest digest is SHA-256
over the complete encoded object. Artifact records contain the SHA-256 and byte
length of their artifact object.

Bootstrap transfers resources in this order:

1. transport memory;
2. canonical closure manifest;
3. artifact objects in manifest artifact order; and
4. notification endpoints in channel order.

The bootstrap envelope carries generation, manifest digest, manifest size,
artifact count, notification count, and total resource count. The manifest and
artifact objects are immutable before process launch. The sandbox validates the envelope,
manifest schema and digest, complete graph, artifact size and digest, resource
count, and generation before loading code or reporting `READY`.

## Symbol binding

Each exported symbol belongs to its artifact namespace. An import record binds
one consumer symbol to one provider node and exported symbol:

```text
consumer symbol -> provider node -> provider namespace -> exported symbol
```

Node zero denotes the sandbox runtime namespace. Runtime imports are accepted
by the host profile allowlist. Artifact imports name a direct dependency and an
export declared by that dependency. Local ELF symbols remain private. A strong
import has exactly one binding. A weak import may be absent only when its record
is marked optional, in which case its resolved value is zero.

Dynamic dependencies of a shared provider are represented by closure
dependencies or by allowlisted sandbox-runtime imports. Resolution uses the
manifest binding and the selected artifact symbol table; it has no process-wide
interposition step.

## Resources and rights

A resource requirement is a symbolic slot containing:

- a nonzero slot ID unique within the closure;
- a resource type;
- required or optional presence;
- minimum and maximum object counts;
- required and maximum rights;
- exclusive or closure-shared access;
- a reset policy; and
- the node IDs that receive the binding.

The resource type defines the meaning of its rights:

| Resource | Rights |
|---|---|
| memory | `READ`, `WRITE`, `MAP`, `DMA` |
| device | `COMMAND`, `MAP`, `DMA` |
| storage | `READ_BLOCKS`, `WRITE_BLOCKS`, `FLUSH`, `DISCARD` |
| notification | `WAIT`, `SIGNAL` |
| channel | `SEND`, `RECEIVE` |

The host policy grants rights within the declared maximum. A required slot is
valid when its count and required rights are satisfied. An absent optional slot
is represented explicitly. Exclusive slots have one consuming node;
closure-shared slots name every consumer.

The controller owns each resource set. Nodes receive generation-scoped resource
IDs for the lifetime of the closure. Native handles are resolved by the host
adapter. Transport-region access rights and closure-resource rights are separate
domains.

## Lifecycle

Startup has the following order:

1. validate the complete manifest and artifact digests;
2. bind every resource slot;
3. map every artifact;
4. complete every relocation and symbol binding;
5. initialize providers in dependency order;
6. initialize root modules;
7. report `READY`; and
8. accept requests.

If initialization fails, successfully initialized nodes are cleaned up in
reverse initialization order. Each node completes its own partial rollback
before returning an initialization failure.

Normal shutdown has the following order:

1. stop admitting requests;
2. quiesce roots and then providers in reverse dependency order;
3. drain requests and callbacks;
4. clean up nodes in reverse initialization order;
5. unload modules and then shared providers;
6. report `STOPPED` and exit the sandbox process;
7. confirm process exit;
8. revoke resources;
9. reset resources whose policy requires it;
10. reap the process record; and
11. release resources.

Normal init, quiesce, and cleanup entries run once per node. A quiesce deadline
failure becomes a closure fault.

## Linux closure loader

The Linux sandbox closure loader consumes one decoded manifest and the exact
artifact object sequence named by it. It validates the complete graph, declared
exports and imports, resource bindings, artifact sizes, and artifact digests
before mapping code.

The loader computes a deterministic topological order, maps every artifact,
resolves each import through its named provider, initializes each node once,
and exposes only declared exports by node ID and symbol name. Lifecycle exports
use the Linux module convention `int entry(void)`.

Resource bindings are completed through an explicit sandbox callback before
artifact mapping. Sandbox-runtime imports use a separate resolver callback and
node zero. Closure imports resolve only against the export table of their
direct provider.

Quiesce and cleanup use reverse topological order. Initialization failure
cleans up initialized nodes in reverse order and unloads every mapped artifact.
Relocatable modules are unloaded before shared providers.

## Multiple modules

A closure may have multiple root modules. All nodes in that closure share an
address space, trust domain, fault domain, resource-revocation unit, and restart
generation. Shared dependencies have one instance and one lifecycle.

Driver instances requiring independent restart, hardware isolation, or resource
revocation use separate closures. Communication between closures uses declared
channels.

## Fault handling

The entire closure is the fault unit. A loader, provider, module, protocol,
resource, or process fault moves the generation to `FAULTED` and fixes the first
fault as its result.

Fault cleanup has the following order:

1. stop admitting requests;
2. terminate the sandbox process;
3. confirm process exit;
4. revoke resources;
5. reset resources whose policy requires it;
6. reap the process record; and
7. release resources.

In-process lifecycle callbacks are not completion conditions on the fault path.
Pending requests complete with generation failure. Notifications and
completions carrying an earlier generation are rejected.

## Controller API

`kb2_closure_builder_t` builds and validates a description. Sealing produces an
immutable `kb2_closure_t`; the controller copies it during idle configuration.
Callers may destroy their original closure after configuration.

`kb2_controller_start` starts a fresh sandbox generation. Its ordered host
actions carry a read-only closure view, digests, limits, and opaque resource and
sandbox IDs. The host adapter performs native allocation, process creation,
transfer, graceful process shutdown or termination, revocation, reset, reap,
and release.
