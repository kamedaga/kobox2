# Linux MM／VMAと別processのhost mapping接続

この計画は、kobox2のLinux driver sandboxが管理するメモリを、Linux host上の別client processから
利用するための接続を扱います。core `.so`内のupstream LinuxがVMA・fault判断・page寿命を管理し、
最下層portがclientへの実mappingを担当します。processをまたぐ共有アクセスで、保護・無効化・
メモリ回収を正しく成立させることが目的です。

状態：公開／終了競合と最終参照回収を含め、Linux host上でMM／host mappingの必須Gateを通過しました。
これは以下のmemory port Gateの合格であり、DRM／PRIMEやGPU実行の完成ではありません。
transport単体や単独の結合caseだけでは、Gate全体の完了とはしません。

machine契約は[MM portの説明](../linux-sandbox/kobox/mm/README-jp.md)に記載します。
進捗と検証結果はREADMEではなく、この計画書へ記録します。

## 必須の結果

VMA・page選択・fault判断・page寿命はLinux、実host mapping・保護・同期的な無効化は最下層portが
担当します。外部host address spaceごとに、単一のboot-rooted core `.so`内の実Linux `mm`を対応させ、
そのmmを使うLinux taskへfaultを配送します。host PIDが再利用されても、古い操作を新processへ
向けてはいけません。

Gateは**実際の2 process**で、実アクセスによるfault、同じbackingの共有alias、mappingごとの
読み取り専用保護・部分unmapを検証します。truncateでは、全対象mappingの無効化完了前にpageを
再利用しません。fault・無効化・process終了が競合しても、古いmappingの復活・参照漏れがないことを
要求します。VMA生成だけ・fault関数の直接呼出しだけ・同一processのaliasだけでは合格にしません。
DRM／PRIME／FD共有との統合はこのmemory port計画の対象外ですが、2 processのmemory Gateは
この計画内で必ず通します。

## 実装前に確認した不足

- `linux-sandbox/kobox/boot/exception_port.c`はcore内の命令アドレスを受け付け、例外表で
  修復する入口であり、user-mm faultを解決する入口ではありません。
- `linux-sandbox/kobox/task/port.c`のlazy TLB／deactivate境界にはkernel taskのみという前提が
  残っています。user-mm切替のarch portが必要です。schedulerを置き換える話ではありません。
- `linux-sandbox/kobox/memory/early_boot.c`はvmallocのkernel PTEをhostへ反映しますが、
  user-mmのTLB無効化完了を接続していません。
- `linux-sandbox/kobox/memory/host.h`にはlocal map／resetしかなく、remote address spaceの
  識別・fault配送・process終了を含む完了確認がありません。無制限のclientへRAM全体のdescriptorを
  渡す方法をprocess境界にはしません。

実装には現在、upstream MM／fault、共有write、読み取り専用保護、部分unmap、truncateを確認する
実2 processの中間結合試験があります。これを完了Gateとは扱いません。
machine契約は[MM portの文書](../linux-sandbox/kobox/mm/README-jp.md)に記載しています。
作業状況・検証結果・残作業はREADMEではなく、この計画文書に記録します。

## 実装順

1. POSIX address-space transportと単体試験を追加します。host側tracerで別processの実行を管理し、
   実faultを取得してmap・保護・resetを行います。clientに別のLinux coreは入れません。
   mapping操作はguest schedulerの進行に依存させず、実際の完了を確認します。応答がないことは
   成功ではありません。process終了を無効化完了として扱う場合も、終了確認・回収が必要です。
2. archのmm activate／deactivateとuser TLB flushを接続します。`mm_alloc`・参照管理・VMA操作・
   page tableはupstreamを使い、host実行の識別子とLinux `mm`の識別子を明確に区別します。
3. 実アクセスfaultを、対応するtask／mmとアクセス種別を保持してupstream x86の
   `do_user_addr_fault`へ渡します。Linuxが決めたsignal／errorを返し、VMA・権限・shmem fault・
   page確保の判断をportで重複実装しません。
4. lockで保護した実Linux PTEを根拠にhost translationを公開します。LinuxのTLB flushを
   host無効化の完了へ接続し、その後にupstreamの遅延page解放を進めます。公開と無効化を同期し、
   古い完了を拒否して、address spaceの処理排出後にmmを解放します。
5. 2 processの全Gateとfault／無効化／終了の競合を通し、同じcoreでboot・SMP／time・memory・
   VFS・shmem・IRQ／softirq・時間付き待機・RCU・workqueue・cleanupのGateを再実行します。

transportには最下層のprocess-local machine interfaceを追加します。既存のlocal window callbackでは
remote実行の識別や無効化完了を表せないためです。具体的なinterface変更時にはcaller・所有権・
threading・error規則を記録し、ABI versionや互換shimは設けません。Linux上位実装は固定upstream
coreに保持します。

POSIXのptrace操作は専用native service threadに集約します。異なるpthreadに対応するLinux
workerから、別pthreadが所有するptraceを直接操作できないためです。fault／実行完了は独立した
machine通知として既存の論理CPU IRQ入口へ入り、upstreamの待機を起こします。追加するのは
process-localな通知種別と非同期event操作であり、wire ABIやschedulerではありません。
同期map／reset命令はLinuxへ入らず処理し、Linuxがpage-table lockを保持していても完了させます。
実行中clientの停止・無効化もhost serviceが担当し、guest workerの実行待ちにはしません。
IRQ禁止中の完了通知保留と、client実行中の無効化を試験します。

## 完了を証明する観測

- 異なる実host PIDとLinux mmを持ち、両processで同じ仮想アドレスを使っても配送先が混ざらない。
- 実アクセスが初回faultし、Linuxが選んだPFNで再開する。共有writeが両processに見え、無関係な
  mappingは分離される。
- 読み取り専用へのwriteをLinuxのfault判断で拒否する。保護変更・部分unmapは対象mappingへ
  適用され、独立したpeerのmappingを壊さない。
- truncateが全対象translationを除去する。無効化の完了を意図的に遅らせた間はpageを再利用せず、
  完了後に実回収する。遅れたfault完了からも、再利用された旧pageへアクセスできない。
- fault公開・無効化・process終了を確実に重ね、逐次的な模擬callbackだけで済ませない。
  client消失・操作中断・途中の初期化失敗でも、生存mappingや参照漏れを残さない。
- 既存の必須Gateを維持し、上位実装の置換や成功stubによって要件を満たしたことにしない。

## 検証結果の記録

この記録はMM／host mapping接続の完了を意味しません。CTest全48件が通過し、2 process結合試験の各ケースは
Linux警告0で50回連続通過しました。host transportは200 roundの停止／fault競合試験を100回
（計2万round）、非同期serviceは停止中clientの死亡を含めて200回通過しています。
host側の両試験は、通常の非instrumentation clientを相手にASan／UBSanでも通過しました。
試験したfull coreはnative built-in object 599個とinitcall target 129個を保持します。SHA-256：
`d0297c86e80ffe6c2e1dd1fbddecea2197c998c7f7c57ddb2d5bd6e4204cc480`。

結合試験が観測するのはLinuxがqueueへ入れたsignal判断であり、Linux userspace signal handlerの
実行ではありません。この過去の検証時点では、当時未検証だった寿命保証を認定しないよう、試験に
`intermediate-not-full-gate` labelを付けていました。

## 無効化完了応答とPFN再利用の検証

`--vm-probe-reuse`は、実際の両clientがアクセスした共有pageを対象にします。upstreamの
`vfs_truncate`中、試験用wrapperが実host resetを実行し、2つ目の完了応答を保留します。
もう一方の論理CPUは、flushが保持するregistry lockをIRQから待たないようIRQを禁止し、512 pageを
確保します。旧PFNが含まれないことに加え、固定RAMのPFN metadataにupstreamの参照が残ることも
確認します。停止中CPUのlocal free listに解放済みpageが残り、確保試験だけをすり抜けることを防ぎます。
試験用のfolio参照は保持しません。

応答後はpage cacheが空であることを確認し、upstream buddyから同じPFNを再取得して上書きします。
両processの旧aliasへの実アクセスがfaultし、再利用pageを読み書きせずLinuxのSIGBUS判断となることを
要求します。この試験はfault公開・process死亡との競合試験ではありません。それらは下記に残します。

このcaseをlauncherから実行・報告するため、process-localな試験descriptor／reportにcase選択と
観測counterを追加します。productionのhost operationsやwire interfaceは変更しません。

検証結果：再buildしたcoreでCTest全49件が通過しました。再利用caseは100回連続通過し、各回で
応答保留中の512 page確保、同じPFNの再取得1件、旧aliasの拒否2件、Linux警告0を確認しました。
coreはnative built-in object 599個とinitcall target 129個を保持します。SHA-256：
`9b8d9285b115e2bb4879b4631ec75ba33b4899df48e1311c75864dba765387b8`。
workspace内のログ：`.artifacts/kobox2-mm-vma-reuse-all-tests.log`、
`.artifacts/kobox2-mm-vma-reuse-repeat.log`。

## PTE公開・IRQ・process終了の競合検証

IRQ caseで修正前の循環待ちを再現しました。公開側はLinux IRQを許可したままtranslation lockを保持し、
flush側はregistry lockを保持してtranslation lock待ち、公開側のVM IRQはregistry lock待ちになります。
修正前coreは`MM race: publication IRQ queued`の出力後に停止し、外部の8秒期限を超過しました
（終了値124）。`publish_pte`を公開区間全体の`raw_spin_lock_irqsave`に変更しました。
upstreamのMM・scheduler・IRQ実装は変更していません。

実2 processで次のcaseを分けて検証します。

- `--vm-probe-irq`：CPU 1の実fault公開を保持し、CPU 0でarchの全体flushへ入り、registry lock
  保持中にVM IRQを投入して公開を完了させます。waitqueue上の観測で、公開後のCPU 1 hardirq
  contextへの配送を確認します。archの全体flush境界の直接試験であり、upstream reclaim policyの
  試験ではありません。
- `--vm-probe-truncate`：公開側がPTE lockを保持したまま実`vfs_truncate`を開始します。
  nativeの`i_size`が0になってから公開を進め、両mappingの無効化・PFN再取得・上書きを済ませた後に
  遅延faultの再開を許可します。両clientの実アクセスがLinuxのSIGBUS判断になることを要求します。
- `--vm-probe-exit-publish`：公開側のhost map前に、別CPUが実processをclose／回収してtruncate
  します。公開は成功counterを増やさず`-ESRCH`で失敗し、PFN再利用後も旧bindingへの古いmap／resumeを
  拒否することを確認します。
- `--vm-probe-exit`：公開・truncate・PFN再利用を進め、fault再開を保留したままclientをclose／回収
  します。古いsequenceと保留中の再開の双方を拒否し、生存peerも再利用pageを読まずfaultすることを
  確認します。

終了caseは試験側からhost close／回収を開始します。close要求によらないEXIT通知配送と参照の完全な回収確認は、
下記の寿命検証に残します。case選択・IRQ投入callback・counterの追加はprocess-localな試験
descriptor／reportだけで、productionのhost operationsやwire messageは変更しません。

検証結果：CTest全53件が通過しました。競合4 caseは各100回連続通過し、全回でLinux警告0、
公開の強制重複1件を確認しました。IRQ caseは公開後の配送を観測し、truncate／終了の3 caseは
保留中faultを進める前に旧PFNを再取得しました。full coreはnative built-in object 599個と
initcall target 129個を保持します。SHA-256：
`fe046b9cc2503f0bc955a78c68c9483bc447d574776a10e709e3f6b5df49edf4`。
workspace内のログ：`.artifacts/kobox2-mm-vma-race-all-tests.log`、
`.artifacts/kobox2-mm-vma-race-repeat.log`、修正前の再現ログ
`.artifacts/kobox2-mm-vma-race-before-irq.log`。

## 終了通知と最終参照回収

`--vm-probe-lifetime`はfileを先にtruncateせず、2つの共有mappingを破棄します。
`--vm-probe-death`は実Linux taskをbindingのevent waitqueueで眠らせ、CPUから離れたことを
確認してから、close／回収を呼ばずhostからSIGKILLを送ります。serviceがprocessの終了を観測・回収し、
VM IRQとupstreamの待機経路からEXITを一度だけ配送します。元の実行sequenceとcurrent／mmの一致を
検査し、次のevent取得は`-ESRCH`となり、生存peerは引き続き自身のmappingを読めることを確認します。

`--vm-probe-rollback`は最初のprocessでmappingと実アクセスを成立させ、2つ目のmm／service taskを
生成します。shmemへの`MAP_SHARED_VALIDATE | MAP_FIXED | MAP_SYNC`をnativeの`vm_mmap`が
`-EOPNOTSUPP`で拒否し、2つ目のmmにVMAがないことを確認して、両側を通常経路で破棄します。
実際に未対応のmapping要求を使い、確保成功の偽装やLinuxの失敗処理の置換はしません。
存在しないclientの起動失敗は、host service試験で別途検証します。

公開／終了競合を含むMM全10 caseに、`boot/vm_lifetime.c`の同じ最終回収検査を適用します。

1. 所有者が生きている間にnative cache、objectの数値アドレス、残存file PFNを記録します。
   mm／taskとinodeの観測用参照を明示的に保持しますが、folio参照は残しません。scratchは破棄前に
   確保し、動作を止めたpage cache全体をPFN記録が覆うことも確認します。
2. mm借用taskをjoinし、host clientを終了・回収してからbindingを外してnativeの`mmput`へ進みます。
   `kthread_use_mm`が保持するのは`mm_users`ではなく`mm_count`なので、前者だけでは利用者の終了を
   証明できません。
3. 各CPUで実kthreadを動かして`init_mm`へ切り替え、正当なnative lazy-mm参照を解消します。
   task work、遅延fput、RCUを排出し、旧mmのusersが0、countが観測用の1だけ、maple treeが空、
   RSSとpage-table bytesが0であることを確認します。停止taskはdeadかつCPU外で、mm／active_mmがなく、
   参照も観測用だけであることを確認します。
4. 観測用参照を外して実RCU callbackの完了を待ちます。native task解放は`arch_release_task_struct`へ
   到達し、pthreadをjoin・破棄します。旧mm 2個とtask 2個のslotを実slab cacheから再取得します。
5. VMAのfput排出後、file参照がcallerの1つだけであることを確認して解放します。fputを排出し、inodeも
   観測用参照だけであることを確認して解放します。LRUとRCUの処理を排出し、残存PFNをbuddyから再取得して
   上書きし、file／inodeのcache slotも再取得します。解放済みobjectは参照しません。

raw slab確保は領域再利用の検査であり、初期化済みLinux objectやsubsystemの代用品ではありません。
Linux task／MM／VFS APIには渡さず、同じupstream cacheへ戻します。参照数の強制クリアやallocatorの
方針変更で通すこともしません。通常・死亡・rollbackの各caseは、残っていたfile PFNの回収を必須とします。
truncateのcaseは、旧faultを再開する前にPFN回収を検証済みです。

process-local試験descriptor／reportに、寿命case選択、対象PIDを限定して検証するhost終了callback、
観測counterを追加します。productionのhost operationsとwire interfaceは変更しません。close済みhost
handleはservice破棄まで所有されたtombstoneとして保持し、その破棄と子processの回収はhost service試験で
確認します。このmemory GateでLinux runtime全体のshutdown方針を追加しません。

## 完了条件と検証の対応

| 必須条件 | 必須case |
| --- | --- |
| 実2 processのfault、mm識別、共有、RO、部分unmap | `probe`、`probe_ro` |
| 無効化応答前のpage再利用禁止、再利用後の旧alias拒否 | `probe_reuse` |
| 公開とIRQ・truncate・process終了の競合、古いmap／resume拒否 | `probe_irq`、`probe_truncate`、`probe_exit_publish`、`probe_exit` |
| 待機中Linux taskへの非同期EXIT、生存peer、途中設定失敗のrollback | `probe_death`、`probe_rollback` |
| 借用taskのjoin、参照排出、allocatorによる実回収 | `probe_lifetime`を含む全10 case |
| native hostの割込み・回収・起動失敗と既存Linux基盤 | transport／service試験とCTest全件 |

memory Gate全体は`ctest --test-dir BUILD -L linux-mm-vma`で選択します。全caseとfull-core load
fixtureを必須とし、単独caseの成功では完了にしません。検証対象artifactは以下の記録で特定します。

## 最終検証

最終coreでCTest全56件が通過しました。通常の寿命回収・非同期死亡・途中設定失敗の3 caseは
それぞれ100回連続通過しました。公開／終了競合を含む既存7 caseも、共通の回収検査追加後に各25回
連続通過しました。反復全475回でLinux警告0、mm 2個とtask 2個の参照排出・再取得、file／inode各1個の
再取得を確認しました。寿命3 caseは残存file PFNも再取得し、EXIT／rollback counterも期待値どおりです。

最終full coreはnative built-in object 599個とinitcall target 129個を保持します。SHA-256：
`27422d5e7d17a4af107278206ee3580cf3f8a0d0bccebb72e86b7de4844b18ce`。
workspace内のログ：

- `.artifacts/kobox2-mm-vma-final-build.log`
- `.artifacts/kobox2-mm-vma-final-all-tests.log`
- `.artifacts/kobox2-mm-vma-final-lifetime-repeat.log`
- `.artifacts/kobox2-mm-vma-final-regression-repeat.log`

compiler警告はありません。新しい寿命検査helperの`checkpatch`はerror 0、制御フロー付き検査macroへの
style warning 1件です。寿命検証のためのproduction subsystem置換、成功を偽装するstub、wire変更は
追加していません。上の完了条件対応表について、このmemory port Gate内に未検証の項目は残っていません。

## 今後の進め方

着手前に計画の前提を現行コードと照合し、不足があれば、必要な具体的操作・根拠・入れる工程・
完了試験を記録します。途中で判明した場合も、進路を変える前に追記します。Gateの黙った縮小、
名前のない将来Gateへの先送り、「未接続」だけの報告は禁止です。順序変更が必要なら、変更前後・
依存関係・影響するGateを実行前に明示します。途中の成功を根拠に最終の完了条件を変えません。
