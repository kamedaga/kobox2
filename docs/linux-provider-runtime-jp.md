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

hosted portはLinuxをprocess hostへ接続するために必要な最下層のmachine境界だけを持ちます。Linux上では
native process/thread/wait機能へ接続し、Linux subsystem semanticsを変えずに同じ境界をPachaOSへ
差し替えられる形にします。

## 構造gate

boot-core inventoryはupstream x86 linkerの出力を使い、initcall、per-CPU、scheduler-class、
parameter、setup、init、core sectionが空でないことを検証します。また、`start_kernel`が
`init/main.o`由来であることと、現行profileのLinux upper API overrideが0であることを要求します。

別のdriver inventoryではload順・cleanup順が`.ko`だけで構成されることを要求します。これはlink構造の
gateであり、hosted architecture portによるLinux bootやGPU runtime完成を示すものではありません。
