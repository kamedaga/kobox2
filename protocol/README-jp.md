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

Transport channel は1つ以上の request lane と、0または1つの application-event lane
を持ちます。Filesystem RPC に event stream はなく、ring の進捗を知らせる doorbell
を application event と見なしてダミーの GPU event lane を作る必要はありません。
Queue 深さは split-ring で表現できる2のべき乗を交渉し、GPU 由来の最低16には縛りません。
これは番号を持たない `dev` schema の刷新であり、旧 schema への fallback は作りません。
既存 GPU profile は、その実際の event lane を host policy で引き続き検証します。

`schema/filesystem.json` は client・中継・storage 共通の操作一覧と新しい filesystem
`dev` request/response header を定義します。旧 filed の操作番号、cache generation 操作、
root 専用操作、短い名前配列、inline 転送上限は引き継ぎません。data と path は重複しない
範囲検査済み span で運び、queue の容量は filesystem のサイズ制限とは区別します。
pathname と filesystem の制限は Linux に任せます。中継専用の資格情報は、信頼できない
client の要求からコピーせず、認証済み owner state から付与する必要があります。
codec の範囲検証自体は権限を付与せず、一覧の全操作が service に実装済みとも意味しません。

Filesystem capability 権限は protocol で定義し、Linux の credential capability と
DAC とは別に維持します。owner が Linux 資格情報とともに登録・更新し、data dispatch
はその組を原子的に取得します。`kb2_fs_request_policy` の検査と VFS 実行には同じ
private request snapshot を使います。共有 request は自分に権限を付与したり、owner
dispatcher を選んだりできません。全権限を持つ client でも、管理・frontend 操作を
data dispatch から実行できません。bound data request の client と credential generation
がゼロなら、登録済み queue の client と現在の資格情報を選びます。非ゼロの値は
整合性検査にだけ使い、権限の根拠にはしません。
