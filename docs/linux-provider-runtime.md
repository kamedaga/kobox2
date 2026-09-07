# Linux sandbox runtime boundary

## Build and process unit

One sandbox process owns exactly one Linux boot:

```text
sandbox process
  -> upstream vmlinux
       -> init/main.o:start_kernel
       -> upstream initcalls and subsystems
       -> selected .ko modules
```

The Linux core is a fixed boot-rooted artifact. A GPU closure selects driver
modules only; it does not choose fragments of the Linux core.

Shutdown quiesces loaded modules and then exits the process. The core is not
unwound and the process cannot perform a second Linux boot.

## Ownership boundary

Linux retains ownership of task state, scheduler classes, per-CPU state,
waitqueues, mutexes, completions, kthreads, workqueues, timers, softirqs, RCU,
page metadata, and driver subsystems. Kobox must not replace those upper APIs.

The hosted port is limited to the lowest machine boundary required to connect
Linux to the process host. The Linux PoC uses native process/thread/wait
facilities; the same boundary can target PachaOS without changing Linux
subsystem semantics.

## POSIX foundation gate

The Linux PoC host surface is restricted to pthreads, a counting permit,
`CLOCK_MONOTONIC`, a one-shot timer, memory mapping, and POSIX asynchronous
notification. An import gate rejects `futex`, `eventfd`, `timerfd`, and any
undeclared host dependency.

Two independent logical-CPU domains are exercised before connecting Linux.
The gate requires same-CPU entry serialization, overlapping execution on
different CPUs, tick and IRQ delivery to CPU-bound threads, IRQ retention while
disabled followed by delivery on enable, and a retained permit posted before
park.

[LKL's thread and semaphore hooks](https://github.com/lkl/linux/blob/master/arch/lkl/kernel/threads.c)
are a useful boundary reference, but its
[architecture selects `!SMP`](https://github.com/lkl/linux/blob/master/arch/lkl/Kconfig).
UML also exposes only CPU ID zero in this pinned tree. Neither is accepted as
evidence for this SMP gate.

## Linux task/SMP gate

The [task/SMP fixture](../linux-sandbox/kobox/task/README.md) connects real Linux
`schedule()`, wakeup, affinity, CPU stopper migration, and task exit to pthreads.
Only task start/switch/release, logical-CPU identity, IRQ/IPI delivery, clock
access, and per-CPU one-shot device programming cross the host boundary. The
host does not choose runnable tasks or manage Linux timer queues.

The two-CPU gate covers local/remote switching, current/per-CPU agreement,
sleeping and running migration, preemption exclusion, IRQ deferral, and native
join after Linux exit. The idle-wait regression also covers a pending IPI whose
notification sequence was already observed before delivery.

The IRQ/time extension covers upstream RCU idle/IRQ transitions, hardirq
context, per-CPU tick progress, high-resolution hrtimers, timer-wheel callbacks,
delayed IRQ delivery, cancel/rearm with stale notifications, and repeated
tick-driven preemption of two busy kthreads per CPU. Static-key initialization
and jiffies state come from Linux; `BUG` and `RCU_EQS_DEBUG` remain enabled.

This isolated subsystem fixture is not an alternative production core build.
Its fail-closed phase imports must never be loaded as a completed driver runtime.
It does not certify the full timed-wait/remaining-time API matrix, RCU grace
periods or reclamation, workqueue execution, full CPU hotplug, or complete
shutdown; the boot-rooted core design above is unchanged.

## Integrated boot/service gate

The [integrated boot gate](../linux-sandbox/kobox/boot/README.md) now reaches
PID 1's hosted entry after real initcalls, CPUHP and boot-end memory protection.
It verifies ksoftirqd overflow, tasklet/irq_work, basic kworker and RCU callback
progress, then reruns the two-CPU IRQ/SMP/time checks in that same boot.
This is the stage-3 gate, not full timed-wait, RCU, workqueue or GPU certification.

## Upstream timed-wait gate

The [stage-4 gate](../linux-sandbox/kobox/boot/wait-gate.md) runs after that same
boot, exercising upstream jiffy/high-resolution waits, waitqueue, completion,
and sleep APIs on both CPUs. It checks expiry, early and pre-schedule wakeups,
real interrupting/non-interrupting signals, API-specific return values and
remaining budgets after repeated wakes. Unrelated hard clockevent interrupts
must leave the tested task asleep. No upper wait implementation or host ABI is
added. RCU, workqueue and cleanup semantics have separate gates.

## Upstream Tree RCU / SRCU gate

The [stage-5 gate](../linux-sandbox/kobox/boot/rcu-gate.md) checks preemptible
Tree RCU and dynamic/static SRCU across both CPUs: readers prevent reclamation
through nesting, preemption, migration and idle, then normal/expedited GPs,
callbacks, barriers and actual SLUB frees complete after unlock. Normal RCU
readers never voluntarily sleep; sleeping-reader coverage belongs to SRCU.
An active-callback barrier recheck exposed callback-wide IRQ suppression in
the POSIX dispatcher. Only the machine IRQ entry/return contract was corrected;
upper RCU/SRCU code was not replaced. Workqueue behavior and integrated cleanup
races have separate gates.

## Upstream workqueue gate

The [stage-6 gate](../linux-sandbox/kobox/boot/workqueue-gate.md) tests the stated
normal/highpri/BH/unbound/ordered paths, delayed work, cancel/flush, self-requeue,
ordered FIFO and live unbound affinity changes. Actual Linux RAM exhaustion must
cause upstream mayday/rescuer execution to release held pages and restore progress
on both CPUs. There is no new host operation or upper implementation. Pinned
upstream does not recognize BH requeue as chained work during drain; those two
BH behaviors are verified separately and their combination is not certified.
Combined IRQ/timer/work/RCU lifetimes have the stage-7 gate below.

## Integrated cleanup gate

The [stage-7 gate](../linux-sandbox/kobox/boot/cleanup-gate.md) closes admission
atomically with RCU unpublication, then follows the fixture's actual producer
dependencies through IRQ/thread synchronization, timer/work shutdown, a public
reader GP, callback barrier and final-work drain before real `vfree()`.
The source continues attempted interrupts after the device's host alias is
revoked. Fifty-two two-CPU cases require twenty observed synchronization waits,
rejected late submissions, no surviving callbacks and exactly one free per
device. Reader GP, callback completion and callback-produced work completion are
separate assertions. No host operation, upper replacement or generic teardown
runtime is added. Actual PCI/DMA/GPU removal and process revoke remain separate
device/integration gates, not claims derived from this synthetic IRQ fixture.

## VFS lifetime gate

The [chapter-2 VFS gate](../linux-sandbox/kobox/boot/vfs-gate.md) uses the same
real-shmem boot core for mount, named-file read/write, Linux FD close/unlink,
linked-file reopen and independent file/inode/folio lifetime checks. Task work,
kthread delayed fput, inode RCU callbacks and superblock callback-produced work
are distinguished, with actual allocator reuse required after reclamation.
No runtime port, host operation, upper implementation or service initialization
fixture is added. User-copy, shared mappings and external DRM clients remain
separate gates.

## Shmem / page-cache gate

The [shmem / page-cache gate](../linux-sandbox/kobox/boot/shmem-gate.md) adds
shared backing and pinned kernel aliases, sparse and partial-page zeroing,
truncate/hole-punch races, allocation rollback and exact block/inode/commit
recovery. Four cases cover both CPUs and reserved/incremental accounting.
Physical PFN reuse is required after the final pin drops. It adds tests, not
an upper implementation or machine port; user VMAs and pressure remain separate.

## Structural gate

Chapter 2 starts with the [boot-integrated memory gate](../linux-sandbox/kobox/boot/memory-gate.md).
The required `boot/config` selects real shmem/tmpfs; page, SLUB, dynamic/static
per-CPU and sparse/shared vmap checks run after upstream boot on every launcher
invocation. Shmem's actual file/folio path and eviction are checked without
initializing services in fixtures. The `--all` gate reruns all chapter-1 paths
in the same boot. The mapping port now honors sparse spans and post-clear/lazy
TLB invalidation, leaving Linux allocators and VFS/shmem policy unchanged.

The boot-core inventory requires the upstream x86 linker output and verifies
non-empty initcall, per-CPU, scheduler-class, parameter, setup, init, and core
sections. It also requires `start_kernel` to come from `init/main.o` and the
active profile's Linux upper-API override count to be zero.

The separate driver inventory requires its load and cleanup orders to contain
only `.ko` artifacts. This link-structure gate alone does not demonstrate
actual boot or GPU runtime completion; the execution gate is separate.
