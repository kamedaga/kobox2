# 共有virtqueue engine

`protocol/include/kobox2/virtqueue.h`と`protocol/src/virtqueue.c`は、controller側clientと
別processのGPL sandboxが共有するsplit-ring状態機械である。共有実装はすべてMITライセンス。
従来の`protocol/test/.../split_virtqueue`は引き続きテスト専用である。
wire schemaは変更しておらず、実装APIは番号を付けない`dev`のままとする。

## 境界と所有権

- engineはportable C11で、allocator・syscall・native handle・Linux構造体・暗黙のglobal状態を持たない。
- OS backendがmappingの寿命と、認証済みで不変なprivate channel/queue/registrationモデルを管理する。
  `kb2_protocol_channel_validate`で、再serializeせずモデルを検証できる。
- mapped-memory backendがtransport addressを解決し、local mapping権限とホスト承認済みの
  lane別領域を検査する。native/transport mappingの重なりを拒否する。
  同じ実体を異なる仮想アドレスへmapしたaliasは、ホスト側でも排除しなければならない。
  仮想アドレスの非重複だけでは、VMOやregistrationの実体の非重複を証明できない。
- `protocol/arch/x86_64/virtqueue_atomic.c`がaligned・lock-freeなu16 acquire/releaseと
  EVENT_IDX用の完全なstore-load barrierを実装する。mapped backendへ明示的に渡す。
  対象はCPU用のcoherent shared memoryで、hardware DMAやMMIOではない。
- endpointごとに独立したchannel/queue状態を持ち、各queueはlocalの単一ownerが操作する。
  concurrentに変更できるのはchannel fault flagだけである。faultは以後の受付を止めるが、
  進行中の操作を止めてからunmapする責任はホストにある。fault flagはcapability revokeではない。

各queueはcallerが所有するslot配列・descriptor-owner配列・private chain領域を借りる。
DRIVERがavailableをpublishしてusedをconsumeし、DEVICEがavailable chainをsnapshotしてusedをpublishする。
いずれも所有中のchainを変更・解放してはならない。

DRIVERの所有権は`take_used`と応答のcopy後に`release`で終わる。
DEVICEの所有権はusedのpublish成功後に終わる。publish後のEVENT_IDX読み出しも含め、
I/O失敗では所有権を保持したままchannelをfaultにする。新規要求として再publishしてはならない。
後始末はホストの責任であり、その場でqueueをresetするAPIは用意しない。

## 検証と通知

index・有限で循環しないchain・read/write順序・registration権限・lane別領域・ring metadataとの
非重複・使用中descriptorの所有権・書き込み領域の重複・usedの完了長を検査する。
indirect tableの未使用項目も含めてtable全体を所有する。
direct chainの末尾にindirect tableを接続する形も受け付け、indirect参照のWRITEと
NEXTがない項目のnext値は[Virtio 1.0](https://docs.oasis-open.org/virtio/virtio/v1.0/virtio-v1.0.html)に従い無視する。
nested indirectとINDIRECT＋NEXTは拒否する。

localの不正publishは共有メモリへ書く前にINVALIDを返す。peerの不正入力はlocal channel全体をfaultにする。
最初のbackend I/O失敗はIO、その後の操作はFAULTED、世代違いは共有メモリに触れずSTALEを返す。
copyが途中でI/O失敗した場合、出力は部分的に変更されうるため使用してはならない。

descriptorはprivate snapshotだが、payloadは引き続き信用しない。callerが要求全体をcopyし、
envelope・generation・correlation・command・session・spanを検証してからsubsystemを操作する。
GPU session・envelope検証・control faultの送信・通知endpointの生成・generation revoke/resetはengineの責務ではない。

publishはデータ書き込み→indexのrelease更新→完全barrier→peerのevent閾値の読み出しの順に行う。
`arm`はconsumerの閾値を更新し、完全barrier後にproducer indexを再確認する。
空であることを再確認してからだけsleepできる。通知の集約・重複・既読は許容し、ringの進捗を正とする。
used IDの順不同とu16 indexの周回に対応する。

重複検査にはchain長に対して二次の処理とqueue slotの走査がある。所有権の明確さを優先した実装で、
ホストは相応のresource上限を選ぶ必要がある。最大構成の性能を確認したという意味ではない。

## 検証

`KB2_BUILD_SANDBOX=OFF`・`KB2_ENABLE_SANITIZERS=ON`で、GCC/Clangとも
engine/process試験を含むCTest全7件がPASS。

`virtqueue_engine_test.c`はSG copy、direct/indirect/混在chain、未使用table項目の所有権、
不正chain/完了、snapshot後のpeer変更、書き込み前のlocal拒否、所有権保持、channel全体のfault、
publish前後のcallback失敗、EVENT_IDX待機開始競合、65,536でのindex周回を確認する。

`virtqueue_process_test.c`は別process・異なる仮想mapping・memfd/eventfdと実x86_64 backendを使用する。
新規2世代それぞれ68,000要求を交換し、direct/indirect/混在chain、逆順完了、通知集約/重複、周回を確認する。
これはsoftware transportの検証であり、GPU reset・native capability revoke・client死亡後の復帰の証明ではない。
