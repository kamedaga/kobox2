# Thread interface

## 範囲

Threadはcore runtimeのinterface 4であり、memory、CPU、synchronizationに
依存します。native concurrent execution、lifecycle制御、cooperative stop、
interrupt、park、name、logical CPU affinity、priorityを提供します。

## Handle state

生成threadは次の状態を取ります。

```text
STARTING -> RUNNING -> EXITED -> JOINED
                    \-> DETACHED
```

`create`はnative thread生成、name、affinity、priority、entry gateの確立後に
返ります。`START_PARKED`はunpark permitを消費するまでentryをgateで止めます。
entryの返り値がjoinのexit statusです。

joinable threadは1回の成功したjoinでreapされます。timeoutしたjoinはjoinable
状態を維持します。joinとdetachは排他的で、ownerがそのhandleの全操作と直列化
します。self joinは`DEADLOCK`です。detachはreapをcoreへ移し、entryが返るまで
bindingをbusyに保ちます。

`current`はprovider管理下にある呼出threadのborrowed handleを返します。生成した
handleは生成bindingとgenerationに属します。

## Stop、interrupt、park

stopとinterruptは独立したsticky bitです。`request_stop`はstopを設定してparkまたは
interruptible blocking pointをwakeします。targetは`stop_requested`で確認します。`interrupt`
はinterruptを設定し、interruptibleなsync、time、park waitをwakeします。
interrupt bitをclearできるのはtarget自身だけです。

`park`はself operationです。stop、interrupt、unpark permit、pending wake、
monotonic absolute deadlineの順に解決します。stopは`CANCELED`、interruptは
`INTERRUPTED`、期限切れは`TIMED_OUT`です。

`unpark`は1個にcoalesceされるpermitを保持し、現在のparkをwakeします。次のpark
がpermitを消費します。`wake`は進行中のparkだけを完了させ、保持しません。
native spurious wakeは内部で再待機します。

parkのprivate execution recordはinterruptible synchronization waitからも登録
します。結果はpredicate、stop、interrupt、deadlineの順で確定します。これにより
公開依存方向をthreadからsynchronizationのまま保ち、lost wakeなしでinterruptとstopを
配送します。

## Scheduling attribute

nameはcoreがcopyし、最大63 byteです。短いnative diagnostic nameへ反映する場合もあります。
stack size 0はprovider defaultを選びます。
明示stackは64 KiB以上かつ4096 byteの倍数です。

CPU maskはdense logical CPU IDで表します。null maskとword count 0は全online
logical CPUを選びます。明示maskはgenerationの正確なCPU word数を持ち、1 CPU
以上を含み、範囲外bitを含みません。affinityはentry前に設定され、後からatomicに
置換できます。

priorityは0から39で、0が最高、20がdefault、39が最低です。`HIGH_PRIORITY`は
生成時にpriority 0を選びます。priorityを上げる操作にはprovider権限が必要です。

## Exitとquiesce

threadはCPU nestingをbalanceし、spin、mutex、rwlockを所有せずに終了します。
違反はclosure faultです。

thread quiesceはcreateを拒否し、全生成threadへstop、interrupt、wakeを送り、
全entryの終了を待ってhandleをreapします。cleanupはsynchronization利用者のdrain
後にprovider thread recordを解放します。終了に協力しないthreadが残る場合、
closure teardownはsandbox processを終了します。
