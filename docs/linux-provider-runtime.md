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
Linux to the process host. On Linux this will use native process/thread/wait
facilities; the same boundary can later target PachaOS without changing Linux
subsystem semantics.

## Structural gate

The boot-core inventory requires the upstream x86 linker output and verifies
non-empty initcall, per-CPU, scheduler-class, parameter, setup, init, and core
sections. It also requires `start_kernel` to come from `init/main.o` and the
active profile's Linux upper-API override count to be zero.

The separate driver inventory requires its load and cleanup orders to contain
only `.ko` artifacts. This is a link-structure gate, not yet a claim that the
hosted architecture port boots Linux or that the GPU runtime is complete.
