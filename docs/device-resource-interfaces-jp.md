# Device resource interface

## slot

GPU closureは`device-pci.so`用に次のrequired slotを予約します。

| slot | interface | resource rights | native handle |
|---:|---|---|---|
| 1 | `kobox2.pci-function` | device: command、map、DMA | PCI function capability |
| 2 | `kobox2.dma-domain` | device: command、DMA | domainとcontext capability |
| 3 | `kobox2.irq-endpoint` | notification: wait | 1〜2048個のnonblocking eventfd |

PCIとDMA slotはobjectを一つ持ちます。IRQ slotはsourceごとにobjectを一つ持ちます。object
identityはgrantのgenerationとobject IDです。PCI reset authorityはhost generation lifecycleが
所有します。

## PCI function

interfaceはPCI identity、align済み8／16／32 bit config access、上限付きBAR subrange mappingを
提供します。BAR mappingはgrantされたfunctionのprotectionとcache propertyを維持します。

LinuxはVFIO PCI device descriptorを受け取ります。conformance backendも同じoperation tableを
実装し、決定的なtestに使います。

## DMA domain

interfaceはaddress width、alignment、segment size、coherencyを返します。DMA allocationと
mapping、明示的なCPU／device synchronization、drain accountingを所有します。mappingは
allocationをborrowし、先にreleaseします。device addressはgrant generationのIOMMU domain内で
のみ有効です。

Linuxはdomain FDとsealed context memfdを受け取ります。contextはiommufd IOAS、VFIO
container、conformance backendを選択し、backend object IDを保持します。この組が同じoperation
tableを実装します。

## IRQ endpoint

各interface objectは一つのhandlerをgeneration-scoped sourceへbindします。core runtimeへ
登録したprovider threadがeventfd counterをdrainし、そのdispatch contextでhandlerを同期実行します。
disableはadmissionを閉じ、実行中handlerの終了を待ちます。

## probe gate

`device-pci.so`は三つの正確なschema digestをbindし、generationとobject IDを検証します。active中
のみimmutable resource snapshotを公開します。Linux PCI enumerationと`virtio_pci.ko` probeは
このgateの後に開始します。quiesceはIRQ deliveryを停止し、live DMA allocationとmappingがzeroで
あることを要求します。

`device-pci.so`はsnapshotから実Linux `pci_host_bridge`を構成します。`pci_ops`はgrantされた一つの
segment／bus／device／functionだけをconfig interfaceへ転送します。enumeration後に
`virtio_pci.ko`をinitし、PCI driver bindingをprobe完了条件とします。

実行順はLinux driver coreとPCI initializer、依存module、PCI enumeration、`virtio_pci.ko`、
`virtio-gpu.ko`です。終了順は`virtio-gpu.ko`、`virtio_pci.ko`、root bus removal、依存moduleです。
