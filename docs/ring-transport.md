# Ring transport

## Model

kobox2 uses the Virtio 1.0 little-endian split virtqueue layout. Each channel
has one event virtqueue and one or more request virtqueues. Each available ring
and used ring has one producer.

A request chain contains readable request buffers and writable response
buffers. An event queue contains writable buffers supplied by the client and
completed by the sandbox. Control and role data channels share this transport
and use separate protocol IDs. Hardware device queues belong to the sandbox
and device interface.

## Queue profile

- `VIRTIO_RING_F_INDIRECT_DESC` and `VIRTIO_RING_F_EVENT_IDX` are required.
- Queue size is a power of two from 16 through 32768.
- Direct and indirect scatter-gather chains are supported.
- Used-element descriptor IDs correlate completions in any completion order.
- The channel descriptor fixes queue size, maximum chain and indirect-table
  lengths, and maximum outstanding requests.

The channel descriptor contains its size, transport version, feature bits,
channel ID, sandbox generation, queue definitions, notification endpoints,
and transport-address regions. Reserved fields are zero. Compatible extensions
increase the descriptor size and select feature bits; incompatible layouts
increase the transport major version.

## Addressing

Each transferred shared-memory capability receives a non-overlapping 64-bit
transport-address interval. Every descriptor range resolves within one
registered interval with matching access rights. A registration contains its
region ID, transport base, length, rights, and generation.

IPC virtqueue memory resides in shared VM objects outside hardware device DMA
address spaces. The host port associates registrations with native
shared-memory capabilities.

## Message envelope

Every request, response, and event starts with this little-endian envelope:

| Field | Type |
|---|---|
| protocol ID | `u32` |
| protocol major | `u16` |
| protocol minor | `u16` |
| opcode | `u32` |
| flags | `u32` |
| generation | `u64` |
| correlation ID | `u64` |
| payload length | `u32` |
| reserved | `u32` |

Responses copy the request correlation ID; unsolicited events use zero. The
used length records the bytes written into writable descriptors.

## Operation

- Available-side publication uses descriptor writes, a release barrier, and
  an available-index update. Consumption begins with an acquire barrier.
- Used-side publication uses a used-element write, a release barrier, and a
  used-index update. Response access begins with an acquire barrier.
- `EVENT_IDX` controls coalescible wake notifications; ring indices are the
  authoritative progress state.
- Consumers validate descriptor indices, chain structure, lengths, address
  bounds, rights, generation, message size, version, flags, and reserved fields.
  A validation failure moves the channel to `FAULTED` and emits a control fault.
- Each sandbox generation receives new virtqueue memory, channel IDs, address
  registrations, and notification endpoints. Restart orders capability
  revocation, device reset, process termination, allocation, transfer, and
  control handshake.
- The controller retains the management channel. Data-channel ownership moves
  to the client and sandbox when channel establishment completes.
