# Linux sandbox runtime境界

## build・process単位

一つのsandbox processが一度だけLinuxをbootします。

```text
sandbox process
  -> upstream vmlinux
       -> init/main.o:start_kernel
       -> upstream initcallとsubsystem
       -> 選択した.ko module
```

Linux coreはboot rootから固定した一つのartifactです。GPU closureが選ぶのはdriver moduleだけで、
Linux coreの断片は選びません。

shutdownではload済みmoduleをquiesceしてからprocessを終了します。coreを巻き戻さず、同じprocessで
二度目のLinux bootは行いません。

## ownership境界

task state、scheduler class、per-CPU state、waitqueue、mutex、completion、kthread、workqueue、
timer、softirq、RCU、page metadata、driver subsystemはLinuxが所有します。Koboxはこれらの上位APIを
置き換えません。

hosted portはLinuxをprocess hostへ接続するために必要な最下層のmachine境界だけを持ちます。Linux PoCは
native process/thread/wait機能を使い、Linux subsystem semanticsを変えずに同じ境界をPachaOSへ
差し替えられる形にします。

## POSIX基盤gate

Linux PoCのhost surfaceはpthread、counting permit、`CLOCK_MONOTONIC`、one-shot timer、memory
mapping、POSIX非同期通知だけに制限します。import gateは`futex`、`eventfd`、`timerfd`と未宣言host
dependencyを拒否します。

Linuxへ接続する前に、独立した2 logical-CPU domainを試験します。同じCPUへの進入直列化、異なるCPUで
重なる実行、CPU-bound threadへのtick・IRQ配送、IRQ disable中の保持とenable時配送、park前にpostした
permitの保持をすべて必須にします。

[LKLのthread・semaphore hook](https://github.com/lkl/linux/blob/master/arch/lkl/kernel/threads.c)は
境界の参考になりますが、[architectureが`!SMP`を選択します](https://github.com/lkl/linux/blob/master/arch/lkl/Kconfig)。
この固定treeのUMLもCPU ID zeroだけを公開します。どちらもこのSMP gateの証拠には使いません。

## Linux task/SMP gate

[task/SMP fixture](../linux-sandbox/kobox/task/README-jp.md)は実Linuxの`schedule()`、wakeup、
affinity、CPU stopperによるmigration、task exitをpthreadへ接続します。host境界を越えるのは
taskのstart／switch／release、logical CPU識別、IRQ／IPI配送、clock取得、CPU別one-shot deviceの設定
だけで、hostはtask選択やLinux timer queue管理を行いません。

2 CPUのgateではlocal／remote切替、currentとper-CPUの一致、sleep中と実行中のmigration、
preempt禁止、IRQ保留、Linux exit後のnative joinを検証します。idle待機の回帰試験では、配送前に
通知sequenceを既に読んだpending IPIも検証します。

IRQ/time拡張ではupstreamのRCU idle／IRQ遷移、hardirq context、CPU別tick進行、high-resolution
hrtimer、timer-wheel callback、遅延IRQ配送、古い通知を伴うcancel／rearm、各CPUのbusy kthread
2つのtickによる反復切替を検証します。static key初期化とjiffies状態もLinux由来であり、
`BUG`と`RCU_EQS_DEBUG`は有効なままです。

この独立subsystem fixtureは製品用coreの別build方式ではありません。後続phaseの未接続部分は
実行時に失敗するguardであり、完成したdriver runtimeとしてloadしてはいけません。
時間付き待機API全体と残量のmatrix、RCU grace period／回収、workqueue実行、完全なCPU hotplug／
shutdownは認定しません。
上記のboot rootから固定するcore設計は変わりません。

## boot／service統合Gate

[boot統合Gate](../linux-sandbox/kobox/boot/README-jp.md)は実initcall・CPUHP・boot終端の
メモリ保護を通り、PID 1のhosted入口へ到達します。ksoftirqdへの超過分移行、tasklet／irq_work、
基本kworker・RCU callbackの進行を検証し、同じbootで2 CPU IRQ／SMP／time試験を再実行します。
これは工程3のGateであり、時間付き待機・RCU・workqueue全体やGPUの認定ではありません。

## upstream時間付き待機Gate

[工程4のGate](../linux-sandbox/kobox/boot/wait-gate-jp.md)は同じbootの後に実行し、
upstreamのjiffy／高精度待機、waitqueue、completion、sleep APIを両CPUで検証します。
期限切れ、早期／schedule前のwake、実signalによる中断・非中断、API固有の戻り値、
繰り返しwake後の残量を確認します。無関係なhard clockevent割込みでは対象taskを起床させません。
上位の待機実装やhost ABIは追加しません。RCU・workqueue・cleanupのsemanticsは別Gateで扱います。

## upstream Tree RCU／SRCU Gate

[工程5のGate](../linux-sandbox/kobox/boot/rcu-gate-jp.md)は両CPUでpreemptible Tree RCUと
dynamic／static SRCUを検証します。nesting・preempt・migration・idleを含めてreaderが回収を
防ぎ、unlock後にnormal／expedited GP、callback、barrier、実SLUB解放が完了することを確認します。
通常RCU readerは自発的にsleepせず、sleepするreaderの検証はSRCUに限ります。
実行中callbackを伴うbarrier再確認によりPOSIX dispatcherのcallback全体にわたるIRQ抑止を
検出しました。修正したのはmachineのIRQ入口／出口契約だけで、上位RCU／SRCUは置換していません。
workqueueと統合cleanup競合は別Gateで検証します。

## upstream workqueue Gate

[工程6のGate](../linux-sandbox/kobox/boot/workqueue-gate-jp.md)は対象のnormal／highpri／BH／
unbound／ordered、delayed work、cancel／flush、自己再投入、ordered FIFO、実行中のunbound
affinity変更を検証します。実Linux RAM不足時にupstream mayday／rescuerが保持pageを解放し、
両CPUで進行を回復することを要求します。host操作や上位実装は追加しません。
固定upstreamではdrain中のBH再投入をchained workと認識しないため、BHの両動作は別々に
検証し、その組合せは認定しません。IRQ／timer／work／RCUの統合寿命は次の工程7のGateで扱います。

## 統合cleanup Gate

[工程7のGate](../linux-sandbox/kobox/boot/cleanup-gate-jp.md)は投入遮断とRCU公開解除を同じlockで
行い、fixtureの実依存関係に沿ってIRQ／thread同期、timer／work停止、公開reader GP、callback barrier、
final work排出を経て実`vfree()`します。deviceのhost alias無効化後もIRQ源の試行を続けます。
2 CPUの52ケースで、実際の同期待機20回、遅れた投入の拒否、callback停止、deviceごとの一度だけの解放を
要求します。reader GP・callback完了・callbackが投入したworkの完了は別条件です。
host操作・上位実装の置換・汎用teardown runtimeは追加しません。実PCI／DMA／GPUのremoveやprocess revokeは
別のdevice／統合Gateであり、合成IRQ fixtureの成功から成立したとは判断しません。

## VFS寿命Gate

[第2章のVFS Gate](../linux-sandbox/kobox/boot/vfs-gate-jp.md)は同じ実shmem boot core上でmount・
名前付きfileの読み書き・Linux FDのclose／unlink・再open・file／inode／folioの独立した寿命を検証します。
task work・kthread delayed-fput・inodeのRCU callback・superblockのcallbackが投入するworkを区別し、
回収後の実allocatorによる再利用まで要求します。runtime port・host操作・上位実装・サービス初期化fixtureは
追加しません。user-copy・共有mapping・外部DRM clientは別のGateです。

## shmem／page cache Gate

[shmem／page cache Gate](../linux-sandbox/kobox/boot/shmem-gate-jp.md)は共有backing・pinしたkernel alias・
疎な範囲／部分pageのゼロ化・truncate／hole punch競合・確保失敗の巻き戻し・block／inode／commitの
正確な回収を追加します。両CPUと事前予約／増分会計の4ケースで、最後のpin解放後の実PFN再利用を
要求します。追加するのは試験であり、上位実装やmachine portではありません。user VMAとpressureは別Gateです。

## 構造gate

第2章は[正規boot memory Gate](../linux-sandbox/kobox/boot/memory-gate-jp.md)から進めます。
必須の`boot/config`で実shmem／tmpfsを選択し、毎回の正規boot後にpage・SLUB・static/dynamic per-CPU・
疎な／共有vmapを検証します。fixtureからサービス初期化せず、実shmemのfile／folio経路とevictionも確認します。
`--all`は同じboot内で第1章の全経路を再検証します。mapping portは疎な範囲とPTE除去後／遅延TLB無効化に
従うよう修正し、Linux allocator・VFS／shmemのpolicyは変更しません。

boot-core inventoryはupstream x86 linkerの出力を使い、initcall、per-CPU、scheduler-class、
parameter、setup、init、core sectionが空でないことを検証します。また、`start_kernel`が
`init/main.o`由来であることと、現行profileのLinux upper API overrideが0であることを要求します。

別のdriver inventoryではload順・cleanup順が`.ko`だけで構成されることを要求します。これはlink構造の
gate単独であり、Linux bootの実行やGPU runtime完成を示すものではありません。実行Gateは別です。
