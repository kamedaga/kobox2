# Connecting Linux MM / VMA to mappings in separate host processes

This plan connects memory managed by the kobox2 Linux driver sandbox to
separate client processes running on the Linux host. Upstream Linux in the
core `.so` owns VMAs, fault decisions and page lifetime; the lowest-layer port
applies mappings to the clients. The goal is shared access with correct
protection, invalidation and reclamation across process boundaries.

Status: the required MM/host-mapping gate passed on the Linux host, including
publication/exit races and final reference reclamation. This certifies the
memory-port gate below, not DRM/PRIME or GPU execution. A transport unit test or
an individual integration case alone does not certify the whole gate.

The machine contract is described in
[the MM port documentation](../linux-sandbox/kobox/mm/README.md).
Progress and verification results are recorded in this plan, not README files.

## Required result

Linux owns VMAs, page selection, fault decisions and page lifetime. The lowest
machine port owns real host mappings, protection and synchronous invalidation.
One external host address-space instance corresponds to one real Linux `mm` in
the single boot-rooted core `.so`. Its faults are routed to a Linux task using
that `mm`; host PID reuse must not retarget a live or delayed operation.

The gate uses **two real processes**, actual faulting instructions, aliases of
the same backing, per-mapping read-only protection and partial unmap. Truncate
must invalidate all affected mappings before their pages can be reused.
Racing faults, invalidation and process exit must not resurrect mappings or
leak references. VMA creation alone, direct fault-function calls alone and
same-process aliases alone cannot pass. DRM / PRIME / FD-sharing integration
is outside this memory-port plan; the two-process memory gate is required here.

## Gaps identified before implementation

- `linux-sandbox/kobox/boot/exception_port.c` accepts core-kernel instruction
  addresses and performs exception-table fixups, not user-mm fault resolution.
- `linux-sandbox/kobox/task/port.c` currently assumes kernel-only task contexts
  in its lazy-TLB/deactivation boundary. Native user-mm switching still needs
  a hosted architecture implementation, not a replacement scheduler.
- `linux-sandbox/kobox/memory/early_boot.c` mirrors vmalloc kernel PTEs. It does
  not yet implement the user-mm TLB invalidation completion boundary.
- `linux-sandbox/kobox/memory/host.h` has local map/reset operations, not an
  independently identified remote address space, fault delivery or death-aware
  completion. Passing a raw RAM descriptor to an unrestricted client would not
  be a suitable process boundary.

## Implementation order

1. Add POSIX address-space transport and its isolated tests. A host-side tracer
   controls separate process execution, captures actual faults and applies map,
   protection and reset operations. Clients do not load another Linux core.
   Mapping operations are independent of guest scheduling and must acknowledge
   actual completion; a missing response is not success. Process death is
   verified/reaped before it can stand in for mapping revocation.
2. Connect native architecture mm activation/deactivation and user TLB flushes.
   Reuse upstream `mm_alloc`, mm references, VMA operations and page tables.
   Host execution identity and Linux `mm` identity remain distinct and explicit.
3. Deliver real access faults to upstream x86 `do_user_addr_fault`, preserving
   the access type and the corresponding Linux task/mm. Linux signal/error
   decisions are transported back; the port does not implement a parallel VMA,
   permissions, shmem-fault or page-allocation policy.
4. Publish host translations from locked, authoritative Linux PTEs. Connect
   Linux's TLB flush boundary to completed host invalidation before upstream
   deferred page release. Serialize publication with invalidation, reject stale
   completions, and drain address-space activity before releasing its mm.
5. Run the entire two-process gate and fault/invalidation/exit races, then rerun
   boot, SMP/time, memory, VFS, shmem, IRQ/softirq, timed waits, RCU, workqueue
   and cleanup gates on the same core.

The transport adds a lowest-layer process-local machine interface: a local
window callback cannot express remote execution identity or revocation
completion. Any concrete interface change must be documented with its caller,
ownership, threading and error rules; no ABI version or compatibility shim is
introduced. Linux upper implementations stay in the fixed upstream core.

The POSIX ptrace owner is a dedicated native service thread: Linux workers
cannot issue ptrace calls owned by another pthread. Fault/run completions use
a distinct machine notification entering the existing logical-CPU IRQ domain,
then wake upstream Linux waits. This adds a process-local notification kind
and async event operations, not a wire ABI or scheduler. Synchronous map/reset
commands are serviced without entering Linux, including while its page-table
locks are held. A running client must be stopped and invalidated by that host
service, not by waiting for a guest worker to run. Tests must cover IRQ-masked
completion delivery and revocation while the client is executing.

## Evidence required for completion

- Distinct live host PIDs and distinct Linux mm identities; correct routing
  when the same virtual address is used by both processes.
- Actual accesses initially fault, then resume using the Linux-selected PFN.
  Two processes observe shared writes; unrelated mappings remain isolated.
- Read-only writes fail through the Linux fault decision. Protection changes
  and partial unmap affect the intended mapping, not its independent peer.
- Truncate removes every affected translation. A held invalidation completion
  prevents page reuse; releasing it allows actual reclamation. The detached
  page is not accessed after reuse, even by a delayed fault completion.
- Forced overlap of fault publication, invalidation and process death, not
  merely sequential simulated callbacks. Missing/dead clients, interrupted
  operations and partial setup failures leave no live mapping or leaked refs.
- All prior mandatory gates still pass; no new upper-subsystem replacement or
  silent-success stub is accepted as an implementation of these requirements.

## Recorded verification checkpoint

This checkpoint does not certify completion of the MM/host-mapping integration.
All 48 CTests passed;
each two-process integration probe passed 50 consecutive runs with zero Linux
warnings. The host transport passed 100 runs of its 200-round interrupt/fault
race (20,000 rounds), and the async service passed 200 runs including
stopped-client death. Both host tests also passed ASan/UBSan with the ordinary,
uninstrumented tracee. The tested full core retained 599 native built-in
objects and 129 initcall targets; SHA-256:
`d0297c86e80ffe6c2e1dd1fbddecea2197c998c7f7c57ddb2d5bd6e4204cc480`.

The integration probes observe Linux's queued signal decisions, not execution
of Linux userspace signal handlers. At this historical checkpoint, their
`intermediate-not-full-gate` label withheld certification of the then-unverified
lifetime guarantees.

## Invalidation acknowledgement and PFN reuse verification

`--vm-probe-reuse` starts with a shared page accessed by both real clients.
During upstream `vfs_truncate`, a test wrapper forwards the real host resets
and holds the second acknowledgement. The other logical CPU, with IRQs masked
to avoid waiting on the flush's registry lock, allocates 512 pages. It checks
both that none is the old PFN and that the permanent PFN metadata still carries
an upstream reference: a freed page on the blocked CPU's local free list cannot
hide behind the allocation sample. No test-owned folio reference is retained.

After acknowledgement, the test checks the empty page cache, reacquires the
same PFN from upstream buddy, and overwrites it. Real accesses through both
old process aliases must fault and receive Linux's SIGBUS decision without
reading or modifying the reclaimed page. This is not a fault-publication or
process-death race test; those requirements remain below.

The process-local test descriptor/report adds a case selector and observation
counters so the launcher can run and report this scenario. The production host
operations and wire interface are unchanged.

Verification: all 49 CTests passed on the rebuilt core. The reuse case also
passed 100 consecutive runs, each reporting 512 held-acknowledgement
allocations, one reclaimed PFN, two rejected old aliases and zero Linux
warnings. The core retains 599 native built-in objects and 129 initcall
targets; SHA-256:
`9b8d9285b115e2bb4879b4631ec75ba33b4899df48e1311c75864dba765387b8`.
Workspace logs: `.artifacts/kobox2-mm-vma-reuse-all-tests.log` and
`.artifacts/kobox2-mm-vma-reuse-repeat.log`.

## Publication, IRQ and process-exit verification

The IRQ case reproduced a circular wait in the original publication boundary:
publication held the translation lock with Linux IRQs enabled, flush held the
registry lock while waiting for the translation lock, and the publisher's VM
IRQ waited on the registry lock. The pre-fix core stopped after the explicit
`MM race: publication IRQ queued` checkpoint and exceeded the eight-second
external deadline (exit 124). `publish_pte` now uses `raw_spin_lock_irqsave`
throughout publication. No upstream MM, scheduler or IRQ implementation changes.

The two-process probes exercise these separate cases:

- `--vm-probe-irq`: hold real fault publication on CPU 1, enter the architecture
  broadcast flush on CPU 0, queue a VM IRQ while its registry lock is held,
  then finish publication. A waitqueue observer verifies delivery in CPU 1's
  hardirq context after publication. This directly tests the architecture
  broadcast boundary, not upstream reclaim policy.
- `--vm-probe-truncate`: start native `vfs_truncate` while publication owns its
  PTE lock. Observe native `i_size` becoming zero before releasing publication;
  invalidate both mappings and reuse/overwrite the PFN before releasing the
  delayed fault resume. Both real clients must then fault with Linux SIGBUS.
- `--vm-probe-exit-publish`: the other CPU closes/reaps the real publisher
  process before publication's host map, then truncates. Publication must fail
  with `-ESRCH` without incrementing its success counter. After PFN reuse,
  delayed map/resume attempts on the dead binding must also fail.
- `--vm-probe-exit`: allow publication, truncate and reuse the PFN, then
  close/reap the client while its fault resume is still held. Both a stale
  sequence and the pending resume must fail, while the surviving peer faults
  instead of accessing the reused page.

The exit cases deliberately initiate host close/reap. Unsolicited EXIT delivery
and complete reference accounting remain part of the lifetime checks below.
Only the process-local fixture descriptor/report gains the case selector,
IRQ-injection callback and counters; production host operations and wire
messages are unchanged.

Verification: all 53 CTests passed. Each of the four race cases passed 100
consecutive runs with zero Linux warnings and one forced publication overlap
per run. The IRQ case recorded delivery after publication; all three truncate/
exit cases reacquired the old PFN before releasing the pending fault. The full
core still contains 599 native built-in objects and 129 initcall targets;
SHA-256: `fe046b9cc2503f0bc955a78c68c9483bc447d574776a10e709e3f6b5df49edf4`.
Workspace logs: `.artifacts/kobox2-mm-vma-race-all-tests.log`,
`.artifacts/kobox2-mm-vma-race-repeat.log`, and the negative reproduction
`.artifacts/kobox2-mm-vma-race-before-irq.log`.

## Exit delivery and final reference reclamation

`--vm-probe-lifetime` tears down two live shared mappings without first
truncating the file. `--vm-probe-death` first parks a real Linux task on its
binding's event waitqueue, checks that it is off CPU, then sends SIGKILL from
the host without calling close/reap. The service observes/reaps the process and
delivers exactly one EXIT through the VM IRQ and upstream wait path. The test
checks the original execution sequence and current/mm identity; another event
dequeue returns `-ESRCH`, and the surviving peer can still read its mapping.

`--vm-probe-rollback` establishes and accesses the first process mapping, then
creates the second mm/service task. Native `vm_mmap` rejects
`MAP_SHARED_VALIDATE | MAP_FIXED | MAP_SYNC` on shmem with `-EOPNOTSUPP`, leaving
the second mm without a VMA. Both sides then follow ordinary teardown. This
is a real unsupported mapping request, not an injected allocation success or
a replacement Linux failure path. Missing-client startup is independently
covered by the host service test.

All ten MM cases, including the publication/exit races, use the same final
reclamation audit in `boot/vm_lifetime.c`:

1. Record native cache identities, numeric object addresses and remaining file
   PFNs while the owners are live. Hold explicit observer references to each
   mm/task and the inode, but no folio reference; allocate scratch before
   retirement. Check that the PFN snapshot covers the quiescent page cache.
2. Join the mm-borrowing tasks and close/reap host clients before removing the
   bindings and running native `mmput`. `kthread_use_mm` holds `mm_count`, not
   `mm_users`: the latter alone cannot certify borrower completion.
3. Run a real helper kthread on each CPU to switch to `init_mm`, releasing
   legitimate native lazy-mm references. Drain task work, delayed fput and RCU.
   Each retired mm must have zero users, only the observer's count, an empty
   maple tree, zero RSS and zero page-table bytes. Each stopped task must be
   dead/off-CPU, with no mm/active_mm and only the observer reference.
4. Drop the observers and wait for actual RCU callbacks. Native task release
   reaches `arch_release_task_struct`, which joins/destroys its pthread. Obtain
   both old mm slots and both old task slots from their real slab caches.
5. After VMA fputs drain, require only the caller's file reference. Drop it,
   drain fput, require only the inode observer, then drop that too. Drain LRU
   work and RCU, reacquire remaining PFNs from buddy and overwrite them, and
   reacquire the file/inode cache slots. No retired object is dereferenced.

Raw slab allocations here are a storage-reuse oracle, not initialized Linux
objects or subsystem substitutes. They are returned to the same upstream
cache, never passed to Linux task/MM/VFS APIs. No refcount is forcibly cleared,
and no allocator policy is changed to make the test pass. The normal, death
and rollback cases must each reclaim their remaining file PFN; truncate cases
already prove PFN reclamation before resuming the old fault.

The process-local test descriptor/report adds a lifetime selector, a narrowly
validated host termination callback and observation counters. Production host
operations and the wire interface are unchanged. Closed host handles remain
owned tombstones until service destruction; the host service test checks that
destruction and child reaping. This memory gate does not add a Linux runtime
shutdown policy.

## Completion evidence mapping

| Required property | Mandatory cases |
| --- | --- |
| Actual two-process faults, mm identity, sharing, RO, partial unmap | `probe`, `probe_ro` |
| No page reuse before invalidation acknowledgement; reject old aliases after reuse | `probe_reuse` |
| Publication versus IRQ, truncate and process exit; reject stale map/resume | `probe_irq`, `probe_truncate`, `probe_exit_publish`, `probe_exit` |
| Unsolicited EXIT through a sleeping Linux task, surviving peer, partial setup rollback | `probe_death`, `probe_rollback` |
| Joined borrowers, drained references, actual allocator reclamation | All ten cases, including `probe_lifetime` |
| Native host interruption/reaping/failed startup and prior Linux foundations | Transport/service tests and full CTest suite |

Select the complete memory gate with `ctest --test-dir BUILD -L linux-mm-vma`.
It requires every case and the full-core load fixture; no single case alone
certifies completion. Verification records below identify the tested artifact.

## Final verification

All 56 CTests passed on the final core. The normal-lifetime, unsolicited-death
and partial-setup-rollback cases each passed 100 consecutive runs. The other
seven MM cases, including all publication/exit races, each passed 25 consecutive
runs after the common reclamation audit was added. All 475 repeated runs
reported zero Linux warnings, two drained/reacquired mms, two drained/reacquired
tasks, and one reacquired file/inode. Each of the three lifetime cases also
reacquired its remaining file PFN; all expected EXIT/rollback counters matched.

The final full core retains 599 native built-in objects and 129 initcall
targets. SHA-256:
`27422d5e7d17a4af107278206ee3580cf3f8a0d0bccebb72e86b7de4844b18ce`.
Workspace logs:

- `.artifacts/kobox2-mm-vma-final-build.log`
- `.artifacts/kobox2-mm-vma-final-all-tests.log`
- `.artifacts/kobox2-mm-vma-final-lifetime-repeat.log`
- `.artifacts/kobox2-mm-vma-final-regression-repeat.log`

The compiler reported no warnings. `checkpatch` reported no errors for the new
lifetime helper and one flow-control assertion-macro style warning. No new
production subsystem replacement, silent-success stub or wire change was
introduced for the lifetime verification. The completion matrix above has no
remaining unverified item within this memory-port gate.

## Execution rules for subsequent work

Before implementation, check the planned prerequisites against the current
code. If something is missing, record the concrete missing operation, evidence,
the step that will implement it and its completion test. If discovered later,
update that record before changing direction. Do not silently weaken a gate,
move work to an unnamed future gate or report only “not connected.” A necessary
order change must state old/new order, its dependency and affected gates before
execution. Intermediate evidence does not redefine the accepted final result.
