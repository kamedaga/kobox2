# kobox2 agent notes（日本語版）

- 所有code、protocol serialization、文書pair、host integration境界には
  [`docs/coding-style-jp.md`](./docs/coding-style-jp.md)の規約を適用します。
- このリポジトリをhost非依存に保ちます。PachaOS固有のrole policyとpackagingは
  PachaOSリポジトリに置きます。
- Apache-2.0 controllerへLinux header、Linux構造体layout、module symbol、subsystem
  semantics、`.ko` loaderを入れません。
- GPL-only codeは`linux-sandbox` submoduleと独立runtime processに閉じ込めます。
  Apache-2.0 controller codeをそのprocessへlinkしません。
- wire messageにはpointer、host FD番号、host kernel object layoutを含めません。
- `linux-sandbox`と共有するfileは`protocol/`に置き、MITのSPDX identifierを維持します。
- controller API、host contract、transport、wire protocolを一つの`dev` interfaceとして
  扱います。明示的にfreezeするまでABI番号と互換性を割り当てません。

実際にagentへ適用される正本は[`AGENTS.md`](./AGENTS.md)です。
