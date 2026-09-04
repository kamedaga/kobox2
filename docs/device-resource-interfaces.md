# Device resource interfaces

## Slots

GPU closures reserve these required slots for `device-pci.so`:

| Slot | Interface | Resource rights | Native handle |
|---:|---|---|---|
| 1 | `kobox2.pci-function` | device: command, map, DMA | PCI function capability |
| 2 | `kobox2.dma-domain` | device: command, DMA | domain and context capabilities |
| 3 | `kobox2.irq-endpoint` | notification: wait | 1–2048 nonblocking eventfds |

PCI and DMA slots contain one object. The IRQ slot contains one object per
source. Object identity is the generation and object ID from its grant. PCI
reset authority belongs to the host generation lifecycle.

## PCI function

The interface returns PCI identity, provides aligned 8-, 16-, and 32-bit
configuration access, and maps bounded BAR subranges. BAR mappings retain the
protection and cache properties of the granted function.

Linux accepts a VFIO PCI device descriptor. A conformance backend implements
the same operation table for deterministic tests.

## DMA domain

The interface reports address width, alignment, segment size, and coherency.
It owns DMA allocations and mappings, explicit CPU/device synchronization, and
drain accounting. A mapping borrows its allocation and must be released first.
Device addresses are valid only in the grant generation's IOMMU domain.

Linux receives a domain FD and a sealed context memfd. The context selects an
iommufd IOAS, VFIO container, or conformance backend and carries its backend
object ID. The pair implements the same operation table.

## IRQ endpoint

Each interface object binds one handler to one generation-scoped source.
Delivery drains its eventfd counter on a core-runtime-admitted provider thread
and invokes the handler synchronously in that dispatch context. Disable closes
admission and waits for a running handler before returning.

## Probe gate

`device-pci.so` binds all three exact schema digests and validates their
generation and object IDs. It publishes an immutable resource snapshot only
while active. Linux PCI enumeration and `virtio_pci.ko` probe begin after this
gate. Quiesce disables IRQ delivery and requires zero live DMA allocations and
mappings.

`device-pci.so` constructs a real Linux `pci_host_bridge` from the snapshot.
Its `pci_ops` forwards only the granted segment/bus/device/function to the
configuration interface. After enumeration, `virtio_pci.ko` is initialized and
PCI driver binding is the probe completion condition.

Execution order is Linux driver core and PCI initializers, dependency modules,
PCI enumeration, `virtio_pci.ko`, then `virtio-gpu.ko`. Shutdown reverses the
two drivers, removes the root bus, then cleans up dependency modules.
