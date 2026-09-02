# Resource grants

## Model

The closure manifest declares immutable resource requirements. A resource grant
set records the objects allocated for one closure generation. A native handle
map connects those objects to the process-transfer mechanism.

The manifest owns slot definitions and node bindings. The grant set owns actual
presence, object IDs, granted rights, and native handle roles. Sandbox resource
identity is the pair of generation and object ID. All interfaces use identity
`dev` until an explicit ABI freeze.

Every resource requirement carries a resource type and an interface schema
digest. The type selects the rights namespace. The interface digest selects the
operation contract and native handle roles. A grant must carry the exact
interface identity declared by its slot.

## Grant set

The grant set is a bounded canonical little-endian binary object. Its header
contains:

- `dev` identity and the exact schema digest;
- total size and table ranges;
- generation and closure manifest digest; and
- slot, object, and native handle binding counts.

Its SHA-256 digest and size are carried by the bootstrap envelope. The encoded
object is immutable before process launch.

Records have this canonical order:

1. slot grants by slot ID;
2. objects by slot ID and object ID; and
3. native handle bindings by object ID and role.

Exactly one slot grant exists for every manifest resource slot. A slot grant
contains the slot ID, resource type, interface digest, `PRESENT` or `ABSENT`
state, and its object range.

An `ABSENT` grant represents an optional slot with zero objects and zero native
handle bindings. A `PRESENT` grant has an object count within the manifest
minimum and maximum. Required slots are `PRESENT`.

## Objects and rights

Each granted object contains a nonzero 64-bit object ID and 64-bit granted
rights. Object IDs are unique within a grant set and remain valid for that
generation. A new generation receives new object IDs.

Every present object satisfies:

```text
required_rights subset-of granted_rights subset-of maximum_rights
```

Objects in one slot may carry different granted rights while each satisfies the
slot requirement. Shared consumers observe the same object IDs and underlying
objects.

The sandbox registry checks generation and rights on every resource operation.
Native access restrictions provide the corresponding host enforcement.

## Node visibility

Manifest resource bindings are the authority for node visibility. The loader
combines those bindings with the grant set to construct one resource view per
node.

- A node sees each explicitly bound slot.
- An exclusive slot has one node view.
- A closure-shared slot exposes the same objects to every bound node.
- Providers receive resources through explicit bindings.
- An absent optional slot is visible with state `ABSENT`.

Dependency edges carry symbol availability. Resource visibility remains
defined by resource bindings. Nodes in one closure share an address space and
trust domain; separate closures provide security and revocation isolation.

## Module context

Every artifact lifecycle entry has this contract:

```c
int entry(const struct kobox_module_context *context);
```

The same immutable context is passed to init, quiesce, and cleanup. It remains
valid from init entry through cleanup return and contains:

- `dev` identity and structure size;
- generation and node ID;
- an opaque node resource view; and
- runtime and core operations.

Modules enumerate a slot and acquire opaque resource handles through the core
operations. Handles retain their generation and granted rights. An absent slot,
an invisible slot, and a stale handle produce distinct results.

## Native handle map

A native handle binding contains an object ID, an interface-defined role, and a
transfer handle index. An object may have any handle count defined by its
interface schema. Roles are unique within an object, and transfer indices form
one complete canonical sequence.

The handle index is independent of a native handle value. Linux maps it to an
index in the received `SCM_RIGHTS` descriptor array. Other hosts map it to their
native handle transfer table.

The Linux transfer order is:

1. transport memory;
2. canonical closure manifest;
3. canonical resource grant set;
4. artifact objects in manifest order;
5. resource descriptors in native handle binding order; and
6. transport notification endpoints in channel order.

Every received descriptor has `FD_CLOEXEC`. Manifest, grant set, and artifact
objects carry the complete write, grow, shrink, and seal seals. Resource
descriptors are validated according to their interface schema and granted
rights.

## Binding transaction

The sandbox completes startup in this order:

1. verify the bootstrap, manifest, grant set, and artifact digests;
2. validate all graph, slot, object, rights, visibility, and handle records;
3. import every native object into a temporary registry;
4. construct every node resource view;
5. commit the registry atomically;
6. map and relocate artifacts;
7. initialize artifacts in dependency order; and
8. report `READY`.

Import failure releases every temporary object in reverse order. Initialization
failure cleans initialized nodes, unloads artifacts, destroys resource views,
and releases the registry.

## Generation lifecycle

Normal shutdown quiesces modules before resource views are destroyed. Process
exit completes Linux descriptor revocation. The host then revokes, resets, and
releases the resource set.

Restart creates a new generation, grant set digest, object IDs, native handles,
module contexts, and resource views. Resource lookup rejects an object from an
earlier generation as stale.
