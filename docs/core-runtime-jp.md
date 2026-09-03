# Core runtime

## identity

core runtimeは一つの`dev` ABI familyです。identityと全operation catalogは
`protocol/schema/core_runtime.json`をcanonical化した内容のSHA-256です。

`core_operations`は`bind`と`unbind`を持つdirectoryです。`bind`はmodule context、interface ID、
正確なfamily digestを受け取り、typed operation tableとopaque binding objectを返します。各interface
callはそのbinding objectを受け取ります。

## interface

| ID | interface | direct dependency | operation |
| --- | --- | --- | --- |
| 1 | memory | — | page、aligned allocation、reallocation、cache、statistics |
| 2 | CPU | memory | topology、execution context、preemption、migration、local IRQ・BH state、per-CPU storage |
| 3 | synchronization | memory、CPU | atomic、refcount、bitmap、barrier、spin、mutex、rwlock、semaphore、event、completion |
| 4 | thread | memory、CPU、synchronization | current、create、join、detach、stop、interrupt、park、wake、affinity、priority |
| 5 | time | synchronization、thread | clock、sleep、busy delay、one-shot・periodic timer |
| 6 | workqueue | memory、CPU、synchronization、thread、time | queue、work、delayed submit・reschedule、cancel、flush |
| 7 | RCU | memory、CPU、synchronization、thread、time、workqueue | classic RCU、SRCU、callback、grace period、barrier |

schemaは全operation ID、input、output、flag、blocking属性、callback context、statusを固定します。
absolute wait deadlineはmonotonic clockを使い、zeroはinfinite waitを表します。

## semantics

page allocationは4096 byte pageとorderを使います。byte・cache allocationのalignmentは2の累乗です。
reallocation failure時は元のallocationが残ります。atomic memory requestはsleepせず完了します。

CPU IDはclosure generation内でdenseかつstableです。preemption、migration、local IRQ、bottom-half
stateはthreadごとにbalancedです。IRQ deliveryはlogical CPU maskに従います。spin operationはsleep
しません。mutex、rwlock、semaphore、event、completionはtry、interruptible、absolute-deadline waitを
持ちます。spin ownership中はpreemptionをdisableし、lockは取得threadがreleaseします。eventは
auto-reset・manual-resetを持ち、completionはreinitializationまで維持する`complete_all` latchを備えた
counting objectです。
atomic32、atomic64、refcount、bitmap、memory barrier primitiveは固定LP64 layoutと明示的なmemory
orderingを持ちます。refcountはoverflow時にsaturateしてfaultとなり、zeroからの取得には
`increment_not_zero`を使います。
synchronization waiterはFIFOで入り、rwlockはwriterで区切られたreader phaseを形成します。
interruptやdeadlineとの競合ではobject条件の成立を優先します。core quiesceはprivate
park/wake backendを通じて待機中のwaitをcancelします。

thread stopとinterruptは独立したsticky stateです。unparkは1個にcoalesceされるpermitを持ち、wakeは
現在のparkだけを対象にします。core quiesceはstop要求、park中threadのinterruptとwake、tracked
threadのjoinを行います。完全な契約は[thread-interface-jp.md](./thread-interface-jp.md)で定義します。
timer synchronous cancelはcallback終了後に完了し、callbackはtimerごとにserializeされます。
完全な契約は[time-interface-jp.md](./time-interface-jp.md)で定義します。
work callbackはworkごとにserializeされ、flushはflush epoch以前のsubmissionを対象にします。
完全な契約は[workqueue-interface-jp.md](./workqueue-interface-jp.md)で定義します。
RCU grace periodはreaderをsnapshotし、callback barrierはcallback registration epochを
snapshotします。完全な契約は[rcu-interface-jp.md](./rcu-interface-jp.md)で定義します。

## ownershipとlifetime

bindingは一つのmodule nodeとgenerationに属します。生成したobjectと登録したcallbackはbindingが
所有します。`thread.current`、`rcu.default_domain`、`cpu.percpu_address`はborrowed objectを返します。
共有利用にはclosureが所有する明示的なshared objectを使います。

各nodeはdependency順にinterfaceをbindし、逆dependency順にunbindします。

正常なmodule cleanupは全owned objectをreleaseしてからunbindします。operation tableはcore cleanupまで
有効です。CPU topologyはgeneration内で維持されます。

## lifecycleとfault

initはinterface ID順、quiesceとcleanupは逆順です。全binding、thread、timer、work、callback、
synchronization object、cache、allocationの解放後にcore cleanupが完了します。

部分的なinit failureは初期化済みinterfaceを逆順にrollbackします。ownership違反、stale callback、
allocator corruptionはclosure faultになります。quiesce failureまたはbusy cleanupはsandbox processの終了と
resource revokeで完了します。
