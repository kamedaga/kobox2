# RCUインターフェース

## 範囲

RCUはcore runtime interface 7です。memory、CPU、synchronization、thread、time、
workqueueに依存し、classic RCUとSRCU domain、read-side token、grace period、
deferred callback、callback barrierを提供します。

## Domain

`default_domain`はcore generationのborrowed classic domainを返します。
`domain_create`は呼出bindingが所有するclassicまたはSRCU domainを作ります。作成
domainはownerだけに可視です。domain destroyは受付を閉じ、readerとcallbackを待ち、
domainを解放します。default domainはcore cleanupが解放します。ownerはdestroy開始と
新規domain operationを直列化し、保持済みtokenのreleaseはdrain中も有効です。

classic read-side sectionはnonblockingで、nest、preempt、migrateできます。SRCU
read-side sectionはblockもできます。read tokenは一意でbindingとdomainに属し、取得
execution threadが一度だけ解放します。nestしたtokenの解放順は任意です。

`read_lock`は保護load前のacquire ordering、`read_unlock`は保護access後のrelease
orderingを確立します。完了したgrace periodは対象readerとのfull ordering boundaryを
提供します。

## Grace period

受付済みreaderにはdomain read sequenceを付けます。`synchronize`はsequenceを
snapshotし、対象readerだけを待ちます。後続readerはgrace periodを延長しません。
同じexecution threadが対象tokenを保持した状態で呼ぶと`DEADLOCK`です。

`EXPEDITED`はproviderの最低latency grace-period経路を要求し、完了条件とorderingは
同一です。normal経路がreaderを厳密追跡するproviderは両形式に同じ経路を使えます。

`quiescent_state`はclassic domainのquiescent pointを記録し、grace-period進行を起こし
ます。callerはそのdomainのtokenを保持していない必要があります。SRCUのquiescent
eventは明示token解放です。

## Callbackとbarrier

`call`はread sequenceをsnapshotし、callbackをdomain FIFOへ追加してregistration後に
返ります。callbackは対象readerのtoken解放後、provider worker thread contextで開始
します。callback完了はrelease orderingを持ち、同一domainのcallbackはregistration順に
開始します。callbackはnonblockingです。RCU callbackからblocking RCU operationを
呼ぶと`DEADLOCK`になり、blocking workはworkqueueへsubmitします。

`barrier`はdomain callback sequenceをsnapshotし、対象callbackの完了まで待ちます。
後続callbackは対象外です。自身の完了が対象となるcallbackは`DEADLOCK`です。callerが
保持する対象readerは`synchronize`と同じ規則で扱います。

## Quiesce

RCU quiesceはreader、callback、domainの受付を閉じ、受付済みgrace periodとcallbackを
すべて完了してcallback workerを停止します。所有中のidle domainはcleanup中に破棄
できます。
