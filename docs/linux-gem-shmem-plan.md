# Upstream GEM shmem buffer lifetime

This gate covers buffer lifetime on the Linux host. It follows the MM/host-mapping
gate and does not certify GPU execution or change the agreed implementation order.

## Required result

Use upstream GEM shmem for object/handle ownership, backing pages, pin/unpin,
vmap/vunmap and mmap. Multiple references and mappings must see the same data.
Closing handles must leave existing mappings valid. After all mapping, pin and
vmap users finish and the last object reference is released, reclaim the real
object and pages. Reference counters or destructor calls alone are insufficient:
observe final allocator reuse without retaining a reference to the old target.

The fixed boot-rooted Linux core remains a shared object. Real upstream DRM/GEM
modules run above it; kobox2 supplies machine boundaries, not a GEM substitute.
Use the real two-process MM/fault transport for userspace mappings.

## Prerequisites

- The pinned configuration builds DRM and GEM shmem as modules. The full boot
  artifact contains native built-ins, not these module implementations.
- `drm_dev_init()` requires completed native `drm_core_init()`. Device/file,
  handle tables and mmap-offset management are part of this gate's setup.
- `drm_gem_mmap()` checks native mmap-offset permission and acquires a separate
  VMA object reference. Handle close revokes future offset access; it does not
  remove that existing VMA reference.
- Native shmem GEM mmap uses `VM_PFNMAP` and `drm_gem_shmem_fault()` with
  `vmf_insert_pfn()`, unlike ordinary file-backed shmem faults. Test this actual
  path through client accesses; do not substitute a plain shmem file mapping.
- Use native module loading, initialization and reference management. Do not
  initialize selected module fields by hand or replace module refcount APIs.

## Machine boundary

- Supply host executable-memory ranges to native execmem. Preserve upstream
  allocation/lifetime, CPA alias checks and strict-module-RWX.
- Translate actual PTE permissions, including direct-map aliases, into host
  mappings and protection. Publish invalidation and permission restoration
  before memory reuse; do not make the whole module window RWX.
- Resolve imports through native modpost-generated ksymtab metadata, including
  existing architecture-port exports. Preserve canonical object separation and
  upstream boot/initcall layout.
- Keep modules within the relocation range of the core. Reject unsupported
  runtime relocations and address truncation; distinguish nonallocated debug
  offsets from runtime addresses.
- Use native module-aware text lookup and exception tables for registered
  module text. Continue rejecting arbitrary native addresses.

## Implementation order

1. Build the real module closure with the same pinned config and lowest-layer
   architecture headers as the full core. Use upstream module loading and
   initialization; implement only missing hosted executable-memory/addressing
   boundaries. Reject missing imports instead of stubbing.
2. Create the test DRM device and native DRM files through upstream APIs. Use
   real GEM handle and mmap-offset lookup/permission paths. No physical GPU
   is required for a shmem GEM lifetime test.
3. Exercise nested object references, handles, pin/unpin and vmap/vunmap. Check
   page identity and contents, including balanced intermediate releases.
4. Map the object into both real client processes through their distinct Linux
   mms. Verify actual faulting reads/writes and visibility through the kernel
   vmap alias. Close handles while mappings survive, then access/fault again.
5. Release users in different orders, including partial unmap and failed mmap
   setup. Drain native deferred cleanup; prove final object/backing-page
   reclamation, then close DRM files/device and release modules in dependency
   order. Keep the MM and foundation regression gates mandatory.

## Completion gate

- Native DRM/GEM implementation and module initialization, without legacy
  synchronization/runtime providers or upper-subsystem replacements.
- Real shared contents across object references, kernel vmap and two client
  mm mappings; balanced nested pins/vmaps.
- Handle close rejects lookup/new unauthorized mapping but does not invalidate
  an existing mapping, including a fresh fault in that surviving mapping.
- Object/pages remain alive while legitimately referenced; after the final
  release, actual allocation reuse proves reclamation. No UAF, double release,
  leaked reference or surviving callback during teardown/rollback.
- The full foundation CTest suite and repeated GEM lifetime tests pass on the
  same core.
