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

outstanding actionは一つまでです。hostは次のcommandまたはeventを渡す前に完了を報告し、
実行failureも明示的なresultとして返します。

startではsandboxがrunningになる前に新しいresourceとgenerationを確立します。正常なstopと
restartはclosureをquiesceし、process終了を確認してからrevoke、reset、reap、resource解放、
次のstartの順で進めます。fault cleanupはprocessをterminateしてから同じresource cleanupを
行います。異なるgenerationの結果は拒否します。

## interface

- launch descriptionは検証済みの不変なclosure、profile/channel digest、resource limitを
  持ちます。reset flagはclosureのresource policyから導出します。
- eventはaction完了、sandbox handshake、process終了、protocol fault、hostからのstop/restart
  requestを通知します。
- actionはtype、generation、token、上限付きargumentを持ちます。native handleはhost adapterが
  解決します。
- statusには`kb2_status_t`を使い、host errorはadapterで変換します。

public controller typeはopaqueです。callerが各objectのlifetimeを所有し、呼び出しを直列化します。
libraryはconfiguration dataをcopyし、明示的なallocator callbackとcontextだけを保持します。
host operationは出力actionで表現します。

## 開発版の契約

controller API、host contract、transport、role protocolは一つの`dev` interfaceです。ABI番号と
互換性保証は持ちません。すべてのcomponentを一致するsource revisionとschema digestでbuild
します。

ABI番号は、conformance fixture通過後に明示的にfreezeするときだけ割り当てます。

action契約は[host-actions-jp.md](./host-actions-jp.md)で定義します。
closure契約は[module-closure-jp.md](./module-closure-jp.md)で定義します。
