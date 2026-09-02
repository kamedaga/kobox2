# ring transport

このtransportは番号なしの`dev` interfaceに含まれます。明示的なABI freezeまでは、
すべてのpeerが同じschema digestを使います。

byte layoutの正本は`protocol/schema/transport.json`で、生成済み定数は
`protocol/generated/include/kobox2/protocol_layout.h`です。固定sizeはchannel headerが
96 byte、queue descriptorが64 byte、region descriptorが32 byte、message envelopeが
40 byteです。

## model

kobox2はVirtio 1.0 little-endian split virtqueue layoutを使います。各channelは一つの
event virtqueueと一つ以上のrequest virtqueueを持ちます。各available ringとused ringの
producerはそれぞれ一つです。

request chainはreadableなrequest bufferとwritableなresponse bufferを持ちます。event
queueはclientがwritable bufferを供給し、sandboxが完了させます。control channelとrole
data channelは同じtransportを使い、protocol IDで分離します。hardware device queueは
sandboxとdevice interfaceが所有します。

## queue profile

- `VIRTIO_RING_F_INDIRECT_DESC`と`VIRTIO_RING_F_EVENT_IDX`を必須とします。
- queue sizeは16から32768までのpower of twoです。
- directおよびindirect scatter-gather chainを扱います。
- used elementのdescriptor IDにより任意順のcompletionを対応付けます。
- queue size、最大chain長、最大indirect table長、最大outstanding request数をchannel
  descriptorで固定します。

channel descriptorはdescriptor size、ABI identity、schema digest、feature bit、channel
ID、sandbox generation、queue定義、notification endpoint、transport-address regionを
持ちます。ABI identityは番号ではなく4 byteの`dev\0`です。schema digestはschemaの
RFC 8785表現のSHA-256で、reserved fieldはzeroです。一致しなければchannel確立を拒否します。

## addressing

転送された各shared-memory capabilityへ、重複のない64-bit transport-address区間を
割り当てます。各descriptor範囲は対応するaccess rightを持つ一つの登録区間内で
完結します。registrationはregion ID、transport base、length、right、generationを
持ちます。

IPC virtqueue memoryはhardware device DMA address space外のshared VM objectに配置し、
host portがregistrationとnative shared-memory capabilityを対応付けます。

## message envelope

すべてのrequest、response、eventは次のlittle-endian envelopeで始まります。

| field | type |
|---|---|
| protocol ID | `u32` |
| ABI identity (`dev\0`) | `u8[4]` |
| opcode | `u32` |
| flags | `u32` |
| generation | `u64` |
| correlation ID | `u64` |
| payload length | `u32` |
| reserved | `u32` |

responseはrequestのcorrelation IDを引き継ぎ、unsolicited eventではzeroを使います。
used lengthにはwritable descriptorへ書いたbyte数を記録します。

## operation

- available側はdescriptor書き込み、release barrier、available index更新の順でpublishし、
  consumerはacquire barrier後に読みます。
- used側はused element書き込み、release barrier、used index更新の順でpublishし、
  clientはacquire barrier後にresponseを読みます。
- `EVENT_IDX`が集約可能なwake notificationを制御し、ring indexを進捗状態の正本とします。
- consumerはdescriptor index、chain構造、length、address範囲、right、generation、message
  size、ABI identity、flags、reserved fieldを検証します。検証failureではchannelを
  `FAULTED`へ移し、control faultを発行します。
- 各sandbox generationへ新しいvirtqueue memory、channel ID、address registration、
  notification endpointを割り当てます。restartはcapability revoke、device reset、
  process終了、割り当て、転送、control handshakeの順です。
- controllerはmanagement channelを保持し、channel確立完了時にdata channelの所有権を
  clientとsandboxへ移します。
