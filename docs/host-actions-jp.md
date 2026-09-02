# host action

`libkobox2`は一度に一つのactionを生成します。すべてのactionはnonzeroのtokenとsandbox
generationを持ち、hostは次のcommandまたはeventより先に同じ値で完了を返します。

| action | 入力 | 成功時の完了結果 |
|---|---|---|
| `ALLOCATE_RESOURCES` | launch digest、limit、flag | 新しい`resource_set_id` |
| `LAUNCH_SANDBOX` | launch digestと`resource_set_id` | 新しい`sandbox_id` |
| `TRANSFER_RESOURCES` | 両方のID | payloadなし |
| `REVOKE_RESOURCES` | resource IDと、存在する場合はlive sandbox ID | payloadなし |
| `RESET_RESOURCES` | resource ID | payloadなし |
| `TERMINATE_SANDBOX` | sandbox ID | payloadなし |
| `RELEASE_RESOURCES` | resource ID | payloadなし |

IDはnonzeroかつopaqueで、一つのcontrollerとgenerationだけで有効です。native handleを
含みません。IDを返すのはallocationとlaunchだけです。失敗時はIDをzeroにし、
`RESOURCE_DENIED`、`RESOURCE_EXHAUSTED`、`HOST_FAILURE`のいずれかを返します。

startはallocation、launch、transfer、handshakeの順です。stopとrestartはrevoke、必要な
reset、terminate、releaseの順です。restartはその後、新しいIDで次のgenerationを開始します。
