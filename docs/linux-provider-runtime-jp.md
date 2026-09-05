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
taskのstart／switch／release、logical CPU識別、IRQ／IPI配送、clock取得だけで、hostはtaskを選びません。

2 CPUのgateではlocal／remote切替、currentとper-CPUの一致、sleep中と実行中のmigration、
preempt禁止、IRQ保留、Linux exit後のnative joinを検証します。idle待機の回帰試験では、配送前に
通知sequenceを既に読んだpending IPIも検証します。

この独立subsystem fixtureは製品用coreの別build方式ではありません。後続phaseの未接続部分は
実行時に失敗するguardであり、完成したdriver runtimeとしてloadしてはいけません。
timer進行、RCU grace period、workqueue実行、完全なCPU hotplug／shutdownは認定しません。
上記のboot rootから固定するcore設計は変わりません。

## 構造gate

boot-core inventoryはupstream x86 linkerの出力を使い、initcall、per-CPU、scheduler-class、
parameter、setup、init、core sectionが空でないことを検証します。また、`start_kernel`が
`init/main.o`由来であることと、現行profileのLinux upper API overrideが0であることを要求します。

別のdriver inventoryではload順・cleanup順が`.ko`だけで構成されることを要求します。これはlink構造の
gateであり、hosted architecture portによるLinux bootやGPU runtime完成を示すものではありません。
