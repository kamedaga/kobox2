# GPU protocol

## identity

GPU data planeは一つのtransport channel上で`kobox2.gpu` protocolを使います。明示的なABI
freezeまではidentityを`dev`とします。各profileはbase schema digest、command-set schema
digest、closure digest、resource interface、limitを一つの不変な単位として固定します。
各launchはそのprofileをgeneration固有のresource-grant digestへbindします。

VirGLとAMDGPUは同じbase protocolを使います。Linux DRM operationの詳細はschemaで選択する
command setが表現します。

| profile | command set |
|---|---|
| VirGL | DRM core、DRM mode、DRM virtgpu |
| AMDGPU | DRM core、DRM mode、DRM amdgpu |

base protocolはsession、dispatch、span、native object交換、completion、notification、fault、
ordering、generationを所有します。command-set schemaは各DRM operationのcanonicalかつ
pointer-freeな表現を所有します。

## 境界

| component | authority |
|---|---|
| PachaOS VFS/personality bridge | DRM namespace、file descriptor semantics、UAPI copyとcanonical serialization |
| `gpud` | client policy、session routing、exchange broker、kobox2 controller、GPU profile |
| GPU sandbox | `drm_file`、GEM/TTM、context、VM、sync object、KMS state、driver state、hardware queue |
| kobox2 controller | sandbox generation、closure、resource grant、restart lifecycle |

一つのopen file descriptionを一つのGPU sessionへ対応付けます。`dup`と`fork`は同じsessionを
retainし、独立した`open`は新しいsessionを作ります。host側の最後の参照がsessionをcloseします。
primary nodeとrender nodeは別のsession typeです。

Linux ioctl number、native C layout、pointer、host handleはLinux personality境界で終端します。
`gpud`は検証済みのcanonical messageをrouteします。GPL sandboxが固定したLinux v6.18.48
UAPI operationを復元し、Linux semanticsを所有します。

## profileとcommand catalog

GPU profileは次を持ちます。

- driver identityとprimary/render nodeのavailability
- base GPU schema digest
- 各setのschema digestを持つ順序付きcommand-set catalog
- 各setのschema digestを持つ順序付きevent-set catalog
- queue数とすべてのbyte、object、request、time limit
- 正確なclosure digestとresource interface digest

各catalog entryはnonzeroのset IDを割り当てます。各command entryはcommand ID、queue class、
request/response上限、span role、attachment role、同期規則、cancel規則を固定します。未知のset、
command、schema digestは副作用より前に拒否します。

DRM core setはversion/capability query、client capability、GEM close、PRIME/dma-buf交換、
DRM syncobj/timeline operation family全体を含みます。DRM mode setはprimary-node authority、
master/lease state、resource、connector、encoder、CRTC、plane、framebuffer、property、blob、
atomic commit、page flip、vblank、sequence eventを含みます。

DRM virtgpu setは固定した`virtgpu_drm.h`の全operationであるmap、execbuffer、getparam、
resource create/info、両方向の3D transfer、wait、capset query、blob creation、context
initializationを含みます。

DRM amdgpu setは固定した`amdgpu_drm.h`の全operationを含みます。対象はGEM
create/map/metadata/VA、context、BO list、command submission、device/firmware query、wait、
VM control、fence、scheduling、user queueです。nested CS chunkとquery outputは個別の上限付き
schema recordで表現します。

## DRM core command契約

| ID | command | canonical契約 |
|---:|---|---|
| 1 | `VERSION` | 3個のoutput byte span、versionと返却length |
| 2 | `GET_UNIQUE` | output byte span、返却/必要length |
| 3 | `GET_CLIENT` | client index、固定幅client record |
| 4 | `GET_STATS` | output stat-entry span、返却count |
| 5 | `SET_VERSION` | 双方向の4個のsigned version component |
| 6 | `GET_CAP` | capability ID、64-bit value |
| 7 | `SET_CLIENT_CAP` | capability IDと64-bit value |
| 8 | `SET_CLIENT_NAME` | 上限付きinput byte span |
| 9 | `GEM_CLOSE` | session-local GEM handle |
| 10 | `GEM_FLINK` | GEM handle、global name |
| 11 | `GEM_OPEN` | global name、GEM handleとsize |
| 12 | `GEM_CHANGE_HANDLE` | session-localな旧handleと新handle |
| 13 | `PRIME_HANDLE_TO_FD` | GEM handle、moveするdma-buf attachment |
| 14 | `PRIME_FD_TO_HANDLE` | shareするdma-buf attachment、GEM handle |
| 15 | `SYNCOBJ_CREATE` | creation flag、syncobj handle |
| 16 | `SYNCOBJ_DESTROY` | syncobj handle |
| 17 | `SYNCOBJ_HANDLE_TO_FD` | handle、flag、point、moveするsyncobj/sync-file attachment |
| 18 | `SYNCOBJ_FD_TO_HANDLE` | shareするsyncobj/sync-file attachment、syncobj handle |
| 19 | `SYNCOBJ_WAIT` | handle span、flag、absolute deadline、最初のsignaled index |
| 20 | `SYNCOBJ_RESET` | handle span |
| 21 | `SYNCOBJ_SIGNAL` | handle span |
| 22 | `SYNCOBJ_TIMELINE_WAIT` | handle/point span、absolute deadline、最初のsignaled index |
| 23 | `SYNCOBJ_QUERY` | in/out handle/point span、返却count |
| 24 | `SYNCOBJ_TRANSFER` | source/destination handleとpoint |
| 25 | `SYNCOBJ_TIMELINE_SIGNAL` | handle/point span |
| 26 | `SYNCOBJ_EVENTFD` | handle/pointとshareするevent attachment |

## DRM mode command契約

objectを持つrequestはtopology epochを持ち、query completionは返すobject IDを支配するepochを
返します。

| ID | command | canonical契約 |
|---:|---|---|
| 1–4 | `GET_MAGIC`, `AUTH_MAGIC`, `SET_MASTER`, `DROP_MASTER` | primary-session authorityとmagic value |
| 5–7 | `WAIT_VBLANK`, `CRTC_GET_SEQUENCE`, `CRTC_QUEUE_SEQUENCE` | CRTC identity、sequence、timestamp、event token、必要なdeadline |
| 8 | `GETRESOURCES` | 上限付きframebuffer/CRTC/connector/encoder ID output span |
| 9–10 | `GETCRTC`, `SETCRTC` | CRTC recordとtyped mode/connector span |
| 11, 35 | `CURSOR`, `CURSOR2` | CRTC、GEM handle、geometry、hotspot |
| 12–13 | `GETGAMMA`, `SETGAMMA` | 同じ長さのred/green/blue `u16` span |
| 14–15 | `GETENCODER`, `GETCONNECTOR` | object metadataとtyped mode/property/encoder span |
| 16–17 | `ATTACHMODE`, `DETACHMODE` | connectorと1個のmode record |
| 18–20 | `GETPROPERTY`, `SETPROPERTY`, `GETPROPBLOB` | typed value/enumと上限付きblob byte |
| 21–25 | `GETFB`, `ADDFB`, `RMFB`, `PAGE_FLIP`, `DIRTYFB` | framebuffer record、event token、rectangle span |
| 26–28 | `CREATE_DUMB`, `MAP_DUMB`, `DESTROY_DUMB` | GEM allocationとmemory mapping attachment |
| 29–31 | `GETPLANERESOURCES`, `GETPLANE`, `SETPLANE` | plane ID、format、source/destination geometry |
| 32, 43–44 | `ADDFB2`, `GETFB2`, `CLOSEFB` | 4-planeのformat/pitch/offset/modifier record |
| 33–34 | `OBJ_GETPROPERTIES`, `OBJ_SETPROPERTY` | object typeとtyped property/value record |
| 36 | `ATOMIC` | object index span、typed property span、output syncobj span |
| 37–38 | `CREATEPROPBLOB`, `DESTROYPROPBLOB` | 上限付きinput byteとblob object ID |
| 39–42 | `CREATE_LEASE`, `LIST_LESSEES`, `GET_LEASE`, `REVOKE_LEASE` | object-ID span、lessee ID、moveするsession attachment |

atomic property valueはscalar、object、blob、input-syncobj、output-syncobjから形式を選びます。
adapterはsyncobj形式をLinux fence-FD propertyへ変換します。返却fenceは宣言したoutput spanの
session syncobj handleです。

## DRM virtgpu command契約

| ID | command | canonical契約 |
|---:|---|---|
| 1 | `MAP` | GEM handleとrights、mapping recordとmemory attachment |
| 2 | `EXECBUFFER` | command byte、BO handle、input/output syncobj span、optional sync-file attachment |
| 3 | `GETPARAM` | parameter ID、64-bit value |
| 4 | `RESOURCE_CREATE` | 3D resource geometry、format、bind、backing field、BO/resource handle |
| 5 | `RESOURCE_INFO` | BO handle、resource size、blob-memory class |
| 6–7 | `TRANSFER_FROM_HOST`, `TRANSFER_TO_HOST` | BO handle、3D box、level、offset、stride |
| 8 | `WAIT` | BO handle、flag、absolute deadline |
| 9 | `GET_CAPS` | capset ID/version、上限付きoutput byte span |
| 10 | `RESOURCE_CREATE_BLOB` | blob class、flag、size、ID、上限付きcommand byte span |
| 11 | `CONTEXT_INIT` | parameter mask、capset、ring設定、上限付きdebug-name span |

`CONTEXT_INIT`はdebug nameに専用byte spanを使うため、numeric parameter recordにnative pointerを
含みません。`EXECBUFFER`はfence FDをsync-file attachmentで表現し、ringとtimeline-syncobj
fieldを保持します。

## AMDGPU command契約

AMDGPU setはLinux 6.18.48の20 commandを固定します。argument 1はcommand固有のinline
request recordです。残りの各argumentはschemaがtyped spanまたはnative-object attachmentへ
bindします。

| ID | command | request data | completion data |
|---:|---|---|---|
| 1 | `GEM_CREATE` | allocation record | GEM handle |
| 2 | `GEM_MMAP` | handleとmapping rights | mapping recordとmemory attachment |
| 3 | `CTX` | operation、context、priority | operationで選択するcontext result |
| 4 | `BO_LIST` | operationとBO-entry span | list handle |
| 5 | `CS` | contextと反復可能なtyped chunk span | submission handle |
| 6 | `INFO` | query、selector、output span | response recordとrequired count |
| 7 | `GEM_METADATA` | metadata record | metadata record |
| 8 | `GEM_WAIT_IDLE` | GEM handleとdeadline | busy stateとdomain |
| 9 | `GEM_VA` | mapping recordとinput-syncobj span | status |
| 10 | `WAIT_CS` | submission identityとdeadline | busy state |
| 11 | `GEM_OP` | operationとtyped output span | response recordとrequired count |
| 12 | `GEM_USERPTR` | rangeとmemory attachment | GEM handle |
| 13 | `WAIT_FENCES` | fence spanとdeadline | signaled stateと最初のfence |
| 14 | `VM` | VM operation | VM flags |
| 15 | `FENCE_TO_HANDLE` | fenceとoutput kind | syncobj handleまたはsyncobj/sync-file attachment |
| 16 | `SCHED` | operationとtarget-session attachment | status |
| 17 | `USERQ` | queue recordとtyped MQD span | queue ID |
| 18 | `USERQ_SIGNAL` | syncobj、read-BO、write-BO span | status |
| 19 | `USERQ_WAIT` | syncobj、timeline、BO、output-fence span | returned/required count |
| 20 | `GEM_LIST_HANDLES` | output-entry span | returned/required count |

`CS`はinput/output syncobj、通常/scheduled dependency、timeline wait/signalを含むUAPI
10 chunkすべてへ個別のrecord IDを割り当てます。`INFO`は定義済み27 query IDごとに許可する
response recordとcardinalityを割り当てます。record size、field offset、UAPI command number、
chunk ID、argument ID、上限、方向、attachment class、ownershipをschemaから生成し、encodeまたは
dispatchより前に検証します。

## startup resource

role closureは次のresource interfaceを宣言します。各interfaceは独自のschema digestと
native-handle roleを持ちます。

| interface | resource typeとrights | consumer |
|---|---|---|
| PCI function | device: command、map、DMA、reset-required | device-pci provider |
| DMA domain | device: command、DMA | device-pciとGPU driver |
| IRQ endpoint | notification: wait | GPU driver |
| firmware store | memory: read、map | AMDGPU driver |
| object exchange | channel: send、receive | DRM bridge |
| GPU data channel | channel: send、receive | GPU role root |

PCI interfaceはcanonicalなconfig-space accessと上限付きBAR map requestを提供します。DMA
interfaceはgenerationのIOMMU domain内でDMA memoryをcreate、map、unmap、synchronizeします。
IRQ objectはsource identityとgenerationを持ちます。firmware storeはAMDGPU profileでpresent、
VirGL profileでabsentです。hard reset authorityはreset-required device resourceに結び付いた
host lifecycleが所有します。

各interfaceはconsumer nodeへ明示的にbindします。複数nodeが同じinterfaceを使う場合は、一つの
closure-shared grant objectが同じunderlying authorityを保持します。sandboxはDRM closureの
mapより前に全device interfaceをcommitします。

## queue topology

channelは次のqueue classを持ちます。

| queue | 個数 | traffic |
|---|---:|---|
| event | 1 | sandboxから`gpud`へのnotification |
| control request | 1 | profile、session、cancel request |
| display request | 1 | DRM mode command |
| execution request | profileで固定、1以上 | render、compute、memory、synchronization command |

全queueはtransport schemaのsplit virtqueue規則を使います。command catalog entryはrequest queue
classを一つ選びます。execution laneは並行実行できます。同じqueueではavailable-ring順を保ち、
queue間のorderingはsync pointとcommand completionで表現します。

## base message

requestはtransport envelopeのgenerationとcorrelation IDを使います。

| request | 結果 |
|---|---|
| `PROFILE_QUERY` | 不変なprofile identity、catalog、feature、limit |
| `SESSION_OPEN` | 新しいgeneration-scoped session IDとnode capability |
| `SESSION_CLOSE` | accepted requestがdispatchを抜けた後のsession close |
| `COMMAND` | catalog command一つとcanonical response |
| `CANCEL` | cancellableなcorrelation ID一つのcancel disposition |

event queueは二つのbase messageを運びます。

| event | 意味 |
|---|---|
| `NOTIFY` | catalogで定義したDRM、KMS、sync、driver event一つ |
| `DEVICE_FAULT` | 現在のGPU generationを終了させるfault情報 |

全completionはbase status、response length、output span数、output attachment数、command
dispositionを持ちます。command acceptanceとGPU execution completionは別の事実です。成功した
非同期submissionはdriver acceptance後に返り、宣言したsync pointがexecutionを通知します。

base session、exchange、notification IDはnonzero 64-bit値で、一つのgenerationだけで有効です。
command-set-localなDRM handleはcommand schemaが定めたwidthとsemanticsを保ち、一つのsession
だけで有効です。base-protocol IDとしては使いません。KMS object IDはtopology epochと組み
合わせます。

## command data

`COMMAND`はsession、command set、command ID、request flagを指定します。dataは次で構成します。

- 上限付きinline canonical payload
- 登録済みtransport regionを参照する上限付きspan
- exchange IDで識別する上限付きnative-object attachment

spanはregion ID、offset、length、access rightsを持ちます。input spanはcompletionまで不変です。
output spanはcompletion時に可視になります。decoderはDRM code呼び出しより前に全nested count、
range、alignment、rights、overlapを検証します。

command-set schemaは全UAPI pointerをinline range、span、attachmentのいずれかへ変換します。
VirGL command streamは宣言されたread-only span内のdriver command byteとして扱います。
AMDGPU indirect bufferはsession内でmapしたGPU virtual addressにより参照し、BO list、sync
dependency、output recordはtypedな上限付きspanを使います。

## memoryとnative object交換

startup resource grantはgeneration-scopedなexchange channelを供給します。Linuxは
`SCM_RIGHTS`、PachaOSはnative capability transferを使います。wire messageが持つのはnonzeroの
exchange ID、object class、rights、roleだけです。

dynamic attachmentはshared memory、dma-buf、syncobj、sync-file、event、leased session
objectを扱います。exchange IDは一つのgeneration内で一意で再利用しません。brokerはnative
transferとcommand attachmentをgenerationとexchange IDで対応付け、両方が揃ってから可視化します。

attachment ownership modeは次です。

| mode | commit結果 |
|---|---|
| borrow | receiverのaccessはcommand completionまで有効 |
| share | receiverが独立した参照を取得 |
| move | receiverがownershipを取得しsenderが自身の参照をrelease |

全attachmentを一つのtransactionとしてstage、検証します。拒否したtransactionでは全inputを以前の
ownerへ戻します。completionは全input/output attachmentの最終dispositionを記録します。process終了で
そのgenerationのstage済みおよびcommit済みnative参照をすべてdropします。

GPU memory identityはsessionごとのDRM handleから独立します。PRIME exportはbrokerを通して
generation-scopedなshared objectをpublishし、各import sessionは自身のDRM handleを取得します。
CPU mappingは要求したmapping rightsとcache policyを持つmemory attachmentをpublishします。
device DMAはsandboxのresource grantとIOMMU domain内に置きます。

## synchronizationとevent

共通のsynchronization identityはsession sync objectと64-bit timeline pointの組です。binary sync
objectはcommand setで定義したbinary pointを使います。submitとatomic-display commandはinput/output
sync pointを宣言します。sync-file import/exportはnative-object attachmentを使います。

transport completionが順序付けるのはresponse dataです。GPU workはdriver queueと明示的なsync
pointで順序付けます。wait commandはabsolute monotonic deadlineを持ち、cancel可能です。eventは
sessionごとのmonotonic event sequenceで配信します。KMS user dataはopaque 64-bit値として
page-flip、vblank、sequence eventへ変更せず返します。

## display ownership

primary sessionは`gpud` policyに従ってDRM masterまたはleaseを取得できます。render sessionは
render authorityを公開します。sandboxがKMS objectとatomic stateを所有し、`gpud`が各clientの
address可能範囲を所有します。hotplugはdevice topology epochを更新しcatalog eventを生成します。
object IDはqueryで返したtopology epochと組み合わせて解釈します。

atomic commit completionはDRMがstateをacceptしたことを表します。out-fenceまたはeventがscanout
completionを通知します。display requestは専用queueを使い、render trafficからtransport capacityを
分離します。

## statusとfault

base completion statusは`OK`、`INVALID`、`UNSUPPORTED`、`DENIED`、`NOT_FOUND`、
`STALE_GENERATION`、`LIMIT`、`NO_MEMORY`、`BUSY`、`TIMED_OUT`、`CANCELED`、
`SESSION_LOST`、`DEVICE_LOST`です。command setはbase statusと検証済みdetail値をclient UAPI
resultへ対応付けます。

well-formedなrefused operationは`UNSUPPORTED`で完了します。invalidなsession/objectは
`NOT_FOUND`または`STALE_GENERATION`で完了します。descriptor、envelope、schema、span、
attachment、reserved fieldの違反はchannel faultです。

`DEVICE_FAULT`はfault class、source set、source ID、optional guilty session、reset requirement、
diagnostic tokenを持ち、そのgenerationを終了させます。hardware resetはsandbox process終了後のhost
resource lifecycleが実行します。

## generation lifecycle

正常停止は次の順です。

1. `gpud`がsession生成とcommand admissionを停止
2. kobox2がmanagement channelでquiesceを要求
3. sandboxがwaitをcancelし、accepted requestをdrainし、sessionをcloseしてDRM closureをquiesce
4. process終了でnative参照のrevokeが完了
5. hostがgeneration resourceをrevoke、reset、reap、release

fault cleanupはprocessをterminateし、同じrevoke、reset、reap、release境界を通ります。restartでは
新しいchannel memory、notification object、resource grant、object ID、exchange ID、generationを
割り当てます。`gpud`は旧completionを破棄し、旧client sessionすべてへdevice lossを返します。

## 必須limit

各profileはsession、in-flight request、execution lane、inline byte、span、attachment、command byte、
sessionごとのobject、総GPU memory、pinned memory、mapping、context、sync object、KMS object、
event backlog、wait durationのlimitを固定します。ownership transferやDRM副作用より前にlimitを
検査します。session closeおよび全generation teardown後にcounterがprofile baselineへ戻ることを
要求します。

## conformance gate

protocol conformanceはcanonical encoding、全command-set catalog、malformed nested data、rights、
attachment rollback、concurrent lane、completion ordering、event backpressure、cancel、queue
wrap-around、generation rollover、stale inputを検証します。

VirGL profile gateはVirGL capset、render node、Mesa/libdrm submission、
`VIRTIO_GPU_CMD_SUBMIT_3D > 0`、accelerated pathでの
`VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D == 0`、llvmpipe/swrast以外のrendererを要求します。
fault injectionはrequest dispatch、attachment exchange、device submission、fence completion、KMS
event delivery、restartを対象にします。

AMDGPU catalog gateは固定した全AMDGPU commandを同じbase messageとattachment modelでencode
します。RX 9060 XT execution gateはAMDGPU closureとdevice resource interfaceを使い、同じsession、
synchronization、event、fault、generation規則を適用します。
