# resource grant

## model

closure manifestは不変のresource要求を宣言します。resource grant setは一つのclosure generationへ
割り当てたobjectを記録します。native handle mapはobjectをprocessのhandle転送機構へ対応付けます。

manifestはslot定義とnode bindingを所有します。grant setは実際のpresence、object ID、granted
rights、native handle roleを所有します。sandbox内のresource identityはgenerationとobject IDの組
です。明示的なABI freezeまではすべてidentity `dev`を使います。

各resource要求はresource typeとinterface schema digestを持ちます。typeはrights namespaceを選択し、
interface digestはoperation契約とnative handle roleを選択します。grantはslotが宣言したinterface
identityと完全に一致します。

## grant set

grant setはcanonicalな上限付きlittle-endian binary objectです。headerは次を持ちます。

- `dev` identityと正確なschema digest
- 全体sizeとtable範囲
- generationとclosure manifest digest
- slot、object、native handle bindingの各record数

bootstrap envelopeはgrant setのSHA-256 digestとsizeを持ちます。encoded objectはprocess起動前に
immutableにします。

recordは次のcanonical orderを使います。

1. slot ID順のslot grant
2. slot ID、object ID順のobject
3. object ID、role順のnative handle binding

manifestの各resource slotに対応するslot grantを一つずつ置きます。slot grantはslot ID、resource
type、interface digest、`PRESENT`または`ABSENT` state、object範囲を持ちます。

`ABSENT`はobject数とnative handle binding数がzeroのoptional slotを表します。`PRESENT`のobject数は
manifestのminimum以上maximum以下です。required slotは`PRESENT`です。

## objectとrights

各grant objectはnonzero 64-bit object IDと64-bit granted rightsを持ちます。object IDはgrant set内で
一意でcanonical object table全体を通して増加し、そのgeneration中だけ有効です。新しいgenerationには
新しいobject IDを割り当てます。

各present objectは次を満たします。

```text
required_rights subset-of granted_rights subset-of maximum_rights
```

同じslotのobjectはそれぞれ異なるgranted rightsを持てます。各objectはslot要求を満たします。shared
consumerは同じobject IDと同じunderlying objectを参照します。

sandbox registryは各resource operationでgenerationとrightsを検証します。native access restrictionが
対応するhost側の強制を行います。

## node visibility

manifest resource bindingをnode visibilityのauthorityとします。loaderはbindingとgrant setを組み合わせ、
nodeごとのresource viewを構築します。

- nodeは明示的にbindされた各slotを参照します
- exclusive slotは一つのnode viewを持ちます
- closure-shared slotは同じobjectをすべてのbinding先nodeへ公開します
- providerも明示的なbindingによってresourceを受け取ります
- absent optional slotはstate `ABSENT`として見えます

dependency edgeはsymbol availabilityを与えます。resource visibilityはresource bindingで定義します。
一つのclosure内のnodeはaddress spaceとtrust domainを共有し、別closureがsecurityとrevokeの分離を
提供します。

## module context

各artifact lifecycle entryは次の契約を使います。

```c
int entry(const struct kobox_module_context *context);
```

init、quiesce、cleanupへ同じimmutable contextを渡します。contextはinit entryからcleanup returnまで
有効で、次を持ちます。

- `dev` identityとstructure size
- generationとnode ID
- closureに固定されたlogical CPU count
- opaqueなnode resource view
- runtimeとcore operation

moduleはcore operationを通してslotを列挙し、opaque resource handleを取得します。handleはgenerationと
granted rightsを保持します。absent slot、invisible slot、stale handleは異なる結果になります。

取得したhandleのbindingには正確なinterface schema digestを指定します。成功するとclosure lifetimeの
opaque objectとinterface固有operation tableを返します。tableの先頭はstructure sizeと`dev` identity
です。sandbox registryはbindingを返す前にnode visibility、generation、interface digestを検証します。

## native handle map

native handle bindingはobject ID、interfaceが定義するrole、transfer handle indexを持ちます。一つの
objectはinterface schemaが定義する個数のhandleを持てます。roleはobject内で一意で、transfer indexは
完全な一つのcanonical sequenceを構成します。

handle indexはnative handle値から独立し、resource handle転送列内のzero-based indexです。Linuxは
受信した`SCM_RIGHTS` descriptor配列の該当部分から解決します。他のhostはnative handle transfer
tableから解決します。

Linuxの転送順は次です。

1. transport memory
2. canonical closure manifest
3. canonical resource grant set
4. manifest順のartifact object
5. native handle binding順のresource descriptor
6. channel順のtransport notification endpoint

受信descriptorはすべて`FD_CLOEXEC`を持ちます。manifest、grant set、artifact objectはwrite、grow、
shrink、sealの全sealを持ちます。resource descriptorはinterface schemaとgranted rightsに従って検証します。

## binding transaction

sandboxは次の順に起動します。

1. bootstrap、manifest、grant set、artifactのdigestを検証
2. graph、slot、object、rights、visibility、handleの全recordを検証
3. 全native objectを一時registryへimport
4. 全node resource viewを構築
5. registryをatomicにcommit
6. artifactをmapしてrelocation
7. dependency順にartifactをinit
8. `READY`を報告

import failure時は一時objectをすべて逆順にreleaseします。init failure時はinit済みnodeをcleanupし、
artifactをunloadし、resource viewを破棄してregistryをreleaseします。

## generation lifecycle

正常停止ではresource viewを破棄する前にmoduleをquiesceします。Linux descriptorのrevokeはprocess終了で
完了します。その後hostがresource setをrevoke、reset、releaseします。

restartは新しいgeneration、grant set digest、object ID、native handle、module context、resource viewを
生成します。resource lookupは以前のgenerationに属するobjectをstaleとして拒否します。
