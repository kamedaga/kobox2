# Filesystem service

Filesystem の ABI は番号付きの互換契約ではなく `dev` とする。
MIT の `protocol/schema/filesystem.json` を正とし、生成された layout と
境界検査付き codec を、別プロセスの GPL Linux と共有する。旧 filed ABI は
翻訳・互換対応せず置き換える。

## 権限と wire format

native host adapter は owner が登録する client 専用 queue にも対応する。
管理 endpoint の CLIENT_PREPARE で新しい binding ID と client を予約し、
CLIENT_BIND で queue VMO と専用 doorbell を接続し、CLIENT_BOUND で接続を ACK する。
client に設定を渡す前に ID を予約するため、異なる client の attach 順は逆でもよい。
doorbell に渡す権限は send/receive/wait/poll/inspect のみで、owner endpoint ではない。
受け付ける packet は private binding に一致する available-ring 通知だけである。
FS dispatch の client は binding から渡し、可変の request header から選ばない。
不正 packet/chain はその client endpoint を退役させ、共通 mount や他 client は停止しない。

CLIENT_RELEASED は受付停止、受付済みjobのcollect/release、mapping解除、保持した
native endpoint inventoryのcloseが完了してから返す。VFS実行中にもreleaseを受け付け、
退役laneのmappingとchainはdrainまで保持する。private応答を破棄し、未消費chainは
実行しない。動的native waitは各subsetにcontrolとprivateな待機一覧wake endpointを含め、nativeの
256-FD wait 上限を超えると短い待機を巡回する。FS の client 数上限ではない。
filed は認証済み session の予約・attach・退役をこの owner API へ接続する。
LPR は通常 FS 要求を専用 sandbox queue へ直接 publish し、直接完了を待つ。
filed はその queue を map・消費せず、client control endpoint の通常 FS 要求は中継せず
拒否する。native bootstrap / 管理 client は信頼済み control 経路を維持する。
LPR は要求別の pending inventory を使い、remote wait の間は session / queue 排他を
保持しない。本番adapterは共通async lifecycleでLinux worker poolとnative ringを接続する。
NVMe rootのQEMUではblocking FIFO open中にも同一processと別processのstatが完了し、
相手側openで待機を解除できた。threadedな共有offsetとatomic vector appendも成功した。

要求は 192-byte header と可変長 span、応答は 64-byte header と正確に
`data_length` 分のデータで構成する。整数は little-endian、reserved はゼロ。
native struct は private な decode 結果であり共有メモリに重ねない。
path は末尾 NUL を含み、途中 NUL は認めない。span の重複・領域外参照を拒否する。
path/name の上限判定は Linux が行い、codec 独自の小さな上限は設けない。

host adapter は認証済み client を要求 bytes とは別に渡す。
CLIENT_REGISTER / CLIENT_CREDENTIALS / CLIENT_UNREGISTER / TRANSFER_DUP は
owner 専用の別 channel を使う。登録は 80-byte credential record と、alignment を
検査した可変長 supplementary groups を運ぶ。Linux は real/effective/saved/fs IDs、
capability sets、securebits を immutable に保持する。ID と更新世代は単調増加し、
古い世代や退役した ID の再利用を拒否する。通常要求の UID/GID は chown の引数であり、
呼び出し元の資格情報ではない。

owner は登録・更新 request の capabilities field で、protocol が定義する filesystem
capability 権限も渡す。これは Linux の credential capability sets と別の権限である。
service は権限と資格情報の組を原子的に保持・取得し、private な decode 結果を認可して、
同じ snapshot を保持済み資格情報の下で実行する。直接 queue から中継側の操作別権限を
迂回させず、Linux DAC も追加の検査として維持する。bit 31 より上を含む未定義権限は
拒否し、通常 request の capabilities は権限を付与しない。全権限を持つ client でも、
data dispatch から owner / frontend 操作は実行できない。

data request の client / credential generation がゼロなら、認証済み queue binding の
client と現在の権限・資格情報を使う。ゼロが owner を選ぶことはない。非ゼロの主張は
binding と一致しなければ拒否する。資格情報・権限の更新は ACK の前に組として公開する。
受付済み要求は旧い組を保持して完了できるが、更新後の受付は新しい組を取得する。

client ごとに xarray で実 Linux `struct file` の参照を管理する。dup と owner が
認めた transfer は同じ実 file を保持し、path を開き直さない。offset / flags も
Linux の同一 object に属する。受取側にはその table の新しい参照を発行する。
受付時にclient、immutableな権限、実file/dirfdをpinするため、queued要求もsource closeや
資格情報変更で失効しない。
mount root と device capability は共有 descriptor ではなく、独立した owner の
信頼済み状態から決まる。

service は owner control と失敗経路も含め、資格情報と request lock を復元してから、
各要求の戻り際に serving task の upstream task work を実行する。PID 1 の要求ループは
Linux user mode に戻らないため、これがないと最終 `fput` が write 権限や unlink 済み
tmpfs inode を無期限に保持する。完了の公開はこの後始末の後とし、他 task が保持する
処理中の参照は有効なまま維持する。

## Linux受付とworkerの所有権

process-localな`fs_service.h`のrequest APIはprepareとexecuteを分離する。
prepareは入力を一度だけcopyし、その時点のclient参照、immutableな資格情報と
capability権限の組を保持する。待機中の要求が後から付与された権限を取得することはない。
executeは実際のLinux task上で一度だけ実行し、request scopeを復元してそのtaskの
deferred workを処理してから完了する。prepare後のcancelは操作を実行しない。
既存の同期dispatchも同じ実装を使い、入力copyを重ねない。

`fs_worker.h`は常駐Linux kthreadと、作成したtaskを唯一のownerとするAPIを提供する。
submit・collect・releaseはownerだけが行う。workerはring index、共有request bytes、
host RPCに触れず、要求ごとのprivate input/output leaseを使う。実行可能なclientは
1要求のdispatchごとに巡回し、client単位の実行枠が他client用のworkerを残す。
VFS操作はpoolの短いlockを保持しない。

data受付はrequest header・immutable input・private outputを3回個別に確保せず、
1つの要求所有leaseとして確保する。実行がclient・資格情報・file参照を解放した後も、
collect/releaseまで領域を保持する。借用bufferの同期dispatchは小要求にinline snapshotを
使うが、これはfast pathの閾値でありpath/payload上限ではない。大要求は全bytesをprivateに
snapshotし、既存のcaller byte budgetに従う。

管理taskを含む各workerが、それぞれ専有のscratch `fs_struct`を持つ。path操作は毎回
root/pwdを照合しumaskを設定、実行taskへ一時的に取り付けてから元のpointerへ戻す。
資格情報とLinux DACは要求ごとのscopeを維持する。再帰使用は新規scopeへfallbackし、
同時実行するtask間でscratchを共有しない。起動失敗は部分確保したscopeを解放し、
停止時は全workerのjoin後・service/mount破棄前に最後のpath参照を解放する。
完了時はownerをwakeし、完了したdata worker自身がfairなrunnable listを再確認する。
自分が解放した1つの実行枠を再使用し、その枠を奪い合う別workerを追加でwakeしない。
submitは追加workerをwakeし、destroyは受付済み要求のdrain前にworkerを停止しない。
data workerはarm済みのwait predicateで実行可能jobを直接取り出す。ready確認と
取り出しで2回lockしない。取り出し後にstopが競合しても実行し、受付leaseを放置しない。

保持したscratchは照合中、worker専有でどのtaskにも取り付けられていない。実際の
mount＋dentryが一致すれば必要な参照を既に所有しており、upstreamのget/set/putを繰り返さない。
rootやdirectory operandが変わればupstream setterを使い、umaskと資格情報は要求ごとに設定する。
client identity／権限のcacheやnameiの省略ではない。fresh／再帰scopeは従来の初期化を維持する。
nativeと照合するnamespace／dirfd／umask／資格情報workloadでも保持scopeを検証する。

online論理CPUごとに1つの常駐data taskを起動前にbindし、残りは移動可能なoverflow
workerとする。単独submitはlocal常駐workerとLinuxの同期wake配置hintを優先する。
backlogではremote常駐worker→移動可能なoverflow→localの順にfallbackする。
直前の受付区間に要求の重なりがあったかだけを最後の完了lease解放まで保持するため、
連続burstの先頭を毎回単発扱いしない。1つの単独要求のdrainでlocal hintへ戻る。
worker選択がwakeを遅延したり実行poolを制限したりすることはない。
worker選択だけを変え、client巡回・実行枠・共有VFS/mount/cache・専有path scopeは維持する。
ownerの永続wait entryは全workerのjoinまでtask参照を保持し、非burstの完了workerが待機へ
移る場合だけ同期配置hintを使う。burst完了は保持したownerを直接wakeし、追加のwait-list lockを
使わない。並列処理量の改善を保証するものではなく、単発latencyとは別に測る。
service寿命中のCPU topologyは固定とする。

ownerは全消去済みの固定サイズdata-job／空client metadataを再利用する。空き分を先に
使うことで、live＋cache件数を設定済み受付件数のhigh-water markに収め、恣意的な上限を
追加しない。response leaseのreleaseと最終task_work後、packed入力・出力領域を全消去し、
同じbyte容量にだけ再利用する。受付ごとに新しい不変snapshotをcopyし、再認証とoperandの
pinを行う。資格情報・file参照・client identityはcacheしない。idle＋live payload bytesは
既存の総budgetを共有し、必要なら確保前にidle領域を退避する。要求サイズの新しい上限や、
匿名idle領域の旧clientへの課金は追加しない。closeで空きbufferとmetadataを即座に解放し、
drain中のreleaseは直接freeする。private診断は新規／再利用bufferを区別し、runtime gateで
消去・client間の再利用・EOF zero・確保失敗・大きい2要求でのcache退避を検証する。
大きい不変snapshotも検証する。idle領域がある状態で確保に失敗したら、それを回収して
実行前の確保だけを1回再試行する。FS操作は再実行しない。controlled ENOMEMとこの回収を
gateで確認するが、全RAM枯渇の実測とはしない。
独立した管理要求の
所有権とbudgetは変更しない。

nativeの論理CPU所有権publicationは常にsequenceを進める。futex wakeはowner lock下で
登録した待機者がいる場合だけ行い、domain-entryとidle-ownerの両方を登録する。
unlock/sleepより前に登録するため、その間のpublicationはfutexのexpected値を無効にする。
notification保護・domainの排他的所有権・host-callのFP保存は維持する。
native CPU switchはowner lock下でdomain移譲を完了し、旧taskの論理CPU bindingを外して
unlock後に後継をwakeする。後継permitのwakeだけをcritical section外へ移し、登録済み
domain sequence待機者へのlock下のpublicationは維持する。Linux runqueueが
後継をpinし、旧taskはparkから戻りcpu_enterでdomainを取り直すまでLinuxへ再入しない。
wake直後に後継を実行し、旧taskのpark前に復帰permitを発行するfocused testも行う。
count付きnative task permitのconsumerは1つだけなので、futex wakeは最大1待機者を指定する。
共有所有権／lockのbroadcastは変更しない。

IRQ query/save/disable/enableは、通知save→leaf CPU lookup→IRQ query/change→通知restoreの
連続したnative transaction全体を1つのFP保存区間にまとめる。その間にLinuxのFP状態を
使う処理を置かず、enable/restoreでcallbackが入れ子になったりCPUが移動しても元のbankを
保存し続ける。IRQ状態はhostへ照会し、cacheしない。preempt/per-CPUのguardは維持する。
診断buildはFS計測の前にIRQ save/restoreと通知save/restoreのpairを別に測る。

すべてのnative境界で、固定legacy FP/SSE bank全体のsave/restoreを継続する。
毎回のFNINITの代わりに、default control・空のx87 tag・zero vector registerを持つ
不変のarchitectural FXSAVE imageからhost環境を初期化する。lazy FPU所有権やhost callbackの
no-FP宣言ではない。nativeの任意clobber、入れ子callback、migrationでもcallerのbankを
復元し、upstream Linuxのtask FPU方針は変更しない。

production storage fragmentで`SLUB_TINY`と無関係なSLUB debug defaultsを無効にする。
既存のsize最適化・SMP/preempt・namespace設定は維持し、profileで再生成したconfig全体を
pinする。coreとdriver moduleは一緒にrebuildし、旧tiny allocatorのmodule closureを
通常SLUBのcoreへ混ぜない。per-CPU slab保持はメモリと確保／並列処理量のtradeoffであり、
すべての単発latencyを減らすとはしない。kobox所有の`storage_slub.config`は保存した基準から
比較構成を再現するfragmentで、upstream allocatorのpatchではない。

storage専用の任意build flag `--fs-internal-bench`で、FS IPCなしの同期dispatchと実workerの
submit→execute→collect/releaseを測る。depth 8は処理量であり要求ごとのlatencyではない。
stage profilingはowner受付と重複し、lease確保/freeをowner submit/releaseへ移しても
その費用が消えたとはしない。Linuxのscheduled task runtimeはhost物理CPU時間やPMUの
unhalted cyclesではない。Linux task切り替え件数はnative futex callと他Linux taskを含めず、
metadata診断は新規／再利用確保件数を数える。owner/data taskごとの再入安全な診断counterで、
通知save・IRQ transaction・host時計取得・handoffも数え、snapshotはphase loopの外で取る。
当該task上のIRQ callbackは含むが、別のidle/control taskやnative futex件数は含まない。
counter追加自体の費用があるため、この走行は診断でありproductionの高速化結果とはしない。
時計取得とno-op FP境界の単体計測も分け、安全なguardを削らず残る構造的対象を選ぶ。
native storage adapterの任意flag `--handoff-bench`は、publication→後継taskのentry、
wake syscall、permit park、owner lock待機のphase総量も記録する。Linuxのowner/data counterと
異なり、全論理CPUのidle/control taskを含む。park待機時間は他taskの処理も含むため、
操作latencyへ加算したりCPU時間とみなしたりしない。task間elapsed TSCと重複区間はPMU
cyclesの会計ではない。phase epochで前phaseからの待機を除外し、hot loop外でログを出す。
native診断は通常buildでOFFとし、host／wire ABIは変更しない。
診断とcontrolled allocation-failure gateは通常runtimeに含めず、
wire/host ABIの変更は不要。

callerが指定するresource budgetで、clientごと・全体の受付件数とinput/output bytesを
制限する。1clientが受付budget全体を消費できない。完了後もcollect/releaseまでprivate
outputを含む計上を保持し、固定サイズのjob/client metadataは受付件数で制限する。
一時的な圧力はEAGAIN、要求がメモリbudgetを超える場合はENOMEMであり、file sizeの
新たな上限ではない。closeは受付を止め、全jobをcollect/releaseするまではdestroyが
EBUSYを返す。その後に常駐taskをjoinする。失効はserviceのidentityを削除して新規jobを
拒否し、受付済み要求は保持済み参照で収束させる。空のclient groupはactive registryから
除き、全消去した確保領域だけ再利用できる。identityや失効tombstoneを無制限に残さない。

data受付とは独立した常駐管理taskに、1件の要求と明示した`control_bytes` budgetを
予約する。ownerのexec/device要求も受付時にimmutableな権限を保持する。native接続では
registry/filetable更新を対象clientのadmission barrierで順序づけ、blocking exec/device
VFSで他clientの受付を止めない。ただし複数clientのsleepで有限data pool全体が埋まる
可能性はあり、bounded admissionは無制限のVFS進行を保証しない。実Linux gateでは
全data workerをinode rwsemで意図的に待機させ、受付と管理完了が継続し、unlock後に
queued dataが再開することを検証した。

共通async lifecycleはopening Linux taskをcontrol/ringの唯一のownerとする。
native helperはendpoint番号のsnapshotでreadinessを待つだけで、controlも共有ringも
消費しない。workerはLinux schedulerでownerを直接wakeし、完了ごとのnative IPCを
追加しない。idleはarm→fence→recheckしてsleepする。fault／QUIESCEは受付を止めるが、
受付済み要求をdrainし、native helperとLinux workerをjoinしてからmappingを退役する。
従来の同期serviceも共通の起動・停止処理を維持する。Linuxのwake-before-sleepと、nativeの
順不同完了／fault／起動失敗の後始末gateは成功した。

本番adapterは受付済みjobごとに要求所有chainを保持する。ownerだけがchainを検査・受付し、
応答公開とlease解放を行う。direct clientのbytesはLinux受付でsnapshotし、nativeで重ねて
copyしない。一時的な受付pressureでは最後の未書き込みchainだけをavailableへ戻す。
実行せず、worker budget外にprivate backlogを保持しない。client barrierは管理応答の
公開とlease解放まで維持する。受付済み要求は元の権限を保持し、後続受付は更新後の組や
失効を反映する。

native readiness helperは短い待機一覧lockでFD番号だけをsnapshotし、wait中にclientの
pointerを保持しない。変更epochを確認して古いwaitの失敗を判定する。ownerはendpointを
待機一覧から外し、waitを解除してからcloseする。queueごとの要求inventoryを事前確保し、
RAM由来のnative budgetへ計上する。workerのprivate input/output budgetもsandbox RAMから
配分し、管理reserveを分ける。ring/worker接続、退役ACKの延期、受付rollback、管理barrier、
513 clientのwait巡回はnative ASAN/UBSAN testで成功した。実Linux lock待機gateと本番NVMe
rootのFIFO testでVFS並列実行を検証し、別private COWのoffline e2fsckも成功した。
停止済み走行ディスクは変更しない。
任意のservice profileはqueue滞在とVFS実行を別々に記録する。

## 管理と退役の完了境界

filedは全queue共通のLinux client identityを失効させてから各queueをdrainする。
内部registry ACKで全bindingの新規VFS受付を止め、受付済み要求は保持済み権限とfileを使う。
外部managerのunregister呼び出しは、全queueのdrain、mapping退役、matching native ACK、
local capability closeが終わってから成功する。ACK／close失敗でsessionが残る場合はclient
recordも保持する。管理RPCのlost/malformed応答はstorage generationを停止し、資格情報更新が
未確定のままcached authorityで処理を続けない。

FD転送escrowの退役はownerが保持するLinux handleを閉じてからnative lease endpointを閉じる。
close失敗ではleaseと残るhandle／FD inventoryを保持し、storage generationを停止する。
ACK確認済みのhandle closeは再実行しない。client所有leaseの退役ではそのclientのOFDを閉じない。

要求所有file viewはpathを開き直さず元のOFDを保持する。installとcloseは共通client tableへ
反映する。owner transferは両clientとsource OFDを、private executable read/statはexec guardを
受付時に保持する。要求teardownで参照を落とし、Linux task_workを実行してからworker完了を返す。

LPRはwait中断や不正応答で実行済みwriteを再publishしない。退役が不明なら繰り返しdropでも
エラーを維持し、mapping/inventoryを保持してremote操作は再送しない。ACK確認済みの退役だけが
残るlocal cleanupの再試行を許し、後始末が完了してから成功する。forkでprivate queue capabilityは
継承しない。childは再利用されたnative FD番号をcloseせずcopyされたqueue viewを破棄し、新bindingを
確立する。本番testではfork/dup/execの共有offset、childだけのCLOEXEC、child終了後のparent FDの
有効性を確認し、native peer死亡／drainとfault gateも検証する。

## Operations

catalog は owner/session/exec の後に Linux 操作を配置する。変更時は適切な位置に
挿入し全 consumer を再生成する。旧 opcode の alias は残さない。

| 操作 | 要求の引数 | 成功応答 |
| --- | --- | --- |
| SESSION_DETACH | generation=attach 済み queue 世代、second_handle=channel ID | result=0、handle=退役した channel ID |
| DEVICE_ROUTE (frontend 専用) | 元の OPENAT2 dirfd/path/flags/mode/resolve | pin 済み O_PATH handle、host service tag、encoded statx |
| EXEC_OPEN (owner のみ) | client/revision、handle=通常 dirfd、path | handle=private executable 参照 |
| OPENAT2 | handle=dirfd、path、flags/mode/resolve、umask | handle=新 file 参照 |
| READ/PREAD | handle、length、PREAD の offset | result=byte 数、raw data |
| WRITE/PWRITE | handle、length=data span 長、PWRITE の offset | result=byte 数 |
| STATX | handle=dirfd、任意 path、flags、mask | 明示 encode した 256-byte statx |
| STATFS | handle | 明示 encode した 120-byte statfs |
| RENAMEAT2/LINKAT | handle/second_handle=dirfd、path/second_path、flags | result=0 |
| SYMLINKAT | path=target、second_path=新 name、handle=dirfd | result=0 |
| MKNODAT | handle=dirfd、path、mode、length=Linux encoded device | result=0 |
| FCNTL | handle、flags=command、length=scalar 引数 | result=Linux の戻り値 |
| FTRUNCATE | handle、offset=新 file size | result=0 |
| SYNC | file handle なし | Linux の全体同期完了後に result=0 |
| SYNCFS | handle=open file 参照 | result=Linux の filesystem 同期結果 |
| READLINK_HANDLE | handle、length=capacity、flags=0/1 | result=byte 数、raw path |
| FILE_VMO | handle、offset、length、mapping rights | result=実 byte 数、native VMO capability |

READLINK_HANDLE は open 時の保存名ではなく Linux の d_path を使う。flags=0 は
readlink 同様に truncate し、削除済み・anonymous file の表記を保つ。flags=1 は
directory/getcwd 用で、末尾 NUL の余裕がなければ ERANGE、削除された directory は
ENOENT。戻り値には NUL を含まず host が補う。path は granted root からのもの。
FILE_VMO は cache しない private snapshot であり、byte 数を opaque handle に入れない。

host 側の private snapshot 作成は、設定済み storage arena に収まる分量で読み込む。
親子 compiler image が生存する間に、image 全体と同じ大きさの返信と一時 channel を
重ねて確保しないためである。caller は snapshot 全体を完成させるので、arena の大きさは
file size の上限ではない。通常 client の READ/PREAD / WRITE は oversized exchange を
含めて1要求を1回の Linux 操作として実行する。

EXEC_OPEN は owner 専用の実行準備操作であり、execute-only file の read 権限を通常の
client へ与える操作ではない。認証済み actor の資格情報と granted FS root の下で
upstream の open_exec を使う。参照は別の private namespace に置き、owner channel の
PREAD / STATX / CLOSE だけが利用できる。dup / transfer / client への export は禁止する。
参照数付き object が画像の読込と実行中の read を越えて Linux の deny-write guard を
保持し、最後の参照解放時に一度だけ解除する。保証対象は private executable snapshot の
準備期間であり、実行中の PachaOS process 全期間に Linux mm->exe_file の write 排除を
再現するものではない。

エラーは負の Linux errno、data / handle は空にする。認証済みで有効な要求が private
snapshot を確保できなければ ENOMEM とし、成功を装わない。不正 framing は transport
error。GPL service は実 Linux namei/VFS/shmem/syscall helper を使い、数値 offset で
Linux struct を再構成しない。

SYNC と SYNCFS は分離する。Linux の sync() は分離された storage runtime 内の
全 superblock と block device を同期するが、syncfs(fd) は有効な open file の
filesystem だけを同期し、その writeback error を返す。SYNCFS の handle=0 は
全体同期の alias ではなく EBADF。Linux は非特権 caller による同期と read-only fd の
syncfs を許すため、write-only file や WRITE grant は要求しない。認証済み actor の
資格情報は操作内だけに適用する。dev catalog の SYNCFS 直前へ SYNC を挿入して
後続操作を振り直し、誤った zero-handle encoding による意味論の不一致を残さない。

## Queue と寿命

DEVICE_ROUTE は直接 open が ENXIO / ENODEV を返した後の、副作用のない振り分けだけを
行い、通常 open を再実行しない。owner entry は元の要求を snapshot し、登録済み client の
権限・資格情報の組を保持して、元の open に必要な capability と filesystem-ID DAC を
検査する。O_PATH lookup のみで create / truncate / driver open はしない。filed は同じ
pin 済み device を分類し、LPR はその statx から既存 native service を選び、alias path を
再解決せず接続して probe handle を閉じる。一般 file や未対応 device でも FS 操作を再送しない。

host の FS channel は既存 split ring に submit / poll / arm / collect API も持つ。
各 pending request は2つの descriptor、重複しない arena lease、private な要求識別子を
自身の collect まで所有する。別 caller が used entry を取得しても lease は解放しない。
arena / descriptor 枯渇は publish 前の local backpressure として返す。公開の成否が
不確実な場合や不正 completion では channel を fault し、remote 退役確認まで inventory
を保持する。queue 操作は単一 owner または短い外部排他が必要だが、remote wait の間は
排他を持たない。reserve / publish と read-result / release も分離し、descriptor / arena
lease を pin したまま大きな payload copy を queue 排他の外で行う。LPR は複数 caller を
接続し、1 caller が doorbell を drain しても全 used entry を要求別 inventory へ記録する。
completion / collect / receiver 交代を futex epoch で local waiter に知らせる。最終退役は
受付を閉じ、queue lock の外で caller 参照を待つ。host test では先頭停止中の逆順 completion、
descriptor / 総量 lease 枯渇時の backpressure、処理中の退役を確認した。private な local
endpoint pair は sibling の queue fault 時、remote reply を待たず native receiver を起こす。
非blocking native receive の EMPTY は channel 障害ではなく待機条件として扱う。
fault 時の待機解除も host test 済み。NVMe root の QEMU では通常操作、大きな直接I/O、
device振り分け、4スレッドの共有offset vector I/O／atomic append、native libc FS/execが
成功した。走行後e2fsckも別private COWで構造修復せず成功した。失敗注入では未attach予約
の回収、lost ACK時の保持、local部分解放のinventory、native wait中断の非再実行を検証した。
sandboxの並列VFS実行を完成したとは扱わない。

data / 管理 channel は共有 split-ring engine を使う。metadata には生成された FS schema
digest を置き、両側が private snapshot と独立した期待値を比較する。古い binary は
互換 fallback ではなく拒否する。HELLO flag 1 は認証済み client の新しい binding ID を
予約し、32-byte config を返す。ID は client identity と別で、length は最小 arena を指定する。

queue の退役と managed client の退役は別の寿命である。runtime が local な client
endpoint を close しても、manager や別の capability holder が同じ側の duplicate を
保持することがある。その HANGUP を native exit / self-exec の前提にすると循環待ちに
なる。このため SESSION_ATTACH の隣へ SESSION_DETACH を挿入し、後続の操作番号と
schema digest を一括再生成する。版数、旧 opcode の alias、互換 fallback は作らない。

SESSION_DETACH は認証済み frontend control endpoint を使い、ring descriptor や
owner credential channel は使わない。要求は header だけで capability を転送しない。
generation は attach 済み queue の世代、second_handle はその channel ID とし、他の
操作引数と span はすべてゼロ。client / credential の envelope field で endpoint の
認証済み actor を置き換えることはできない。server は actor 自身の queue に限り、ID と
世代を照合して退役する。queueをquiesce / faultし、受付済みjobをdrain / releaseしてから、
shared memoryのunmapと保持する
queue capability の close を完了してから、result=0 / handle=channel ID の ACK を返す。
caller は照合済みの ACK を受けて初めて自身の mapping を解放し、local endpoint の close
だけを ACK と見なさない。存在しない queue や繰り返した detach は失敗し、native 退役が
不確実なら未解決の inventory を保持して成功 ACK は返さない。queue detach は client の
identity、資格情報、file参照、file leaseを失効させない。退役中queueにも実行中jobが
残り得るため、drainが終わるまでmappingとprivate chain inventoryをpinする。

通常 FS と owner credential の queue は分離する。descriptor の client field だけで
権限を得ることはない。path 検査・Linux 実行前に要求を private bytes へコピーし、
応答も completion まで private に保持する。

arena サイズは交渉済み transport budget であり、FS の I/O 上限ではない。
LPR は caller が残る旧 queue を退役させず、同じ認証済み client へ大きな追加 queue を
予約・bind する。追加 queue は各 call の直後に退役させ、対応する sandbox ACK の後で
mapping / capability を解放する。filed は共有 backing、最悪時の不変 input snapshot、
private inventory の総量を設定済み storage sandbox RAM の1/4以内に制限する。lease 数は
native FD table の半分を上限とする（1 lease 当たり保持する capability は2つ）。通常 client
arena は RAM / lease 数 budget から導き、設定済み owner arena 以下にする。全 process に
大きな owner arena を常設せず、大きな call は追加 lease を交渉する。自 client の実行中
追加 lease がある場合はその退役を待つ。他 client の pressure しかなく local wake がない
場合は allocation failure を返す。ACK が不明なら charge / inventory を terminal teardown
まで保持する。attach前の確保失敗はHELLO予約をmatching DETACH ACKで回収する。
回収ACK不明や不正configでは受付を閉じ、新しいgenerationを繰り返し予約しない。
local close/unmap失敗では残った資源を正確に保持し、metadataは最後に解放する。
資格情報と file は維持し、通常要求を直接
publish する。filed 中継への
fallback はない。不確実な queue を大きな arena の要求で reset しない。信頼済み native
管理要求は一時 owner-private channel を使う場合がある。
大きな read/write/readv を interleave 可能な複数 Linux 操作に分割しない。
実 RAM / native FD budget が足りなければ allocation は失敗する。
scalar DATA は staging VMO を確保しない。vector I/O は metadata のみ snapshot し、input
を要求所有 arena へ copy、output を completion から scatter する。全量の private bounce
allocation は行わない。

teardown は全 request / mapping を退役させ、sandbox を停止・WAIT した後 DMA と
controller generation を解放する。退役が不確実なら inventory を保持してremote操作は再送しない。
remote退役確認済みの場合だけ、残るlocal cleanupを再試行できる。
sandbox が落ちると世代を失効させ、新 root へ handle を復元しない。

## Host integration と現在の範囲

PachaOS が role policy を持ち、原本の read-only bootfs capability から immutable な
storage package を直接読む。USB/network/GPU/storage は共通 owner startup helper を
使い、4 個目の起動・lifecycle 実装を作らない。実 block device の PCI 祖先を authorized
device と照合し、設定された ext4 UUID を選ぶ。重複 UUID は拒否する。
初期 package の単一 NVMe は具体的な policy であって構造上の device 数上限ではない。
RAM / queue budget は immutable boot config から渡す。

filed に残すのは client 認証・capability policy・接続管理、native 管理、exec、private VMO export、service
routing。filed VFS/cache/tmpfs と LPR の FS data/name/offset cache を撤去し、Linux が
namespace と file 状態を持つ。exec は固定 argv/envp/string buffer でなく可変長 span を使う。

live cpio boot と MAP_SHARED の page-cache coherence / writeback は後続 live/shmem
フェーズの対象。SHARED_FILE_VMO は現在 EOPNOTSUPP とし、private copy を Linux と共有の
メモリとして扱わない。pointer を持つ fcntl lock 操作も serialization を定義するまで
明示的に未対応。private mapping の試験成功だけでこれらも動くとは主張しない。
