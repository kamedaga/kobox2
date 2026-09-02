# Wire protocol

Files in this directory are licensed under MIT independently of the
Apache-2.0 repository default. Generated headers must begin with:

```text
SPDX-License-Identifier: MIT
```

All schemas are `dev`. They have no ABI version number or compatibility
guarantee until an explicit freeze.

Files under `schema/` are canonical. Each digest is SHA-256 over its RFC 8785
representation. `tools/generate_protocol.py` validates them and produces the
checked-in layout constants. The build rejects a stale generated header. The
public codec reads and writes bounded little-endian byte ranges without mapping
native structures onto wire data.

The `kobox2_protocol` library target is independent of the controller library
and is the only kobox2 target linked into the test sandbox process.
