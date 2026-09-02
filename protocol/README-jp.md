# wire protocol

このディレクトリのfileは、リポジトリ既定のApache-2.0とは独立してMIT licenseを
適用します。生成headerは次の行で始めます。

```text
SPDX-License-Identifier: MIT
```

すべてのschemaは`dev`です。明示的にfreezeするまでABI番号と互換性保証を持ちません。

`schema/`以下のfileを正本とし、各RFC 8785表現のSHA-256をdigestとします。
`tools/generate_protocol.py`が検証し、repositoryへ含めるlayout定数を生成します。生成headerが
古い場合はbuildが失敗します。公開codecはnative structureをwire dataへ重ねず、範囲を検証した
little-endian byte列を読み書きします。

`kobox2_protocol` library targetはcontroller libraryから独立し、test sandbox processへ
linkするkobox2 targetはこれだけです。
