# GPU protocol

## Identity

The GPU data plane uses the `kobox2.gpu` protocol over one transport channel.
Its identity is `dev` until an explicit ABI freeze. Every profile fixes the
base schema digest, command-set schema digests, closure digest, resource
interfaces, and limits as one immutable unit. Each launch binds that profile
to a generation-specific resource-grant digest.

VirGL and AMDGPU use the same base protocol. Linux DRM operation details are
carried by schema-selected command sets:

| Profile | Command sets |
|---|---|
| VirGL | DRM core, DRM mode, DRM virtgpu |
| AMDGPU | DRM core, DRM mode, DRM amdgpu |

The base protocol owns sessions, dispatch, spans, native-object exchange,
completion, notification, fault, ordering, and generation. A command-set
schema owns the canonical pointer-free representation of each DRM operation.

## Boundaries

| Component | Authority |
|---|---|
| PachaOS VFS/personality bridge | DRM namespace, file-descriptor behavior, UAPI copy and canonical serialization |
| `gpud` | client policy, session routing, exchange broker, kobox2 controller and GPU profile |
| GPU sandbox | `drm_file`, GEM/TTM, contexts, VM, sync objects, KMS state, driver state and hardware queues |
| kobox2 controller | sandbox generation, closure, resource grant and restart lifecycle |

One open file description maps to one GPU session. `dup` and `fork` retain that
session; an independent `open` creates a new session. The last host reference
closes the session. Primary and render nodes are distinct session types.

Linux ioctl numbers, native C layouts, pointers and host handles terminate at
the Linux-personality boundary. `gpud` routes validated canonical messages.
The GPL sandbox reconstructs the pinned Linux v6.18.48 UAPI operation and owns
its Linux semantics.

## Profile and command catalog

A GPU profile contains:

- driver identity and primary/render node availability;
- the base GPU schema digest;
- an ordered command-set catalog with a schema digest for every set;
- an ordered event-set catalog with a schema digest for every set;
- queue counts and all byte, object, request and time limits; and
- the exact closure and resource interface digests.

Each catalog entry assigns a nonzero set ID. Each command entry fixes its
command ID, queue class, request and response bounds, span roles, attachment
roles, synchronization behavior and cancellation behavior. Unknown sets,
commands and schema digests are rejected before side effects.

The DRM core set covers version and capability queries, client capability,
GEM close, PRIME/dma-buf exchange and the complete DRM syncobj/timeline
operation family. The DRM mode set covers primary-node authority, master and
lease state, resources, connectors, encoders, CRTCs, planes, framebuffers,
properties, blobs, atomic commits, page flips, vblank and sequence events.

The DRM virtgpu set covers the complete pinned `virtgpu_drm.h` operation set:
map, execbuffer, getparam, resource create/info, both 3D transfers, wait,
capset query, blob creation and context initialization.

The DRM amdgpu set covers the complete pinned `amdgpu_drm.h` operation set,
including GEM create/map/metadata/VA, contexts, BO lists, command submission,
device and firmware queries, waits, VM control, fences, scheduling and user
queues. Nested CS chunks and query outputs are separate bounded schema records.

## DRM core command contract

| ID | Command | Canonical contract |
|---:|---|---|
| 1 | `VERSION` | three output byte spans; version and returned lengths |
| 2 | `GET_UNIQUE` | output byte span; returned and required length |
| 3 | `GET_CLIENT` | client index; fixed-width client record |
| 4 | `GET_STATS` | output stat-entry span; returned count |
| 5 | `SET_VERSION` | four signed version components in both directions |
| 6 | `GET_CAP` | capability ID; 64-bit value |
| 7 | `SET_CLIENT_CAP` | capability ID and 64-bit value |
| 8 | `SET_CLIENT_NAME` | bounded input byte span |
| 9 | `GEM_CLOSE` | session-local GEM handle |
| 10 | `GEM_FLINK` | GEM handle; global name |
| 11 | `GEM_OPEN` | global name; GEM handle and size |
| 12 | `GEM_CHANGE_HANDLE` | old and new session-local handles |
| 13 | `PRIME_HANDLE_TO_FD` | GEM handle; moved dma-buf attachment |
| 14 | `PRIME_FD_TO_HANDLE` | shared dma-buf attachment; GEM handle |
| 15 | `SYNCOBJ_CREATE` | creation flags; syncobj handle |
| 16 | `SYNCOBJ_DESTROY` | syncobj handle |
| 17 | `SYNCOBJ_HANDLE_TO_FD` | handle, flags and point; moved syncobj or sync-file attachment |
| 18 | `SYNCOBJ_FD_TO_HANDLE` | shared syncobj or sync-file attachment; syncobj handle |
| 19 | `SYNCOBJ_WAIT` | handle span, flags and absolute deadline; first signaled index |
| 20 | `SYNCOBJ_RESET` | handle span |
| 21 | `SYNCOBJ_SIGNAL` | handle span |
| 22 | `SYNCOBJ_TIMELINE_WAIT` | handle/point span and absolute deadline; first signaled index |
| 23 | `SYNCOBJ_QUERY` | in/out handle/point span; returned count |
| 24 | `SYNCOBJ_TRANSFER` | source/destination handles and points |
| 25 | `SYNCOBJ_TIMELINE_SIGNAL` | handle/point span |
| 26 | `SYNCOBJ_EVENTFD` | handle and point plus shared event attachment |

## DRM mode command contract

Every object-bearing request carries the topology epoch. Query completions
return the epoch governing their object IDs.

| IDs | Commands | Canonical contract |
|---:|---|---|
| 1–4 | `GET_MAGIC`, `AUTH_MAGIC`, `SET_MASTER`, `DROP_MASTER` | primary-session authority and magic values |
| 5–7 | `WAIT_VBLANK`, `CRTC_GET_SEQUENCE`, `CRTC_QUEUE_SEQUENCE` | CRTC identity, sequence, timestamp, event token and deadline where applicable |
| 8 | `GETRESOURCES` | bounded framebuffer, CRTC, connector and encoder ID output spans |
| 9–10 | `GETCRTC`, `SETCRTC` | CRTC record plus typed mode and connector spans |
| 11, 35 | `CURSOR`, `CURSOR2` | CRTC, GEM handle, geometry and hotspot |
| 12–13 | `GETGAMMA`, `SETGAMMA` | equal-length red, green and blue `u16` spans |
| 14–15 | `GETENCODER`, `GETCONNECTOR` | object metadata plus typed mode, property and encoder spans |
| 16–17 | `ATTACHMODE`, `DETACHMODE` | connector and one mode record |
| 18–20 | `GETPROPERTY`, `SETPROPERTY`, `GETPROPBLOB` | typed values/enums and bounded blob bytes |
| 21–25 | `GETFB`, `ADDFB`, `RMFB`, `PAGE_FLIP`, `DIRTYFB` | framebuffer records, event token and rectangle span |
| 26–28 | `CREATE_DUMB`, `MAP_DUMB`, `DESTROY_DUMB` | GEM allocation and memory mapping attachment |
| 29–31 | `GETPLANERESOURCES`, `GETPLANE`, `SETPLANE` | plane IDs, formats and source/destination geometry |
| 32, 43–44 | `ADDFB2`, `GETFB2`, `CLOSEFB` | four-plane format, pitch, offset and modifier record |
| 33–34 | `OBJ_GETPROPERTIES`, `OBJ_SETPROPERTY` | object type and typed property/value records |
| 36 | `ATOMIC` | object index span, typed property span and output syncobj span |
| 37–38 | `CREATEPROPBLOB`, `DESTROYPROPBLOB` | bounded input bytes and blob object ID |
| 39–42 | `CREATE_LEASE`, `LIST_LESSEES`, `GET_LEASE`, `REVOKE_LEASE` | object-ID spans, lessee IDs and moved session attachment |

Atomic property values select scalar, object, blob, input-syncobj or
output-syncobj form. The adapter converts syncobj forms to the Linux fence-FD
properties. Returned fences are session syncobj handles in the declared output
span.

## DRM virtgpu command contract

| ID | Command | Canonical contract |
|---:|---|---|
| 1 | `MAP` | GEM handle and rights; mapping record and memory attachment |
| 2 | `EXECBUFFER` | command bytes, BO handles, input/output syncobj spans and optional sync-file attachments |
| 3 | `GETPARAM` | parameter ID; 64-bit value |
| 4 | `RESOURCE_CREATE` | 3D resource geometry, format, bind and backing fields; BO/resource handles |
| 5 | `RESOURCE_INFO` | BO handle; resource size and blob-memory class |
| 6–7 | `TRANSFER_FROM_HOST`, `TRANSFER_TO_HOST` | BO handle, 3D box, level, offset and strides |
| 8 | `WAIT` | BO handle, flags and absolute deadline |
| 9 | `GET_CAPS` | capset ID/version and bounded output byte span |
| 10 | `RESOURCE_CREATE_BLOB` | blob class, flags, size, ID and bounded command byte span |
| 11 | `CONTEXT_INIT` | parameter mask, capset, ring configuration and bounded debug-name span |

`CONTEXT_INIT` gives the debug name its own byte span; its numeric parameter
record therefore contains no native pointer. `EXECBUFFER` represents fence FDs
as sync-file attachments and preserves its ring and timeline-syncobj fields.

## AMDGPU command contract

The AMDGPU set fixes 20 commands against Linux 6.18.48. Argument 1 is the
command's inline request record. Each additional argument is bound by the
schema to a typed span or native-object attachment.

| ID | Command | Request data | Completion data |
|---:|---|---|---|
| 1 | `GEM_CREATE` | allocation record | GEM handle |
| 2 | `GEM_MMAP` | handle and mapping rights | mapping record and memory attachment |
| 3 | `CTX` | operation, context and priority | operation-selected context result |
| 4 | `BO_LIST` | operation plus BO-entry span | list handle |
| 5 | `CS` | context plus repeated typed chunk spans | submission handle |
| 6 | `INFO` | query, selector and output span | returned record and required count |
| 7 | `GEM_METADATA` | metadata record | metadata record |
| 8 | `GEM_WAIT_IDLE` | GEM handle and deadline | busy state and domain |
| 9 | `GEM_VA` | mapping record plus input-syncobj span | status |
| 10 | `WAIT_CS` | submission identity and deadline | busy state |
| 11 | `GEM_OP` | operation plus typed output span | returned record and required count |
| 12 | `GEM_USERPTR` | range plus memory attachment | GEM handle |
| 13 | `WAIT_FENCES` | fence span and deadline | signaled state and first fence |
| 14 | `VM` | VM operation | VM flags |
| 15 | `FENCE_TO_HANDLE` | fence and output kind | syncobj handle or syncobj/sync-file attachment |
| 16 | `SCHED` | operation plus target-session attachment | status |
| 17 | `USERQ` | queue record plus typed MQD span | queue ID |
| 18 | `USERQ_SIGNAL` | syncobj, read-BO and write-BO spans | status |
| 19 | `USERQ_WAIT` | syncobj, timeline, BO and output-fence spans | returned and required counts |
| 20 | `GEM_LIST_HANDLES` | output-entry span | returned and required counts |

`CS` assigns separate record IDs to all ten UAPI chunk kinds, including input
and output syncobjs, ordinary and scheduled dependencies, and timeline wait
and signal. `INFO` assigns the 27 defined query IDs to an allowed response
record and cardinality. Record sizes, field offsets, UAPI command numbers,
chunk IDs, argument IDs, bounds, directions, attachment classes and ownership
are generated from the schema and checked before encode or dispatch.

## Startup resources

The role closure declares these resource interfaces. Each interface has its
own schema digest and native-handle roles.

| Interface | Resource type and rights | Consumer |
|---|---|---|
| PCI function | device: command, map, DMA; reset-required | device-pci provider |
| DMA domain | device: command, DMA | device-pci provider |
| IRQ endpoint | notification: wait | device-pci provider |
| firmware store | memory: read, map | AMDGPU driver |
| object exchange | channel: send, receive | DRM bridge |
| GPU data channel | channel: send, receive | GPU role root |

The PCI interface provides canonical config-space access and bounded BAR map
requests. The DMA interface creates, maps, unmaps and synchronizes DMA memory
inside the generation's IOMMU domain. IRQ objects carry source identity and
generation. The firmware store is present in the AMDGPU profile and absent in
the VirGL profile. Hard reset authority remains with the host lifecycle bound
to the reset-required device resource.

Every interface is explicitly bound to its consumer nodes. Sharing one
interface between nodes uses one closure-shared grant object and preserves the
same underlying authority. The sandbox commits all device interfaces before
mapping the DRM closure.

The PCI, DMA, and IRQ contracts and their fixed GPU closure slots are defined
by [device-resource-interfaces.md](./device-resource-interfaces.md).

## Queue topology

The channel has these queue classes:

| Queue | Count | Traffic |
|---|---:|---|
| event | 1 | sandbox-to-`gpud` notifications |
| control request | 1 | profile, session and cancellation requests |
| display request | 1 | DRM mode commands |
| execution request | profile-defined, at least 1 | render, compute, memory and synchronization commands |

Every queue uses the split-virtqueue rules of the transport schema. A command
catalog entry selects exactly one request queue class. Execution lanes may run
concurrently. Available-ring order is preserved within one queue; ordering
across queues is expressed by sync points and command completion.

## Base messages

Requests use the transport envelope generation and correlation ID.

| Request | Result |
|---|---|
| `PROFILE_QUERY` | immutable profile identity, catalogs, features and limits |
| `SESSION_OPEN` | a new generation-scoped session ID and node capabilities |
| `SESSION_CLOSE` | closure of the session after its accepted requests leave dispatch |
| `COMMAND` | one catalog command and its canonical response |
| `CANCEL` | cancellation disposition for one cancellable correlation ID |

`gpu_session.h` provides the canonical base-session codec. The envelope keeps
the request opcode. Session completions use `completion_header` without
argument, span or attachment descriptors; all table offsets and `inline_offset`
equal the header size. Successful `SESSION_OPEN` puts `session_open_response`
directly in the inline area, with the same nonzero session ID in both records.
The base opcode selects this result structure; it is not a command-catalog
record. Failed OPEN has session ID zero and no inline bytes. CLOSE retains the
requested nonzero ID and has no inline bytes, including on error. Reserved and
detail fields are zero and disposition is `COMPLETED`. Callers separately
validate envelope generation/correlation and the requested CLOSE ID.

The event queue carries two base messages:

| Event | Meaning |
|---|---|
| `NOTIFY` | one catalog-defined DRM, KMS, sync or driver event |
| `DEVICE_FAULT` | terminal fault information for the current GPU generation |

Every completion contains a base status, response length, output span count,
output attachment count and command disposition. Command acceptance and GPU
execution completion are separate facts. A successful asynchronous submission
returns after driver acceptance; its declared sync points report execution.

Base session, exchange and notification IDs are nonzero 64-bit values scoped
to one generation. Command-set-local DRM handles retain the width and semantics
defined by their command schema and are scoped to one session. They never act
as base-protocol IDs. KMS object IDs are additionally paired with a topology
epoch.

## Command data

A `COMMAND` identifies the session, command set, command ID and request flags.
Its data consists of:

- a bounded inline canonical payload;
- bounded spans into registered transport regions; and
- bounded native-object attachments keyed by exchange ID.

A span contains region ID, offset, length and access rights. Input spans remain
immutable until completion. Output spans become visible at completion. The
decoder validates all nested counts, ranges, alignment, rights and overlap
before invoking DRM code.

The command-set schema replaces every UAPI pointer with an inline range, a
span, or an attachment. VirGL command streams remain driver command bytes in
declared read-only spans. AMDGPU indirect buffers are addressed through the
session's mapped GPU virtual address; BO lists, synchronization dependencies
and output records use typed bounded spans.

## Memory and native-object exchange

The startup resource grant supplies a generation-scoped exchange channel.
Linux uses `SCM_RIGHTS`; PachaOS uses native capability transfer. Wire messages
carry only a nonzero exchange ID, object class, rights and role.

Dynamic attachments cover shared memory, dma-buf, syncobj, sync-file, event
and leased session objects. An exchange ID is unique within one generation and
is never reused. The broker matches the native transfer and command attachment
by generation and exchange ID before making either visible.

Attachment ownership modes are:

| Mode | Commit result |
|---|---|
| borrow | receiver access lasts through command completion |
| share | receiver obtains an independent reference |
| move | receiver obtains ownership and sender releases its reference |

All attachments are staged and validated as one transaction. A rejected
transaction returns every input to its previous owner. A completion records
the final disposition of every input and output attachment. Process exit drops
all staged and committed native references of that generation.

GPU memory identity is independent of a per-session DRM handle. PRIME export
publishes a generation-scoped shared object through the broker; each importing
session receives its own DRM handle. CPU mapping publishes a memory attachment
with the requested mapping rights and cache policy. Device DMA remains inside
the sandbox resource grant and IOMMU domain.

## Synchronization and events

The common synchronization identity is a session sync object plus a 64-bit
timeline point. Binary sync objects use their command-set-defined binary
point. Submit and atomic-display commands declare input and output sync points.
Sync-file import/export uses native-object attachments.

Transport completion orders response data only. GPU work is ordered by driver
queues and explicit sync points. A wait command carries an absolute monotonic
deadline and is cancellable. Event delivery is ordered per session by a
monotonic event sequence. KMS user data is an opaque 64-bit value and is
returned unchanged with its page-flip, vblank or sequence event.

## Display ownership

Primary sessions may acquire DRM master or a lease according to `gpud` policy.
Render sessions expose render authority. The sandbox owns KMS objects and
atomic state; `gpud` owns which client may address them. Hotplug changes the
device topology epoch and produces a catalog event. Object IDs are interpreted
with the topology epoch returned by their query.

Atomic commit completion means DRM accepted the state. An out-fence or event
reports scanout completion. Display requests have a dedicated queue so render
traffic cannot consume their transport capacity.

## Status and fault

Base completion statuses are `OK`, `INVALID`, `UNSUPPORTED`, `DENIED`,
`NOT_FOUND`, `STALE_GENERATION`, `LIMIT`, `NO_MEMORY`, `BUSY`, `TIMED_OUT`,
`CANCELED`, `SESSION_LOST` and `DEVICE_LOST`. Command sets map these statuses
and their own validated detail values to the client UAPI result.

A well-formed refused operation completes with `UNSUPPORTED`. An invalid
session or object completes with `NOT_FOUND` or `STALE_GENERATION`. Descriptor,
envelope, schema, span, attachment or reserved-field violations fault the
channel.

`DEVICE_FAULT` contains the fault class, source set, source ID, optional guilty
session, reset requirement and diagnostic token. It ends the current
generation. Hardware reset is performed by the host resource lifecycle after
the sandbox process exits.

## Generation lifecycle

Normal stop proceeds as follows:

1. `gpud` stops session creation and command admission.
2. kobox2 requests quiesce on the management channel.
3. the sandbox cancels waits, drains accepted requests, closes sessions and
   quiesces the DRM closure;
4. process exit completes native-reference revocation; and
5. the host revokes, resets, reaps and releases the generation resources.

Fault cleanup terminates the process and follows the same revoke, reset, reap
and release boundary. Restart allocates new channel memory, notification
objects, resource grants, object IDs, exchange IDs and generation. `gpud`
discards old completions and reports device loss to every old client session.

## Required limits

Every profile fixes limits for sessions, in-flight requests, execution lanes,
inline bytes, spans, attachments, command bytes, objects per session, total GPU
memory, pinned memory, mappings, contexts, sync objects, KMS objects, event
backlog and wait duration. Limit checks occur before ownership transfer or DRM
side effects. Counters return to the profile baseline after session close and
after every generation teardown.

## Conformance gates

Protocol conformance covers canonical encoding, every command-set catalog,
malformed nested data, rights, attachment rollback, concurrent lanes,
completion ordering, event backpressure, cancellation, queue wrap-around,
generation rollover and stale input.

The VirGL profile gate requires a VirGL capset, a render node, successful
Mesa/libdrm submission, `VIRTIO_GPU_CMD_SUBMIT_3D` greater than zero,
`VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D` equal to zero on the accelerated path, and
a renderer that is neither llvmpipe nor swrast. Fault injection covers request
dispatch, attachment exchange, device submission, fence completion, KMS event
delivery and restart.

The AMDGPU catalog gate encodes every pinned AMDGPU command through the same
base messages and attachment model. The RX 9060 XT execution gate uses the same
session, synchronization, event, fault and generation rules with its AMDGPU
closure and device resource interfaces.
