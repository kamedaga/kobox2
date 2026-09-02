# コーディング規約

本文書はkobox2が所有するcodeと、`linux-sandbox`およびhost integrationとの境界に
適用する規約を定めます。upstream projectのコーディング規約を上書きしません。

## 共通規約

- textはUTF-8、LF改行、末尾改行あり、行末空白なしとします。
- 新しいsource fileには正しいSPDX license identifierを付けます。
  - kobox2 controller: `Apache-2.0`
  - `protocol/`配下: `MIT`
  - linux-sandbox code: `GPL-2.0-only`
  - host adapter: host repositoryのlicense（PachaOSでは`MIT`）
- Cのfile名、関数、変数には`snake_case`を使います。
- commentには理由、不変条件、所有権、分かりにくい制約を書きます。codeをそのまま
  言い換えません。
- API境界では所有権、寿命、thread safetyを明示します。隠れたglobal mutable stateを
  避けます。
- sizeやoffsetを計算する前に範囲を検証し、整数overflowを確認します。
- 成功を偽装するstubを追加しません。unsupported operationは明示的なerrorを返すか、
  文書化した不変条件で停止させます。
- compiler warningを残さず、成功経路と失敗経路の両方をtestします。
- project概要は`README.md`、実装詳細は`docs/`、強制的なrepository ruleは
  `AGENTS.md`に置きます。

英語文書を正本とします。利用者向けのkobox2所有Markdownには対応する`-jp.md`を置き、
同じ変更で更新します。upstream Linux文書は翻訳も書き換えも行いません。

## kobox2 controller

controller codeにはportable C11を使います。host非依存codeからcompiler extension、
Linux header、host固有の整数値やerror値へ依存しません。

- C sourceは空白4文字でindentし、tabを使いません。
- 1行は100文字以内を目安にします。
- 関数とcontrol statementの開始braceは同じ行に置きます。
- public headerは`include/kobox2/`、internal declarationは`src/`に置きます。
- public headerにはinclude guardを使い、`#pragma once`へ依存しません。
- publicな関数と型には`kb2_`、macroとenum定数には`KB2_`を付けます。
- 表現をstable ABIの一部として意図的に公開するまでは、public構造体をopaqueにします。
- public境界では`kb2_status_t`と名前付き`KB2_STATUS_*`値を使います。hostの`errno`や
  OS固有failureはhost adapterで変換し、controller APIへ漏らしません。
- allocatorとI/O operationを明示的に渡します。`malloc`、process-global state、特定の
  host runtimeへ暗黙に依存しません。
- caller-owned stateとcontroller state machineごとの単一ownerをdefaultにします。
  並行呼び出し可能なoperationはそのことを明記します。
- C testは`*_test.c`、再利用するtest fixtureは`*_fixture.c`と命名します。

controller所有codeにはrepositoryの`.clang-format`を使います。新しいcodeはC11として
ClangとGCCの両方でbuildし、最低でも`-Wall -Wextra -Wpedantic`のwarningを解消します。
project CIではこれらをerrorへ昇格できます。対応する環境ではhost testを
AddressSanitizerとUndefinedBehaviorSanitizerでも実行します。

## 共有protocol

protocolはserializeされたwire formatであり、native C構造体の共有ではありません。

- fixed-width integer fieldを使い、little-endianとして明示的にencodeします。
- pointer、host FD番号、`size_t`、native C enum、bit-field、host object layoutをwireへ
  含めません。
- shared memoryをC message構造体へ直接castせず、`packed` layoutへ依存しません。
  上限検査を行うhelperでfieldをdecode、encodeします。
- 利用前にすべてのoffset、length、alignment、object generation、completion generationを
  検証します。
- reserved fieldはzeroで送信し、現在のschemaで定義されていないnonzero値を拒否します。
- schemaを正本とし、生成headerやencoderを直接編集しません。
- controller、host contract、transport、protocol interfaceは明示的なABI freezeまで
  番号なしの`dev` statusを使います。
- `dev`では偶発的な互換性を約束するより明快な破壊的修正を優先し、関連する値を
  一貫した順序に保ちます。

## linux-sandbox

`linux-sandbox` repository内のLinux codeとkobox固有codeは、upstream Linux kernelの
`Documentation/process/coding-style.rst`と、repositoryに既存の`.clang-format`および
`.editorconfig`に従います。

- Cのindentには幅8のtabを使い、1行は80文字以内を目安にします。
- 関数の開始braceは次の行、control statementのbraceは同じ行に置きます。
- upstream Linuxの型、helper、subsystem、Kbuild integrationを使います。
- upstream fileを一括formatまたは機械的に書き換えません。
- upstreamへの変更は小さく、patchとしてreview可能に保ちます。
- Linux構造体offsetを数値定数として埋め込みません。
- kobox所有integrationは`kobox/`に置き、PachaOSの名前、policy、package path、
  syscall番号をこのrepositoryへ持ち込みません。

## host integration

host adapterはhost repositoryで確立された規約に従います。PachaOSの`gpud` integrationは
PachaOS userlandに合わせ、portable C11、空白4文字のindent、同じ行の開始brace、
100文字の行長目安を使います。

host adapterはnative handle、error、process bootstrap state、capability policyを境界で
変換します。これらの表現をkobox2 controllerや共有wire protocolの一部にしません。
