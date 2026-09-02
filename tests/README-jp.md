# test

controller testはconfiguration拒否、lifecycle transition、generation rollover、host
failure、crash/restartを扱います。protocol testはround trip、境界、schema不一致、reserved
field、region right、message envelopeを扱います。

Linuxではtest hostが独立したsandbox processを起動し、bootstrap socketを通じて一つの
shared-memory objectと四つのeventfdを転送します。split virtqueueによりpollingなしで
request completionとlifecycle eventを処理します。direct/indirect descriptor、
`EVENT_IDX`、wrap-around、region right、不正入力、quiesce、決定的なprocess fault、pidfdで
確認するrevoke、reset、reap、再生成を検証します。device固有のacceptance testはhost OS
リポジトリに置きます。

GPL sandbox fixtureは`fixture_core.so`をloadし、ET_RELの`fixture_module.ko`を再配置して
initとcleanup entryを呼びます。moduleはallocation、lock、wait/wake、二本のnative
thread、per-CPU state、RCU grace period、monotonic timeを実行し、request virtqueueへ結果を
返します。
