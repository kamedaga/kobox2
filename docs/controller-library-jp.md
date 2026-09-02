# controller library

## 契約

`libkobox2`はhost serviceへlinkするportable C11 libraryです。一つのcontroller objectが
一つのsandbox instanceを管理します。起動記述を検証し、lifecycleを遷移させ、順序付きの
host actionを生成します。

libraryはI/Oもthread生成も行わず、Linuxやhost OSのsemanticsを持ちません。hostがactionを
実行し、その結果を返します。

## state machine

各呼び出しは一つのcommandまたはeventを渡します。成功時は一つの決定的な遷移を確定し、
必要なactionを生成します。不正な入力ではcontrollerを変更しません。

startではsandboxがrunningになる前に新しいresourceとgenerationを確立します。stopとrestartは
revoke、deviceがある場合のreset、process終了、resource解放、次のstartの順で進めます。
異なるgenerationの結果は拒否します。

## interface

- launch descriptionはopaqueなmanifest/profile digest、resource limit、capability、channel
  descriptionを持ちます。
- eventはaction完了、sandbox handshake、process終了、protocol fault、hostからのstop/restart
  requestを通知します。
- actionはtype、generation、token、上限付きargumentを持ちます。native handleはhost adapterが
  解決します。
- statusには`kb2_status_t`を使い、host errorはadapterで変換します。

public controller typeはopaqueです。callerが各objectのlifetimeを所有し、呼び出しを直列化します。
libraryはcallerのpointerを保持せず、明示的なallocatorだけを使い、host operationを出力actionで
表現します。

## 開発版の契約

controller API、host contract、transport、role protocolは一つの`dev` interfaceです。ABI番号と
互換性保証は持ちません。すべてのcomponentを一致するsource revisionとschema digestでbuild
します。

最初のABI番号は、conformance fixture通過後に明示的にfreezeするときだけ割り当てます。
