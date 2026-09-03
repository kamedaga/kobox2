# Memory arena interface

## Grant

The interface identity is `dev`; its schema digest is the RFC 8785 canonical
SHA-256 of `protocol/schema/memory_arena.json`.

One present memory object is bound to the core provider with `READ`, `WRITE`,
and `MAP` rights. It carries one native handle with role `memory`.

## Mapping

The object exposes one shared read-write mapped range for the resource-binding
lifetime. Its address, length, and page size are 4096-byte aligned. The minimum
length is 8192 bytes.

On Linux, the handle is a `FD_CLOEXEC` memfd with fixed size and exactly
`F_SEAL_SEAL`, `F_SEAL_SHRINK`, and `F_SEAL_GROW`. Import maps the complete
object with `MAP_SHARED` and marks the mapping `MADV_DONTDUMP`.

## Operations

The operation table begins with the common size and `dev` identity fields.
`mapped_range` returns the mapped address and length. The object and operation
table remain valid until registry release.

## Core lifecycle

Core init binds the exact schema digest, initializes the arena in the mapped
range, and then initializes its remaining provider steps. Core cleanup requires
all arena pages to be returned before the mapping is released.
