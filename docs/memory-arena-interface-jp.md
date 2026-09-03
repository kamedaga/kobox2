# Memory arena interface

## grant

interface identityは`dev`です。schema digestは`protocol/schema/memory_arena.json`をRFC 8785で
canonical化した内容のSHA-256です。

一つのpresent memory objectを`READ`、`WRITE`、`MAP` rightsでcore providerへbindします。objectは
role `memory`のnative handleを一つ持ちます。

## mapping

objectはresource-binding lifetimeのshared read-write mapped rangeを一つ公開します。address、length、
page sizeは4096 byte alignedで、minimum lengthは8192 byteです。

Linux handleはsize固定の`FD_CLOEXEC` memfdです。sealは`F_SEAL_SEAL`、`F_SEAL_SHRINK`、
`F_SEAL_GROW`の三つです。importはobject全体を`MAP_SHARED`でmapし、`MADV_DONTDUMP`を設定します。

## operation

operation tableの先頭は共通のsizeと`dev` identityです。`mapped_range`はmapped addressとlengthを
返します。objectとoperation tableはregistry releaseまで有効です。

## core lifecycle

core initは正確なschema digestをbindし、mapped range上でarenaを初期化してから残るprovider stepを
初期化します。core cleanupは全arena pageの返却後にmappingをreleaseします。
