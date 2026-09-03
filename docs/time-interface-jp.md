# Timeインターフェース

## 範囲

Timeはcore runtime interface 5です。synchronizationとthreadに依存し、3種の
clock、絶対時刻sleep、busy delay、所有権を持つone-shot／periodic timerを提供
します。

## Clockとdeadline

`MONOTONIC`はsystem稼働中に進みます。`BOOTTIME`はsuspend期間も含みます。
`REALTIME`は調整可能なwall clockです。値はunsigned nanosecondで、overflowは
`EXHAUSTED`です。

sleep、busy delay、timerのdeadlineは選択したclockの絶対時刻です。deadlineが
zero、または到達済みなら即時完了します。realtime deadlineはclock調整に追従
し、前進で期限到達、後退で延期されます。

`sleep_until`はthread contextのwaitです。`INTERRUPTIBLE`ではstopを
`CANCELED`、interruptを`INTERRUPTED`として解決し、同時競合ではdeadline到達
を優先します。core quiesceは全sleepをcancelします。`busy_wait_until`はcaller
を実行状態に保ち、atomic contextで利用できます。

## Timer配置とcallback context

timerは作成bindingとgenerationに属します。`PINNED`は作成時のlogical CPUを
固定します。unpinned timerはarmごとにonline logical CPUへ配置されます。

`ATOMIC` callbackは割当CPUのsoftirq contextで実行し、nonblocking operation
だけを使います。`THREAD` callbackは同CPUのprovider管理thread contextで実行
します。thread callbackでは`thread.current`がborrowed handleを返します。
callbackはtimerごとに直列化されます。

deferrable timer単独では次のCPU wakeを設定しません。期限到達後、割当CPU上の
次のprovider scheduling activityで実行します。他のtimerのdeadline動作は維持
されます。

## Armとperiodic delivery

`timer_arm`はpending armとqueued deliveryを新しいarm generationでatomicに
置換します。period zeroはone-shotです。periodic timerは指定した絶対deadline
からphaseを維持します。

`expiration_count`は1回のcallbackが表すexpiration数です。dispatch前または
callback実行中に蓄積したexpirationは集約します。`UINT64_MAX`を超える個数は、
expirationを失わず複数callbackに分けます。

実行中callbackからのarm／cancelは後続deliveryを制御し、現在のcallbackは正常
終了します。置換前generationのexpirationが新generationへ渡ることはありません。

## Cancel、destroy、quiesce

`timer_cancel`は待機せずpending armとqueued deliveryを除去します。どちらかが
存在した場合、結果はtrueです。実行中callbackは継続します。
`timer_cancel_sync`は同じcancel後、そのcallback終了を待ちます。同じtimerの
callbackから同期cancelまたはdestroyを呼ぶと`DEADLOCK`です。

`timer_remaining`はpending armがなければzero、それ以外は次のdeadlineまでを
zero下限で返します。`timer_is_pending`はarmed deadlineまたはqueued deliveryを
含み、実行中callbackだけの状態は含みません。

destroyはtimerを同期drainしてhandleを解放します。ownerはdestroyと他の全handle
operationを直列化します。time quiesceは新しいsleep、create、armを拒否し、
sleeperとqueued timerをcancelした後、実行中callbackとprovider execution thread
の終了を待ちます。
