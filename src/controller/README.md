# Controller implementation

This directory will contain the host-independent lifecycle state machine. It
must depend only on the public controller API and the MIT wire protocol, not on
Linux or a particular host OS.

The library contract is specified in
[`docs/controller-library.md`](../../docs/controller-library.md).
