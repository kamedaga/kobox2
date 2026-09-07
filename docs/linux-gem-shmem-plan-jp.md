# upstream GEM shmemのbuffer寿命管理

Linux host上のbuffer寿命を対象とし、MM／host mapping Gateに続く工程です。
GPU実行の完成とは扱わず、合意済みの実装順序も変更しません。

## 必須の結果

object／handleの所有権、backing page、pin／unpin、vmap／vunmap、mmapにはupstream GEM shmemを
使います。複数参照・mappingで同じ内容が見え、handleを閉じても既存mappingが有効であることを
要求します。全mapping・pin・vmapの利用終了と最後のobject参照解放後に、実objectとpageを回収します。
参照counterやdestructor呼出しだけでは合格にせず、旧対象の参照を残さずallocatorから再取得します。

固定boot-rooted Linux coreは共有objectとし、その上で実upstream DRM／GEM moduleを動かします。
kobox2はmachine境界だけを担当し、GEMの代用品は作りません。userspace mappingには実2 processの
MM／fault transportを使います。

## 前提条件

- 固定configではDRMとGEM shmemはmoduleです。full boot artifactが含むのはnative built-inであり、
  これらのmodule実装は別途必要です。
- `drm_dev_init()`にはnativeの`drm_core_init()`完了が必要です。device／file、handle表、
  mmap offset管理の初期化も、このGateのsetupに含めます。
- `drm_gem_mmap()`はnative mmap offsetの権限を検査し、VMA用の独立したobject参照を取得します。
  handle閉鎖は以後のoffsetアクセス権を取り消しますが、既存VMAの参照は消しません。
- native shmem GEM mmapは`VM_PFNMAP`と`drm_gem_shmem_fault()`の`vmf_insert_pfn()`を使い、
  普通のshmem file faultとは異なります。client実アクセスでこの経路を検証し、普通のshmem file
  mappingで代用しません。
- nativeのmodule load／初期化／参照管理を使います。moduleの一部fieldだけを手で初期化したり、
  module参照APIを置き換えたりしません。

## machine境界

- native execmemへhostの実行可能メモリ範囲を渡します。upstreamの確保・寿命管理、CPAのalias検査、
  strict-module-RWXを維持します。
- direct-map aliasを含め、実PTEの権限をhost mapping／保護へ反映します。メモリ再利用前に無効化と
  権限復元を公開し、module window全体をRWXにはしません。
- 既存arch-portのexportを含む、native modpost生成のksymtab metadataでimportを解決します。
  canonical objectの分離とupstream boot／initcall配置を維持します。
- moduleをcoreから再配置可能な範囲へ配置します。非対応のruntime再配置とアドレス切詰めを拒否し、
  非allocatedなdebug offsetはruntimeアドレスと区別します。
- 登録済みmodule textにはnativeのmodule対応text検索とexception tableを使い、任意のnative
  アドレスは引き続き拒否します。

## 実装順序

1. full coreと同じ固定config・最下層arch headerで実module closureをbuildします。upstreamのmodule
   loadと初期化を使い、不足するhost executable memory／アドレス接続だけを実装します。
   未解決importはstub化せず拒否します。
2. upstream APIから試験用DRM deviceと実DRM fileを生成し、実GEM handleとmmap offsetの
   検索・権限検査を通します。shmem GEMの寿命試験に物理GPUは必要ありません。
3. object参照、handle、pin／unpin、vmap／vunmapを多重に取得・解放し、途中の解放を含めて
   pageの同一性と内容を検証します。
4. 異なるLinux mmを持つ実client 2 processへobjectをmapし、実faultを伴うread／writeとkernel vmap
   alias間の可視性を確認します。mappingを残してhandleを閉じ、その後もアクセス・新規faultを起こします。
5. 部分unmapやmmap setup失敗を含め、利用者の解放順序を変えて検証します。nativeの遅延cleanupを
   排出し、object／backing pageの実回収後、DRM file／device、moduleを依存順に解放します。
   MM・基盤Gateの回帰試験も必須です。

## 完了Gate

- native DRM／GEM実装とmodule初期化を使い、旧同期runtime providerや上位subsystem置換がない。
- object参照、kernel vmap、client 2 mmのmappingで実内容が共有され、多重pin／vmapが正しく釣り合う。
- handle閉鎖後はlookup・権限のない新規mappingが拒否されるが、既存mappingは新規faultも含め有効。
- 正当な参照が残る間はobject／pageが生存し、最後の解放後はallocatorからの再取得で回収を証明する。
  teardown／rollbackでUAF、二重解放、参照漏れ、残存callbackがない。
- 同じcoreで基盤CTest全件とGEM寿命の反復試験が通る。
