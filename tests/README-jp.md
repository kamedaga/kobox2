# test

controller testはconfiguration拒否、lifecycle transition、generation rollover、host
failure、crash/restartを扱います。protocol testはround trip、境界、schema不一致、reserved
field、region right、message envelopeを扱います。

closure testはDAGの到達可能性、cycle、明示的なsymbol binding、resource rights、sharing、
seal、不変な参照を扱います。

Linuxではtest hostが独立したsandbox processを起動し、bootstrap socketを通じて一つの
shared-memory object、一つのcanonical closure manifest、一つのcanonical resource grant
set、不変なartifact object、一つのresource handle、四つのeventfdを転送します。sandboxは
`READY`の前に両schemaとdigest、manifest/grant binding、全artifact digest、resource handle
mappingを検証します。
split virtqueueによりpollingなしで
request completionとlifecycle eventを処理します。direct/indirect descriptor、
`EVENT_IDX`、wrap-around、region right、不正入力、quiesce、決定的なprocess fault、pidfdで
確認するprocess終了、revoke、reset、reap、再生成を検証します。device固有のacceptance testはhost OS
リポジトリに置きます。

GPL sandbox conformance targetは`fixture_core.so`、`fixture_provider.ko`、
`fixture_consumer.ko`で構成します。testは依存・symbol解決、resourceの可視範囲・rights、
native thread、per-CPU state、RCU、time、逆順lifecycle、init rollback、export集合、
stale generation、fault/restartを検証します。
