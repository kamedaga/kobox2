# Workqueueインターフェース

## 範囲

Workqueueはcore runtime interface 6です。memory、CPU、synchronization、thread、
timeに依存し、所有権を持つexecution queue、再利用可能なwork、絶対時刻delayed
submission、cancel、epoch flushを提供します。

## Queueと配置

bound queueはlogical CPUごとにexecution domainを持ちます。明示CPU IDはその
domainを選択し、`CPU_ANY`はsubmitしたCPUを取得します。`UNBOUND`はclosure全体の
domainを一つ作り、`CPU_ANY`だけを受け付けます。

`maximum_active`はdomainごとのnormal callback同時実行数を制限します。zeroは
provider管理の正数limitを選びます。`ORDERED`は`UNBOUND`と明示limit 1を要求し、
`MEMORY_RECLAIM`とは併用せず、eligible callbackをready遷移順に開始します。

`HIGH_PRIORITY`はpriority zeroの独立worker poolを使います。`MEMORY_RECLAIM`は
normal active limit外のforward-progress予約workerをdomainごとに一つ加えます。
`FREEZABLE`はcore quiesce時にpending workをcancelし、それ以外のqueueは受付済み
workをdrainします。

queue nameはcoreがcopyし、最大63 byteです。workerはnative provider threadです。
callbackは選択domainのthread contextで実行し、`thread.current`はborrowed worker
handleを返します。

## Work stateとsubmission

workは作成bindingに属し、次の状態を取ります。

```text
IDLE -> READY | DELAYED -> RUNNING -> IDLE
                         \-> RUNNING + successor -> READY | DELAYED
```

実行中callbackと一つのpending successorは共存できます。callbackはworkごとに
直列化します。`submit`はimmediate successorを作ります。`submit_at`はmonotonic
絶対deadlineを使い、zeroまたは到達済みはimmediateです。successorが既にpending
なら両operationはfalseを返します。

`reschedule_at`はsuccessorがなければdelayed successorを作りfalseを返します。
存在する場合はdeadlineとCPU配置をatomicに置換し、submission epochを維持して
trueを返します。pendingまたはrunning workは一つのqueueに属し、別queueは
`BUSY`です。

## Cancelとflush

`cancel`はpending successorを除去し、存在したかを返します。実行中callbackは
継続します。`cancel_sync`はそのcallback終了も待ちます。待機対象callback自身から
synchronous cancel、work destroy、`flush_work`、`flush_queue`を呼ぶと
`DEADLOCK`です。

受付済みsuccessorにはwork sequenceとqueue submission epochを付けます。
`flush_work`はwork sequence、`flush_queue`はqueue epochをsnapshotし、対象callback
完了まで待ちます。後続submissionは対象外です。rescheduleは対象epochを維持する
ため、既存flushから外れません。

`is_pending`はsuccessor状態を返し、callbackだけが実行中の状態を含みません。
work destroyはsuccessorをcancelし、callbackをdrainしてhandleを解放します。
queue destroyは全受付済みitemをdrainし、execution domainを停止してqueueを解放
します。ownerはdestroyとhandle利用を直列化します。

## Quiesce

workqueue quiesceはcreateとsubmissionを拒否します。freezable queueのpending work
をcancelし、他の受付済みworkは指定deadlineでeligibleとなり正常にdrainします。
全callback完了後に全workerとdispatcherを停止し、所有中のidle handleはcleanup用に
維持します。
