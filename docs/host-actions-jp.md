# host action

`libkobox2`は一度に一つのactionを生成します。すべてのactionはnonzeroのtokenとsandbox
generationを持ち、hostは次のcommandまたはeventより先に同じ値で完了を返します。

| action | 入力 | 成功時の完了結果 |
|---|---|---|
| `ALLOCATE_RESOURCES` | closure view、launch digest、limit、導出済みflag | 新しい`resource_set_id` |
| `LAUNCH_SANDBOX` | closure view、launch digest、`resource_set_id` | 新しい`sandbox_id` |
| `TRANSFER_RESOURCES` | 両方のID | payloadなし |
| `QUIESCE_SANDBOX` | 両方のID | processが`STOPPED`を報告して終了 |
| `TERMINATE_SANDBOX` | `sandbox_id` | process終了 |
| `REVOKE_RESOURCES` | `resource_set_id` | payloadなし |
| `RESET_RESOURCES` | resource ID | payloadなし |
| `REAP_SANDBOX` | 終了済み`sandbox_id` | payloadなし |
| `RELEASE_RESOURCES` | resource ID | payloadなし |

IDはnonzeroかつopaqueで、一つのcontrollerとgenerationだけで有効です。native handleを
含みません。IDを返すのはallocationとlaunchだけです。失敗時はIDをzeroにし、
`RESOURCE_DENIED`、`RESOURCE_EXHAUSTED`、`HOST_FAILURE`のいずれかを返します。

startはallocation、launch、transfer、handshakeの順です。正常なstopとrestartはquiesce、
process終了、revoke、必要なreset、reap、releaseの順です。fault cleanupはterminate、process
終了、revoke、必要なreset、reap、releaseの順です。restartはその後、新しいIDで次のgenerationを
開始します。
