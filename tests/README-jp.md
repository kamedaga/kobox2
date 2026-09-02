# test

controller testはconfiguration拒否、lifecycle transition、generation rollover、host
failure、crash/restartを扱います。protocol testはround trip、境界、schema不一致、reserved
field、region right、message envelopeを扱います。

closure testはDAGの到達可能性、cycle、明示的なsymbol binding、resource rights、sharing、
seal、不変な参照を扱います。

Linuxではtest hostが独立したsandbox processを起動し、bootstrap socketを通じて一つの
shared-memory object、一つのcanonical closure manifest、不変なartifact object、四つの
eventfdを転送します。sandboxは`READY`の前にmanifestと全artifactのdigestを検証します。
split virtqueueによりpollingなしで
request completionとlifecycle eventを処理します。direct/indirect descriptor、
`EVENT_IDX`、wrap-around、region right、不正入力、quiesce、決定的なprocess fault、pidfdで
確認するprocess終了、revoke、reset、reap、再生成を検証します。device固有のacceptance testはhost OS
リポジトリに置きます。

GPL sandbox fixtureはmanifestの依存順で、転送されたobjectから`fixture_core.so`をloadし、
ET_RELの`fixture_module.ko`を再配置します。宣言されたimportを明示的なprovider namespace
から解決し、graph順にinit、quiesce、cleanup entryを呼びます。moduleはallocation、lock、wait/wake、二本のnative
thread、per-CPU state、RCU grace period、monotonic timeを実行し、request virtqueueへ結果を
返します。
