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
Only task start/switch/release, logical-CPU identity, IRQ/IPI delivery, and clock
access cross the host boundary. The host does not choose runnable tasks.

The two-CPU gate covers local/remote switching, current/per-CPU agreement,
sleeping and running migration, preemption exclusion, IRQ deferral, and native
join after Linux exit. The idle-wait regression also covers a pending IPI whose
notification sequence was already observed before delivery.

This isolated subsystem fixture is not an alternative production core build.
Its fail-closed phase imports must never be loaded as a completed driver runtime.
It does not certify timer progress, RCU grace periods, workqueue execution, full
CPU hotplug, or complete shutdown; the boot-rooted core design above is unchanged.

## Structural gate

The boot-core inventory requires the upstream x86 linker output and verifies
non-empty initcall, per-CPU, scheduler-class, parameter, setup, init, and core
sections. It also requires `start_kernel` to come from `init/main.o` and the
active profile's Linux upper-API override count to be zero.

The separate driver inventory requires its load and cleanup orders to contain
only `.ko` artifacts. This is a link-structure gate, not yet a claim that the
hosted architecture port boots Linux or that the GPU runtime is complete.
