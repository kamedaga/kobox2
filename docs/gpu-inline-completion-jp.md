# inline形式の同期GPU completion

`kb2_gpu_inline_completion_encode/decode`は既存completion headerとargument descriptorの
明示的な部分集合を実装する。dispositionは`COMPLETED`、command固有detailは0、span/attachment
descriptorはなし、inline結果argumentは1個またはデータなしとする。非OK statusではinline結果を
付けない。非同期submissionやattachment所有権は別途完全な実装が必要で、黙って受理しない。

wire field、ABI version、schemaは変更しない。VERSIONの出力spanはrequestで登録したものを使い、
responseで新たなspan権限を与えない。completionにはcanonical結果recordを入れ、transportの
completion公開時に外部出力storageを可視化する。

codecはidentity/digest、正確なsize/table offset、reserved field、status、session、inline
descriptorを検査する。ただし、commandに対してどの結果recordが許されるかは判断しない。
呼出し側がprivate保存したrequestと照合し、record ID・長さ・reserved・意味を検査する。
generationとcorrelationは外側のtransport envelopeにあり、処理中requestとの一致が必要。
sessionの一致だけでは足りない。

encode入力とdecode snapshotはcaller所有で、呼出し中は不変とする。encode入力dataと出力先は
重ねない。decode結果のdataは入力snapshotを借用する。エラーでは出力値とencode出力先を変更しない。
APIは割当を行わず、状態を保持しない。

`kobox2.gpu_protocol`で正常・エラーcompletion、切詰め、旧session、不正descriptor、未対応応答、
decode失敗時の出力不変を検証する。command固有の結果照合はhost integrationでも検証する。
# Device-backed CPU mapping

mapping completion の cache policy は `0` (WB)、`1` (WC)、`2` (UC) とする。
実際の属性は別送する memory capability が保持し、利用者による属性変更を許可
する値ではない。Linux 内部の mapping 結果では sandbox RAM の page index と
device physical page を区別する。後者はホストの PCI grant の BAR 範囲で検査し、
変更不能・shared・nonexec の memory capability として公開する。物理ページ番号を
クライアント protocol に載せない。revoke と cache retirement の完了後に BO を unpin する。

AMDGPU provider は CPU-visible VRAM を driver の `io_mem_pfn` で解決し、連続配置と
仮定しない。移動完了を待ち、mapping lease の間 BO を pin する。不可視 VRAM や
WB alias が残る WC RAM は明示的に未対応とし、cached RAM へ黙って移さない。
現在の PRIME は WB RAM のみ受け付け、device backing は公開前に拒否する。
