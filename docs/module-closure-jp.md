# module closure

## 実行単位

closureは一つのsandbox processと一つのgenerationが持つ不変の内容です。一つ以上のroot
module、推移的に依存するすべてのmoduleとprovider、明示的なsymbol binding、resource slot、
lifecycle entryを含みます。closureの一部を変更すると新しいgenerationを生成します。

manifest digestはcanonicalなpackage manifest全体に対するSHA-256です。artifact digest、graph
record、symbol binding、lifecycle entry、resource slot、rootを対象に含みます。controllerはdecode
済みdescriptionとdigestを受け取り、hostとsandboxはpackage manifestとartifact objectの転送時に
検証します。

closure interfaceはすべてidentity `dev`を使います。明示的なfreezeまではABI versionと互換性
保証を持ちません。

## manifest graph

各artifact recordは次を持ちます。

- closure内で一意なnonzero node ID
- shared providerまたはrelocatable moduleを示すartifact kind
- 不変のcontent digest
- namespace名
- 明示的なinit、quiesce、cleanup entry symbol
- entry moduleを示すroot marker

dependency recordはconsumerから直接providerへのDAGを構成します。すべてのnon-root nodeはroot
から到達可能です。複数consumerが共有するproviderは一度だけload・initします。独立したnodeは
node ID順とし、load順とlifecycle順を再現可能にします。shared providerのdependencyはshared
providerで構成します。

seal済みdescriptionはartifactをnode ID、dependencyをconsumer/provider ID、symbolをowner/name、
resourceをslot ID、bindingをslot/node IDの順に保持します。

package manifestはresourceとして転送する不変かつ上限付きのobjectです。artifactはhost pathや
native handleではなくnode IDとdigestで識別します。

## process転送

canonical manifestはshared MIT schemaで定義する上限付きlittle-endian binary objectです。headerは
`dev` identity、正確なschema digest、全体size、各table範囲を持ちます。固定size tableがartifact、
dependency、export、import、resource、resource bindingを表し、nameは末尾のbyte tableに置いて
検証済みoffsetとlengthで参照します。

recordはseal時に確定した順序を使います。manifest digestはencoded object全体に対するSHA-256です。
artifact recordはartifact objectのSHA-256とbyte lengthを持ちます。

bootstrapはresourceを次の順に転送します。

1. transport memory
2. canonical closure manifest
3. canonical resource grant set
4. manifestのartifact順に並べたartifact object
5. grant binding順のresource handle
6. channel順のnotification endpoint

bootstrap envelopeはgeneration、manifestとgrantのdigestとsize、artifact数、resource handle数、
notification数、transfer総数を持ちます。manifest、grant set、artifact objectはprocess起動前に
immutableにします。sandboxはcodeをloadして`READY`を報告する前にpackage全体を検証します。

## symbol binding

各export symbolはartifact namespaceに属します。import recordは一つのconsumer symbolを一つの
provider nodeとexport symbolへbindingします。

```text
consumer symbol -> provider node -> provider namespace -> exported symbol
```

node zeroはsandbox runtime namespaceを表します。runtime importはhost profileのallowlistで
受理します。artifact importは直接dependencyと、そのdependencyが宣言したexportを指定します。
ELF local symbolはprivateです。strong importは一つのbindingを持ちます。weak importはrecordが
optionalの場合に限って不在を表現でき、その解決値はzeroです。

shared providerのdynamic dependencyはclosure dependencyまたはallowlist済みsandbox runtime
importで表現します。解決にはmanifest bindingと指定artifactのsymbol tableを使い、process全体の
interpositionは介在しません。

## resourceとrights

resource要求は次を持つsymbolic slotです。

- closure内で一意なnonzero slot ID
- resource type
- interface schema digest
- requiredまたはoptional
- object数のminimumとmaximum
- required rightsとmaximum rights
- exclusiveまたはclosure-shared access
- reset policy
- bindingを受け取るnode ID

rightsの意味はresource typeごとに定義します。

| resource | rights |
|---|---|
| memory | `READ`, `WRITE`, `MAP`, `DMA` |
| device | `COMMAND`, `MAP`, `DMA` |
| storage | `READ_BLOCKS`, `WRITE_BLOCKS`, `FLUSH`, `DISCARD` |
| notification | `WAIT`, `SIGNAL` |
| channel | `SEND`, `RECEIVE` |

host policyは宣言されたmaximum内でrightsをgrantします。required slotはobject数とrequired rightsを
満たした時点で有効です。optional slotの不在は明示的に表現します。exclusive slotは一つの
consumer nodeを持ち、closure-shared slotはすべてのconsumerを列挙します。dependency edgeはsymbol
accessを与え、resource bindingはresource visibilityを与えます。

各resource setはcontrollerが所有します。nodeはclosureのlifetime中だけgeneration-scopedな
resource IDを受け取ります。native handleはhost adapterが解決します。transport regionのaccess
rightsとclosure resource rightsは別のdomainです。

generationごとのgrant形式、node resource view、module context、native handle mappingは
[resource-grants-jp.md](./resource-grants-jp.md)で定義します。

## lifecycle

起動順は次です。

1. manifest、grant set、artifactの全digestを検証
2. resourceを一時registryへimport
3. node resource viewを構築してregistryをcommit
4. すべてのartifactをmap
5. すべてのrelocationとsymbol bindingを完了
6. dependency順にproviderをinit
7. root moduleをinit
8. `READY`を報告
9. request受付開始

initが失敗した場合は、init済みnodeを逆init順にcleanupします。各nodeはinit failureを返す前に、
自身の部分的な変更をrollbackします。

正常停止順は次です。

1. request受付停止
2. rootからproviderへ逆dependency順にquiesce
3. requestとcallbackをdrain
4. 逆init順にcleanup
5. module、shared providerの順にunload
6. node resource viewとsandbox registryを破棄
7. `STOPPED`を報告してsandbox processを終了
8. process終了を確認
9. resourceをrevoke
10. policyが要求するresourceをreset
11. process recordをreap
12. resourceをrelease

正常系のinit、quiesce、cleanup entryは同じimmutable module contextを使って各nodeにつき一回実行
します。quiesce deadlineの失敗はclosure faultになります。

## Linux closure loader

Linux sandboxのclosure loaderは、一つのdecode済みmanifest、一つのdecode済みgrant set、そこに
記載された正確なartifact列とresource handle列を受け取ります。codeをmapする前にgraph、symbol、
resource、rights、visibility、native handle map、artifact size、artifact digestを検証します。

loaderは決定的なtopological orderを計算し、すべてのartifactをmapし、各importを指定providerから
解決し、各nodeを一回だけinitします。外部へ公開するのはnode IDとsymbol名で宣言されたexportだけ
です。lifecycle exportは`int entry(const struct kobox_module_context *context)`形式です。

resourceはtransactionとしてimportし、artifact mapより前にnode resource viewを通して公開します。
sandbox-runtime importは別のresolver callbackとnode zeroを使います。closure importは直接providerの
export tableだけから解決します。

quiesceとcleanupは逆topological orderで行います。init failure時はinit済みnodeを逆順にcleanupし、
map済みartifactをすべてunloadし、resource viewを破棄してregistryをreleaseします。relocatable
moduleはshared providerより先にunloadします。

## 複数module

一つのclosureは複数のroot moduleを持てます。closure内の全nodeはaddress space、trust domain、
fault domain、resource revoke単位、restart generationを共有します。共有dependencyは一つの
instanceと一つのlifecycleを持ちます。

独立したrestart、hardware isolation、resource revokeを必要とするdriver instanceは別closureを
使います。closure間の通信には宣言済みchannelを使います。

## fault処理

closure全体をfault単位とします。loader、provider、module、protocol、resource、processのfaultは
generationを`FAULTED`へ移し、最初のfaultをその結果として確定します。

fault cleanup順は次です。

1. request受付停止
2. sandbox processをterminate
3. process終了を確認
4. resourceをrevoke
5. policyが要求するresourceをreset
6. process recordをreap
7. resourceをrelease

fault経路ではprocess内のlifecycle callbackを完了条件に含めません。未完了requestはgeneration
failureとして完了します。以前のgenerationを持つnotificationとcompletionは拒否します。

## controller API

`kb2_closure_builder_t`がdescriptionを構築して検証します。sealすると不変の`kb2_closure_t`を生成し、
controllerはidle configuration時にその内容をcopyします。callerはconfiguration後に元のclosureを
破棄できます。

`kb2_controller_start`は新しいsandbox generationを開始します。順序付きhost actionはread-onlyな
closure view、digest、limit、opaqueなresource IDとsandbox IDを持ちます。host adapterがnativeな
allocation、process生成、grant set構築、transfer、正常なprocess停止またはterminate、revoke、reset、
reap、releaseを実行します。
