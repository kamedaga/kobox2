# Linux provider runtime

## provider graph

Linux closureは三つのshared providerを使います。

```text
core/primitive.so -> device-pci.so -> drm.so
```

`device-pci.so`は`core/primitive.so`に依存します。`drm.so`は両方に依存します。各providerは
`dev` identityと明示的なinit、quiesce、cleanup entryを持つ一つのclosure nodeです。

## lifecycle

provider stateは次の通りです。

```text
BOUND -> INITIALIZING -> ACTIVE -> QUIESCING -> QUIESCED -> CLEANING -> CLEAN
```

loaderはdependency順にproviderをinitし、逆順にquiesce・cleanupします。すべてのentryへ同じimmutable
module contextを渡します。init entryはfailureを返す前に自身の部分状態をrollbackします。

各providerは保持対象のLinux initializerを並べたgenerated init tableを所有します。entry順はLinux init
level、link済みsection順、symbol名で決定します。table digestはprovider inventoryに含めます。

## core memory arena

`core/primitive.so`は`READ`、`WRITE`、`MAP` rightsを持つ一つのclosure-shared memory resourceを
slot ID `1`で受け取ります。interface bindingはcore cleanupまで有効なpage-aligned mapped rangeを
返します。

arenaのpage sizeは4096 byteです。allocator metadataは先頭のaligned prefixを使い、残る完全なpageを
buddy allocatorにします。allocationとreleaseはpage orderを使い、provider thread間で安全に動作し、
正確なfree page数とallocated page数を保持します。allocation headとorderを記録し、release時にownership
を検証してbuddyをcoalesceします。arenaの破棄は全allocated pageが返却された後に完了します。

grantとmapping契約は[memory-arena-interface-jp.md](./memory-arena-interface-jp.md)で定義します。
core service全体の契約は[core-runtime-jp.md](./core-runtime-jp.md)で定義します。

core initはLinuxのpage、slab、per-CPU、thread、time、lock、workqueue、RCU初期化より先にarenaを確立
します。core cleanupはdependent providerとmoduleのcleanup後に利用者をdrainしてarena stateを解放します。

RCUは明示read tokenをsequenceで追跡します。一つのunbound provider workerがdomain FIFOごとに
eligible callbackを実行し、callback owner nodeのborrowed thread handleを公開します。eventfd通知で
pollingせずgrace-periodとcallback進行を起こします。

## logical CPU

closure loaderは0以外のlogical CPU countを全moduleのimmutable contextへ固定します。IDはzeroから
denseでgeneration中はstableとなり、すべてpossibleかつonlineです。loader threadはlogical CPU zeroで
開始し、provider threadは割り当てられたlogical CPU IDを保持します。

preemption、migration、local IRQ、bottom-halfのnestingはthread-localかつbinding単位です。nestingが
残る間、bindingはbusyです。per-CPU allocationはlogical CPUごとに独立してalignされたobjectを持ち、
生成したCPU bindingが所有します。

## synchronization

synchronization interfaceはCPU stateの後に初期化します。object stateとFIFO waiter queueはcore arena
に置き、spin ownershipはCPU preemption counterを使います。sleep可能なobjectはabsolute monotonic
deadlineを持つprivate x86-64 Linux futex syscallで待機し、公開thread interfaceへの依存やlibcの
synchronization importを追加しません。

quiesceは全objectをcloseし、queued waiterを`CANCELED`で起床します。その後のcleanupではarena解放前に
moduleが全objectをdestroyしていることを要求します。完全な契約は
[synchronization-jp.md](./synchronization-jp.md)で定義します。

## Thread

thread interfaceはnative Linux process threadを使います。生成時のstart handshake
により、module code実行前にname、native affinity、priority、TLS execution identity、
optional parked entry gateを確立します。logical CPU maskはclosure generationで取得
したprocess CPU setへ対応付けます。

private futex registrationによりthreadのinterruptとstop stateをparkおよび
interruptible synchronization waitへ接続します。quiesceは両stateを送り、全blocking
pointをwakeし、全生成threadをreapします。完全な契約は
[thread-interface-jp.md](./thread-interface-jp.md)で定義します。

## Timeとtimer

Linuxは3種のcore clockをnative clock IDへ直接対応付けます。絶対時刻sleepと
timer schedulingはrealtime調整とboottimeのsuspend semanticsを維持します。
timer dispatchはlogical CPUごとに分割し、atomic callbackはsoftirq context、
thread callbackはprovider管理native threadで実行します。完全な契約は
[time-interface-jp.md](./time-interface-jp.md)で定義します。

## Workqueue

Linux workqueueはqueue所有のnative execution domainを使います。bound domainは
logical CPUへaffinityし、unbound domainはclosure CPU setを使います。monotonic
dispatcherがdelayed workをreadyへ移し、native workerがactive limit、ordered
execution、reclaim capacityを保証します。完全な契約は
[workqueue-interface-jp.md](./workqueue-interface-jp.md)で定義します。

## provider初期化

`device-pci.so`はcore active後に保持対象のIRQ、PCI、IOMMU entryを初期化します。`drm.so`はcoreと
device-pci active後に保持対象のdma-buf、video entryを初期化します。root moduleは全provider active後に
initします。

`READY`には全providerとroot moduleのactive到達が必要です。quiesce完了にはclosureが所有するprovider
work、callback、referenceのdrainが必要です。
