# Synchronization

## 範囲

Synchronizationはcore runtimeのinterface 3であり、memoryとCPUに依存します。
object操作はspin、mutex、rwlock、semaphore、event、completionです。atomic、
bitmap、refcount、barrierはcompiler inline primitiveであり、bindingを必要と
しません。

## Inline primitive

`atomic32`、`atomic64`、`refcount`、`bitmap_word`は固定LP64 layoutです。
read-onceとwrite-onceは自然alignmentの8、16、32、64-bit値を扱います。
bitmapのbit 0は最下位bitです。

loadはrelaxed、acquire、sequentially consistent、storeはrelaxed、release、
sequentially consistentを受理します。read-modify-writeとfenceは定義済みの
全orderを受理します。compare-exchangeのfailure orderはrelaxed、acquire、
sequentially consistentのいずれかで、success orderを超えません。不正な
orderはclosure faultです。

refcountの0取得には`increment_not_zero`を使います。underflow、0からの
increment、overflowは値を飽和させ、closureをfaultさせます。0へのdecrement
はacquire-release、その他のrefcount操作はrelaxed orderingです。

## 待機順序

waiterはobjectごとにFIFOで並びます。競合時はobject条件の成立、thread stop、
interrupt、deadline切れの順で結果を確定します。hostのspurious wakeは内部で再待機します。
deadlineはmonotonic clockの絶対nanosecondで、0は無期限です。try操作は利用
不能時にも`OK`を返し、結果をfalseにします。

blocking waitはthread contextかつpreemption、migration、local IRQ、
bottom-halfのnestingがすべて0の場合に限ります。providerは公開thread
interfaceより下位のprivate park/wake backendで待機を実装します。

## Lock

spin、mutex、rwlockは非再帰です。再取得、readからwriteへのupgrade、writeから
readへのdowngradeは`DEADLOCK`です。lockは取得threadだけが解放でき、別thread
からの解放は`OWNER`です。owner identityにはprocess lifetime内で単調増加する
thread IDを使い、再利用可能なhost thread addressからは生成しません。

spin保持中はpreemptionが無効で、sleepしません。mutexは単一threadが所有します。
rwlockはwriterで区切られたFIFO reader phaseを使います。次のwriterより前に
並んだ連続readerは同時に入り、writerより後ろのreaderは追い越しません。
read ownerはthreadごとに追跡します。

## Counterと通知

semaphore downはpermitを1つ消費します。upはFIFO waiterへ直接permitを渡し、
残りをstored countへ加えます。count 0のupは不正です。残りが設定上限を超える
場合はobjectを変更せず`EXHAUSTED`を返します。

auto-reset eventは1つのsignalを保持するか、1 waiterへ渡します。manual-reset
eventはsignaled状態を保持し、全waiterとresetまでの将来のwaiterを通します。

completionは個別の完了countを保持します。`complete`は1 waiterへ渡すかcountを
増加させます。`complete_all`はlatchを立て、現在と将来の全waiterを通します。
`reinit`はcountとlatchを消去し、waiterが存在する間は`BUSY`です。

## Lifetime

objectは生成bindingとgenerationに属します。destroyはownerが直列化し、lock保持中
またはwaiter存在中は`BUSY`です。全objectをdestroyするまでsync bindingはbusyです。

core quiesceは全synchronization objectをclosingにし、新規waitを拒否し、待機中の
waitを`CANCELED`で完了させます。cleanup用のunlock、signal、reset、reinit、destroy
は引き続き利用できます。全objectのdestroy後にcleanupが完了し、残存時はsandbox
processを終了してclosureを破棄します。
