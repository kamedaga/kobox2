# Filesystem service

The filesystem interface is an unnumbered `dev` contract. The MIT schema in
`protocol/schema/filesystem.json` is the source of truth; generated layout
constants and bounded codecs are shared with the separate GPL Linux process.
It replaces the old filed ABI, rather than translating or supporting it.

## OPEN metadata

`OPENAT2.mask = 0` requests only a handle. A nonzero mask requests a
best-effort statx payload for that opened handle, using bits from
`STATX_BASIC_STATS`; other bits are rejected before opening. The client must
provide `STATX_SIZE` output bytes before an open can create or truncate a file.
On successful open the response contains exactly that many bytes. Attributes
are obtained under the captured credentials only when the captured filesystem
rights include `STAT`. A denied or failed attribute lookup returns an all-zero
statx payload without turning the successful open into a failure. Open failures
return no attributes or handle.

This uses the existing request mask and statx encoding, without new layout,
opcode, or version fields. It removes the extra FSTAT round trip used by LPR to
classify device aliases while leaving handle-only clients free of attribute
work. LPR does not fall back to a separate FSTAT when the payload is malformed.

## Authority and wire format

The native host adapter now also supports owner-registered, dedicated client
queues. On the management endpoint, `CLIENT_PREPARE` reserves a fresh binding
ID and client, `CLIENT_BIND` installs its queue VMO and dedicated doorbell, and
`CLIENT_BOUND` acknowledges installation. IDs are reserved before giving the
queue configuration to a client, so clients may attach in a different order.
The doorbell grants send/receive/wait/poll/inspect only; it is not an owner
endpoint. Its only accepted packet is an available-ring notification matching
the private binding. Filesystem dispatch receives the client from that binding,
not from the mutable request header. Invalid packets/chains retire that client
endpoint without retiring the common mount or other clients.

`CLIENT_RELEASED` is sent only after admission has stopped, accepted jobs have
been collected and released, the mapping is gone and retained native endpoint
inventory has closed. Release can arrive while VFS is executing: the retiring
lane retains its mapping and chains until drain, discards private responses and
never executes unconsumed chains. Dynamic native waits include control and a
private watch-set wake endpoint in every bounded subset and rotate sleeps beyond the native 256-FD
wait limit; this is not a filesystem client-count ceiling. Filed connects
authenticated session reservation/attachment/retirement to these owner APIs.
LPR publishes ordinary FS requests and waits directly on its dedicated sandbox
queue. Filed never maps or consumes that queue, and its client control endpoint
rejects ordinary FS requests instead of relaying them. Native bootstrap and
management clients retain the trusted control path. LPR now uses per-request
pending inventory and does not hold session/queue exclusion across remote
waits. The production adapter connects the Linux worker pool to native rings
through the common async lifecycle. NVMe-root QEMU verifies that a blocking FIFO
open leaves same-process and other-process stat requests able to complete, and
that the counterpart open then releases it. Threaded shared-offset and atomic
vector-append tests also pass.

Requests have a 192-byte header, followed by variable spans. Responses have a
64-byte header and exactly `data_length` bytes. Integers are little-endian;
reserved bytes must be zero. Native structures are private decoded values,
never overlays on shared memory. Paths include a final NUL but no embedded
NUL. Spans cannot overlap or escape their readable descriptor lease. Linux
owns path/name validation; the codec adds no smaller pathname limit.

The host adapter supplies the authenticated client independently of request
bytes. `CLIENT_REGISTER`, `CLIENT_CREDENTIALS`, `CLIENT_UNREGISTER` and
`TRANSFER_DUP` use a separate owner-only channel. Registration carries the
80-byte credential record and variable, aligned supplementary groups. Linux
retains immutable real/effective/saved/filesystem IDs, capability sets and
securebits. Updates and IDs are monotonic; stale generations and attempts to
reuse a retired identity fail. Ordinary requests cannot supply credentials:
their UID/GID fields are chown operands, not the caller's identity.

The owner also supplies protocol-defined filesystem capability rights in the
registration/update request's `capabilities` field. These rights are separate
from the Linux credential capability sets. The service retains rights and
credentials atomically, authorizes the private decoded request, then executes
that same snapshot under the retained credentials. This prevents direct queues
from bypassing the relay's operation-level policy; Linux DAC remains an
additional check. Unknown rights, including bits above bit 31, are rejected.
Ordinary request capability fields never grant rights. Owner and frontend
operations are rejected by data dispatch even for an all-rights client.

A data request may leave client and credential-generation claims zero. Its
authenticated queue binding then selects that client's current authority;
zero never selects the owner. Nonzero claims must match. A credential/rights
update publishes the pair before its ACK. Already admitted requests may finish
with the previous retained pair, but later admissions obtain the new pair.

Each client has an xarray of monotonically minted references to actual Linux
`struct file` objects. Duplication and owner-authorized transfer retain that
object, including its offset and flags; they do not reopen the pathname. A
recipient gets a new reference in its own table. Requests pin the client,
immutable authority and actual file/dirfd operands at admission, so queued
requests also survive source close and credential replacement. The mount root and device capabilities
come from independently trusted owner state, not shared descriptors.

The service runs the serving task's upstream task work at each request return,
including owner controls and errors, after restoring credentials and dropping
request locks. Its PID 1 loop does not return to Linux userspace, so otherwise
deferred final `fput` could indefinitely retain writer authority and unlinked
tmpfs inodes. Completion is published only after this cleanup boundary;
in-flight references held by other tasks remain valid.

## Linux admission and worker ownership

The process-local `fs_service.h` request API separates preparation from execution.
Preparation copies input once and retains the client plus immutable credential
and capability-right pair immediately. A queued request cannot pick up authority
granted by a later owner update. Execution runs exactly once on a real Linux task,
restores request scopes and runs that task's deferred work before completion.
Cancellation of a prepared request does not execute its operation. The existing
synchronous dispatch uses the same implementation without a second input copy.

`fs_worker.h` provides persistent Linux kthreads with one creating task as owner.
Only that owner submits, collects and releases; workers never access ring indices,
shared request bytes or host RPC. Each job owns private input and output leases.
Eligible clients rotate after one dispatch, and a per-client execution allowance
leaves workers for other clients. No VFS operation holds the pool's short lock.

Data admission allocates the request header, immutable input and private output
as one owned lease, rather than three separate allocations. It survives through
collect/release even after execution drops client, credential and file refs.
Borrowed-buffer synchronous dispatch uses a small inline snapshot; this is a
fast-path threshold, not a path or payload limit. Larger requests still snapshot
all bytes privately and remain subject to the caller's existing byte budget.

Every worker, including the management task, owns a separate scratch `fs_struct`.
Path operations reconcile its root and pwd and reset umask, then install it on the
executing task, then restore that task's previous pointer. Credentials and Linux
DAC checks remain request-scoped. Recursive use falls back to a fresh scope;
scratch contexts never cross simultaneously executing tasks. Construction
unwinds partially allocated scopes, and destruction frees their final path refs
only after every worker joins, before service/mount teardown. Completion wakes
the owner, while the completing data worker directly checks the fair runnable
list again. It fills the one running slot it just released without waking an
extra worker to compete for that slot. Submissions wake additional workers;
destruction cannot stop them until admissions are drained.
The armed data-worker wait predicate dequeues an eligible job directly, rather
than locking once to check readiness and again to claim it. A claimed job still
executes if stop races, so its admission lease cannot be stranded.

Retained scratch is exclusive and uninstalled while checking its actual
mount+dentry pairs. An unchanged pair already owns the required refs, so it
does not repeat upstream get/set/put. Changed roots or directory operands still
use the upstream setters; umask and credentials remain request-scoped. This
does not cache client identity or authority and does not skip namei. Fresh and
recursive scopes retain their original initialization. The native-matched
namespace/dirfd/umask/credential workload also exercises retained scopes.

One resident data task per online logical CPU is bound before it starts; any
remaining data tasks are migratable overflow workers. Isolated submissions
prefer the local resident with a Linux synchronous-wakeup placement hint.
Backlog prefers remote residents, then migratable overflow and local fallback.
Only whether the preceding accepted interval overlapped is remembered through
its final completion-lease release. Repeated bursts therefore do not classify
every first request as isolated; one drained isolated request restores the local
hint. Selection never delays wakeup or limits the execution pool.
Only worker selection changes: client rotation, running caps, VFS/mount/cache
sharing and private path scopes remain intact. The persistent owner wait entry
holds its task reference through all worker joins; non-burst completions use a
synchronous hint only when the completing worker is about to wait. Burst
completions directly wake the pinned owner without the extra wait-list lock.
This is not a promise
of better throughput: isolated latency and parallel admission are measured
separately. CPU topology is fixed during the service lifetime.

The owner recycles fully cleared fixed-size data-job and empty-client metadata.
It consumes free objects before allocating, bounding live plus cached objects by
the configured request-count high-water mark without another arbitrary cap.
After response-lease release and final task_work, packed input/output storage is
fully scrubbed and reusable only for an equal byte capacity. Admission copies a
new immutable snapshot and rechecks authentication and pins every operand;
credentials, file refs and client identities are never cached. Idle plus live
payload bytes share the existing total budget. Admission evicts idle buffers
before allocation as needed, without a new request-size cap or per-client charge
for anonymous idle storage. Closing frees idle buffers and metadata immediately;
releases during drain free directly. Private diagnostics distinguish new and
reused buffers; runtime gates verify scrubbing, cross-client reuse, EOF zero,
large immutable snapshots and eviction under two larger simultaneous leases.
If allocation fails while idle storage remains, the owner reclaims it and retries
allocation once before execution; no filesystem operation is replayed. Gates
cover controlled ENOMEM and this pressure-reclaim path, not full-RAM exhaustion.
The independent management request retains its separate ownership and budget.

Native logical-CPU ownership publication always advances its sequence. A futex
wake is issued only for waiters registered under the ownership lock, including
both domain-entry and idle-owner waiters. Registration precedes unlock/sleep;
publication in that interval invalidates the futex expected value. Notification
deferral, exclusive domain ownership and host-call FP preservation are retained.
Native CPU switching finishes domain transfer under the owner lock and removes
the old task's logical-CPU binding before unlocking, then wakes the successor.
The successor-permit wake runs outside that critical section; registered domain
sequence waiters retain their locked publication. The Linux runqueue pins the
successor; the old task cannot reenter Linux until its park returns and cpu_enter
reacquires its domain. A focused test runs the successor immediately at wake,
including a return permit published before the old task parks.
The counted native task permit has exactly one consumer. Its futex wake asks
for at most one waiter; shared ownership/lock broadcasts are unchanged.

IRQ query/save/disable/enable now enclose each complete native transaction in
one FP-preserving boundary: notification save, leaf CPU lookup, IRQ query/change,
then notification restore. No intervening code uses Linux FP state. The original
bank remains saved through nested callback entry or migration on enable/restore;
IRQ state is still queried from the host, never cached. Preempt/per-CPU guards
remain intact. The diagnostic build separately measures IRQ save/restore and
notification save/restore pairs before running filesystem phases.

Every native boundary still saves and restores the full fixed legacy FP/SSE
bank. Host initialization restores an immutable architectural FXSAVE image
with default controls, empty x87 tags and zero vector registers instead of
executing FNINIT each time. This is not lazy FPU ownership or a no-FP promise
from host callbacks. Arbitrary native clobbers, nested callbacks and migration
still restore the caller's bank; upstream Linux task FPU policy is unchanged.

The production storage fragment disables `SLUB_TINY` and unrelated SLUB debug
defaults, retaining the existing size-optimization, SMP/preempt and namespace
configuration. Its profile pins the complete regenerated config. Core and driver
modules must be rebuilt together; an old tiny-allocator module closure must not
be mixed with the regular-SLUB core. Per-CPU slab retention trades memory for
allocation/parallel throughput and is not claimed to reduce every single-call
latency. The owned `storage_slub.config` fragment reproduces the controlled
comparison from an archived baseline without patching upstream allocator code.

The optional storage-only `--fs-internal-bench` build measures synchronous
dispatch and the real worker submit/execute/collect/release path without FS IPC.
Depth 8 reports throughput, not individual latency. Stage profiling overlaps
owner admission; moving lease allocation/free to owner submit/release is not
eliminating that cost. Linux scheduled task runtime is not host physical CPU
time or unhalted PMU cycles. Linux task switch counts exclude native futex calls
and other Linux tasks; metadata diagnostics count new/reused allocations only.
Task-local, reentry-safe diagnostic counters also observe notification saves,
IRQ transactions, hosted clock reads and handoffs on the owner/data tasks.
Snapshots run outside each phase loop. They include interrupt callbacks on
those tasks, but not unrelated idle/control tasks or native futex counts.
Counting adds overhead: an instrumented run is diagnostic evidence, not a
production speedup. Separate clock-read and no-op FP-boundary measurements
help choose a remaining structural target without removing safety guards.
The native storage adapter's optional `--handoff-bench` flag also records phase
totals for publication-to-successor entry, wake syscalls, permit parking and
owner-lock waits. These include idle/control tasks on every logical CPU, unlike
the owner/data Linux counters. Park wait intervals include other tasks' work and
must not be added to operation latency or interpreted as CPU time. Cross-task
elapsed TSC and overlapping stages are not PMU cycle accounting. Phase epochs
exclude waits begun in an earlier phase; logging occurs outside the hot loop.
Native tracing is off in normal builds and changes neither host nor wire ABI.
Diagnostics and controlled allocation-failure gates
are absent from the normal runtime; no wire or host ABI changes are required.

Caller-supplied resource budgets bound request count and input/output bytes per
client and globally. A single client cannot consume the whole admission budget.
Completed-but-unreleased jobs remain charged, including their private output;
fixed job/client metadata is bounded by request counts. Temporary pressure returns
`EAGAIN`; a request exceeding the memory budget returns `ENOMEM`, not a new file
size limit. Closing stops admission; destruction returns `EBUSY` until all jobs
are collected and released, then joins the persistent tasks. Revocation removes
the service identity, rejects new jobs and drains retained, already-admitted
requests. Idle client groups leave the active registry; their cleared allocation
may be reused, without retaining an identity or an unbounded revocation tombstone.

An independent persistent management task has one reserved request and an
explicit `control_bytes` budget, separate from data admission. Prepared owner
exec/device requests retain their immutable authority at admission too. Native
integration orders registry/filetable changes with an admission barrier for
the affected client; unrelated clients must not wait for blocking exec/device VFS.
A finite data pool can still fill if several clients block; bounded admission
does not guarantee unlimited VFS progress. The real Linux gate deliberately
blocks every data worker in the inode rwsem, verifies admission and management
completion still proceed, then verifies queued data resumes after unlock.

The common async lifecycle makes the opening Linux task the sole control/ring
owner. A native helper only waits for readiness using endpoint-number snapshots;
it never consumes control or shared rings. Workers wake the owner directly via
the Linux scheduler, not a per-completion native IPC. Idle uses arm/fence/recheck
before sleep. Faults and QUIESCE stop admission but drain accepted requests before
joining the native helper and Linux workers, then retiring mappings. Legacy
synchronous services continue using the same common startup/shutdown code.
Linux wake-before-sleep and native out-of-order/fault/startup cleanup gates pass.
The production adapter retains a request-owned chain for each admitted job.
Only the owner validates/takes chains, publishes responses and releases leases;
Linux admission snapshots a direct client's bytes without a second native copy.
Temporary admission pressure returns the latest unwritten chain to availability,
without execution or a private backlog outside the worker budget. Client barriers
last through management response publication and lease release. Accepted requests
keep their original authority; later admissions see the updated pair or revocation.

The native readiness helper snapshots FD numbers under a short watch-set lock,
never retains a client pointer across a wait, and checks a mutation epoch before
interpreting stale wait failures. The owner removes an endpoint from the watch
set and interrupts the wait before closing it. Request inventory is preallocated
per queue and charged to a RAM-derived native budget. Worker private input/output
budgets likewise derive from sandbox RAM, with separate management reserve.
Ring/worker integration, delayed retirement ACK, admission rollback, management
barriers and 513-client wait rotation pass native ASAN/UBSAN tests. Real Linux
lock-wait gates and the production NVMe-root FIFO test verify VFS concurrency;
offline e2fsck passes on a separate private COW without changing the workload disk.
Optional service profiling reports queue residence separately from VFS execution.

## Management retirement boundary

Filed revokes the common Linux client identity before draining any of its queues.
The internal registry acknowledgment stops new VFS admission for every binding;
previously prepared requests keep their retained authority and files. The external
manager's unregister call succeeds only after every queue's drain, mapping retirement,
matching native acknowledgment and local capability close. A remaining session
retains its client record if an acknowledgment or close fails. Missing or malformed
management RPC replies fault the storage generation rather than letting cached
authority continue serving an uncertain credential update.

FD-transfer escrow retirement closes its owner-held Linux handle before closing
the native lease endpoint. A failed close retains the lease and its exact remaining
handle/FD inventory and faults the storage generation. An acknowledged handle close
is not repeated; retiring a client-owned lease does not close that client's OFD.

Request-owned file views retain the original OFD, not a reopened path. Installs and
close still act on the common client table. Owner transfers retain both clients and
the source OFD; private executable reads/stat retain their exec guard at admission.
Request teardown drops these refs and runs Linux task work before worker completion.

LPR never republishes an executed write after a wait interruption or malformed reply.
Uncertain retirement remains an error on repeated drop, with the mapping/inventory
retained and no remote retry. Only already-acknowledged retirement permits retrying
the exact remaining local cleanup, and success requires that cleanup to finish.
Private queue capabilities are not inherited at fork; the child discards its copied
queue view without closing reused native FD numbers and establishes its own binding.
Production tests verify fork/dup/exec shared offsets, child-only CLOEXEC and parent
FD validity after child exit, alongside native peer-death/drain and fault gates.

## Operations

The catalog places owner/session/exec operations before the Linux operations.
Breaking corrections insert entries in their logical location and regenerate
all consumers; there are no old opcode aliases. Useful operand mappings are:

| Operation | Request operands | Successful response |
| --- | --- | --- |
| SESSION_DETACH | generation = attached queue generation; second_handle = channel ID | result = 0; handle = retired channel ID |
| DEVICE_ROUTE (frontend only) | original OPENAT2 dirfd/path/flags/mode/resolve | pinned O_PATH handle; host service tag; encoded statx |
| EXEC_OPEN (owner only) | client/revision; handle = ordinary dirfd; path | handle = private executable reference |
| OPENAT2 | handle = dirfd; path; flags/mode/resolve; umask | handle = new file reference |
| READ/PREAD | handle; length; offset for PREAD | result = bytes; raw data |
| WRITE/PWRITE | handle; length = data span length; offset for PWRITE | result = bytes |
| STATX | handle = dirfd; optional path; flags; mask | explicitly encoded 256-byte statx |
| STATFS | handle | explicitly encoded 120-byte statfs |
| RENAMEAT2/LINKAT | handle/second_handle = dirfds; path/second_path; flags | result = 0 |
| SYMLINKAT | path = target; second_path = new name; handle = dirfd | result = 0 |
| MKNODAT | handle = dirfd; path; mode; length = Linux encoded device | result = 0 |
| FCNTL | handle; flags = command; length = scalar argument | result = Linux result |
| FTRUNCATE | handle; offset = new file size | result = 0 |
| SYNC | no file handle | result = 0 after Linux global synchronization |
| SYNCFS | handle = open file reference | result = Linux filesystem synchronization result |
| READLINK_HANDLE | handle; length = capacity; flags = 0 or 1 | result = bytes; raw path |
| FILE_VMO | handle; offset; length; mapping rights | result = actual bytes; native VMO capability |

`READLINK_HANDLE` uses Linux `d_path`, not a saved open-time pathname. Flag 0
has readlink-style truncation and preserves deleted/anonymous-file naming.
Flag 1 is directory/getcwd mode: it requires space for a final NUL, returns
`ERANGE` for a small buffer and `ENOENT` for a removed directory. Its result
still excludes the NUL, which the host appends. Paths are relative to the
granted root. `FILE_VMO` produces an uncached private snapshot; its byte count
is not placed in the opaque handle field.

`DEVICE_ROUTE` is a side-effect-free fallback after a direct open returns
`ENXIO`/`ENODEV`, never a second ordinary open. The owner entry snapshots the
original request, retains the registered client's rights/credentials together,
checks the original open's capability requirements and filesystem-ID DAC, and
opens only `O_PATH`: no create, truncate or driver open. Filed classifies the
same pinned device; LPR selects its existing native service from that statx,
without resolving an alias pathname again, then closes the probe handle.
Ordinary-file and unsupported-device failures do not replay filesystem I/O.

Host-side private snapshot assembly reads in pieces that fit the configured
storage arena. This avoids full-image reply and temporary-channel copies
alongside live parent/child compiler images. The caller completes the whole
snapshot; arena size is not a file-size limit. Ordinary client READ/PREAD and
WRITE requests remain single Linux operations, including oversized exchanges.

`EXEC_OPEN` is an owner-only preparation operation, not a way to give ordinary
clients read access to execute-only files. It uses upstream `open_exec` under
the authenticated actor's credentials and the granted filesystem root. Its
references live in a separate private namespace: only owner-channel PREAD,
STATX and CLOSE can use them. They cannot be duplicated, transferred or
exported to clients. A reference-counted object holds Linux's deny-write guard
through image loading and in-flight reads, releasing it exactly once on final
release. This protects preparation of the private executable snapshot; it does
not claim Linux `mm->exe_file` write exclusion for a running PachaOS process.

Errors are negative Linux errno, with no data or handle. A valid authenticated
request that cannot allocate its private snapshot gets `ENOMEM`, not silent
success. Malformed framing is a transport error. The GPL service calls real
Linux namei, VFS, shmem and syscall helpers; it does not reconstruct Linux
structures using numeric offsets.

`SYNC` and `SYNCFS` are distinct because Linux `sync()` flushes every
superblock and block device in this isolated storage runtime, whereas
`syncfs(fd)` operates on one valid open file's filesystem and reports its
writeback errors. A zero handle is not a global-sync alias for `SYNCFS` and
returns `EBADF`. Neither operation requires a write-only file or a WRITE
grant; Linux permits an unprivileged caller to synchronize and accepts
read-only descriptors for `syncfs`. The authenticated actor's credentials
remain scoped to the operation. `SYNC` is inserted immediately before
`SYNCFS` in the dev catalog and all later operations are renumbered; keeping
an accidental zero-handle encoding would hide a semantic mismatch.

## Queue and lifetime

The host FS channel also exposes caller-owned submit, poll, arm and collect
operations over the existing split ring. Each pending request owns two
descriptors, a disjoint arena lease and a private request identity through its
own collection. Another caller consuming its used entry does not release its
lease. Arena/descriptor exhaustion returns local backpressure before publish;
ambiguous publication and malformed completions retain inventory on a faulted
channel until confirmed remote retirement. These operations require one local
queue owner or short external exclusion, not a lock held across remote waits.
Reserve/publish and read-result/release are also separate so large payload
copies run outside queue exclusion while descriptor/arena leases remain pinned.
LPR connects multiple callers: one drains doorbells, all used entries stay in
their own pending inventory, and a futex epoch wakes local waiters on completion,
collection and receiver handoff. Final retirement closes admission and waits
for caller references without holding a queue lock. A private local endpoint pair
wakes the native receiver on a sibling's queue fault without waiting for a remote
reply. Nonblocking native receive's EMPTY result means wait, not channel failure.
Host tests cover blocked-first reversed completion, descriptor and aggregate lease
backpressure, in-flight teardown and fault-time wakeup. NVMe-root QEMU checks pass
for ordinary operations, oversized direct I/O, device routing and four-thread
shared-offset vector I/O / atomic append, alongside native libc FS/exec checks.
Post-run e2fsck succeeds without structural repair in a separate private COW.
Failure injection verifies pre-attach reservation cancellation, lost-ACK retention,
partial local cleanup inventory, and native wait interruptions without operation
replay. This does not claim concurrent sandbox VFS execution.

Data and management channels use the shared split-ring engine. Channel metadata also
contains the generated filesystem schema digest. Both sides compare private
snapshots with independently expected policy before binding; a stale binary
is rejected, not accepted through compatibility fallback. HELLO flag 1 reserves
a fresh binding ID for the authenticated client and returns the 32-byte config;
the ID is distinct from client identity. Its length requests a minimum arena.

Queue retirement is independent of managed client retirement. A manager or
another capability holder can retain a duplicate of the same client endpoint
after a runtime closes its local copy. Waiting for that endpoint's HANGUP
before native exit or self-exec creates a circular lifetime dependency.
`SESSION_DETACH` therefore follows `SESSION_ATTACH` in the operation catalog;
all subsequent operation IDs and the schema digest are regenerated together,
with no version number, legacy opcode alias or compatibility fallback.

`SESSION_DETACH` uses the authenticated frontend control endpoint, not a ring
descriptor or the owner credential channel. It carries exactly the request
header with no capability transfers: `generation` selects the attached queue
generation and `second_handle` its channel ID. All other operation operands
and spans are zero. Client/credential envelope fields cannot replace the
endpoint's authenticated actor. The server only retires that actor's own queue
with matching ID and generation. It first quiesces/faults the queue, drains and
releases accepted jobs, unmaps its
shared memory and closes its retained queue capabilities, then acknowledges
with `result = 0` and `handle = channel ID`. Only that validated acknowledgment
allows the caller to release its own mapping; closing a local endpoint alone
is not an acknowledgment. Missing or repeated detach requests fail, and an
uncertain native retirement retains unresolved inventory without a successful
acknowledgment. Client identity, credentials, file references and file leases
are not revoked by queue detach. A retiring queue can still have executing jobs;
its mapping and private chain inventory remain pinned until those jobs drain.

Normal filesystem and owner credential queues are separate. A descriptor's
client field alone never authorizes an operation. Request bytes are copied
into private storage before path checks or Linux execution, and response
storage stays privately owned until completion.

Arena size is a negotiated transport budget, not a filesystem I/O limit.
LPR reserves/binds an additional larger queue for the same authenticated client
without retiring a queue that still has callers; credentials and files survive.
Each additional queue is retired immediately after its call, and its mapping and
capabilities are released only after the matching sandbox acknowledgment.
Filed budgets aggregate shared backing, worst-case immutable input snapshots and
private inventory against one quarter of configured storage sandbox RAM. Lease
count is bounded by half the real native FD table (two retained capabilities per
lease). Idle client arena capacity is derived from that RAM/count budget, capped
by the configured owner arena, rather than reserving the large owner arena for
every process. Larger calls negotiate larger leases, not smaller Linux operations.
Admission pressure waits for this client's live temporary leases to retire;
foreign pressure with no local wake source returns allocation failure. Lost
acknowledgments retain the charge and uncertain inventory until terminal teardown.
Failed pre-attach allocation cancels the HELLO reservation through matching
DETACH acknowledgment. A lost cancellation or malformed configuration closes
admission; it cannot repeatedly reserve new generations. Failed local close/unmap
retains exactly the resources still owned, including metadata until the last step.
It then publishes one ordinary request directly, with no filed relay fallback.
An uncertain queue cannot be reset by requesting a larger arena. Trusted native
management calls may instead use temporary owner-private channels. Large read/write/readv
operations remain one Linux operation, not a sequence of interleavable calls.
Allocation can fail against actual memory/native descriptor budgets.
Scalar DATA calls do not allocate a staging VMO, and vector I/O snapshots only
the vector metadata: inputs copy into the request-owned arena and outputs scatter
from its completion, without a full-size private bounce allocation.

Teardown first retires all requests and native mappings, then terminates and
waits for the sandbox, revokes DMA and releases the controller generation.
Uncertain retirement retains inventory without remote replay; confirmed remote
retirement permits retrying only remaining local cleanup. A sandbox failure
invalidates its generation; handles are not reconstructed against a new root.

## Host integration and current scope

PachaOS owns role policy and reads immutable storage packages directly from
its original read-only bootfs capability. The common owner startup helper is
used by USB, network, GPU and storage; the storage role does not introduce a
fourth copy of startup/lifecycle logic. Device discovery walks actual block
devices under the authorized PCI ancestors and selects the configured ext4
UUID, rejecting ambiguity. The initial single-NVMe package is a concrete
policy, not a one-device structural limit. Runtime RAM and queue budgets come
from immutable boot configuration.

filed owns client authentication/capability policy, connection management,
native control operations, exec, private VMO export and
service routing. Its VFS/cache/tmpfs and LPR filesystem data/name/offset caches
are removed; Linux owns namespace and file state. Exec uses variable spans,
counts and strings instead of the old fixed argument/environment buffers.

SHARED_FILE_VMO exports pinned Linux page-cache/shmem folios through native
sparse page views, never private pread copies. A generation/sequence/export-ID
bound management attachment carries the attenuated native capability. Linux
owns mmap admission, seals and filesystem page_mkwrite; FileD owns client
authentication. Truncate/growth refresh page views and retire native TLBs
before releasing old pins. Beyond-EOF access is delivered as Linux SIGBUS by
LPR. Non-growing mremap relocation/shrink retains the same view identity and
offset; growth of a shared range view is not supported. Exports retain their
writable-mapping seal counter until the last native
alias/capability has retired and revocation succeeds. Writable exports are
tracked by their lifetime permission ceiling, not current mprotect state.
For shmem with native read-only exports, the Kobox seal bridge retains every
physical pin and compares Linux's expected folio references plus precisely
its own read-only slot references. Other external pins still block WRITE
sealing. Admission, security checks, EXEC implications and writable-counter
rollback follow Linux memfd semantics; upstream Linux is unchanged.
An empty inventory admits concurrent backing operations under a read guard;
publication requires write admission and waits for those operations. Idle
maintenance does not queue a needless writer. With exports present, backing
operations (including reads for O_DIRECT coherence) still serialize globally;
unrelated namespace/stat operations do not. This is not per-inode concurrency.
Writable exports are conservatively dirtied before filesystem operations and periodically because
native PTEs do not participate in Linux page_mkclean tracking. DAX/device and
non-page-cache exports are unsupported. Live cpio boot and pointer-bearing
fcntl lock serialization remain outside this implementation. Host unit tests
alone do not establish browser or guest writeback correctness.
Native MPROTECT currently reports an authority-ceiling rejection with the
same status as invalid ranges; LPR therefore returns EINVAL rather than Linux's
EACCES. The write escalation is denied; exact errno parity remains unsupported.
