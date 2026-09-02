# architecture境界

## controller

kobox2 controllerは、PachaOSの`gpud`などhost側serviceが利用するlibraryです。
lifecycle state、不変なrole closureの検証、channel確立、上限付きresource transfer、
restart generationの監督を担当します。

device data pathは所有せず、Linux APIも解釈しません。

controller API、host contract、transport、role protocolは一つの`dev` interfaceです。
明示的にfreezeするまでABI番号を割り当てません。

## Linux sandbox

GPL-2.0-onlyのsandboxは独立processです。`.so`/`.ko` self-loader、Linux core
primitiveとsubsystem state、device queue、DMA mapping、IRQ処理、Linux driver objectを
所有します。

各roleにはmanifestで宣言したresourceだけを渡し、宣言したdependency closureだけを
loadします。GPU roleとstorage roleのLinux runtime state、DMA domain、generation、
device capabilityは共有しません。

## host側integration

host OSがservice名、process bootstrap、role manifest、device選択、capability policy、
packagingを所有します。そのためPachaOS portはこのリポジトリではなくPachaOS
リポジトリに置きます。

最初の開発順は次のとおりです。

1. sandbox lifecycleとfixture module
2. virtio-gpu VirGL command submissionとrender node
3. PachaOS `gpud`経由のMesa、Xorg、Xfce GPU acceleration
4. 同じGPU protocolを使うRX 9060 XT上のAMDGPU
5. 同じcontroller境界を使うNVMeとext4

virtio-gpu 2D renderingはmilestoneにもfallback経路にも含めません。VirGLのgateでは
実際の3D submissionを必須とし、llvmpipe/swrast fallbackを拒否します。
