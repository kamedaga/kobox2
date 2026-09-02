# test

controller testはconfiguration拒否、lifecycle transition、generation rollover、host
failure、crash/restartを扱います。protocol testはround trip、境界、schema不一致、reserved
field、region right、message envelopeを扱います。

Linuxではtest hostが独立したsandbox processを起動し、bootstrap socketを通じて一つの
shared-memory objectと四つのnotification objectを転送します。shared memory上の応答と
notificationを確認し、restartではrevoke、process終了、新しいobject、新しいgenerationを
検証します。device固有のacceptance testはhost OSリポジトリに置きます。
