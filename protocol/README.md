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

Transport channels require at least one request lane and permit zero or one
application-event lane. Filesystem RPC has no event stream; queue-progress
doorbells are not application events and do not justify a dummy GPU event lane.
Queue depth is a negotiated power of two within the split-ring representation,
not a GPU-derived minimum of sixteen. This corrects the unnumbered `dev`
transport schema; there is no legacy-schema fallback. Existing GPU profiles
still establish and validate their actual event lane in their host policy.

`schema/filesystem.json` defines the new filesystem `dev` request/response
headers, with one operation catalog for client, relay and storage. It deliberately
does not preserve the old filed operation numbers, cache-generation operations,
root special cases, short name arrays or inline transfer limit. Data and paths
use non-overlapping bounded spans; queue capacity is a transport resource, not
a filesystem size limit. Linux owns pathname and filesystem limits. Relay-only
credentials must be supplied from authenticated owner state, not copied from
an untrusted client. The codecs validate byte ranges but do not grant authority
or claim that every catalog operation already has a service implementation.

Filesystem capability rights are protocol-defined rights separate from Linux
credential capabilities and DAC. The owner registers and updates the rights
with the retained Linux credentials. Data dispatch obtains both atomically and
checks `kb2_fs_request_policy` against the same private request snapshot used
for VFS execution. A shared request cannot grant itself rights or select the
owner dispatcher. Management/frontend operations are excluded from data
dispatch, even if the client has every right. A bound data request may leave
client and credential-generation claims zero; the queue binding then selects
its registered client and current credentials. Nonzero claims are only
consistency checks, never a source of authority.
