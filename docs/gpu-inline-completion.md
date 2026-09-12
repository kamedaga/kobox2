# Inline synchronous GPU completions

`kb2_gpu_inline_completion_encode/decode` implements the existing completion
header and argument descriptor schema for a deliberately explicit subset:
`COMPLETED` disposition, zero command-specific detail, no span/attachment
descriptors, and either one inline result argument or no data. Non-OK status
has no inline result. Asynchronous submission and attachment ownership need
their own complete implementation; this codec does not accept them silently.

This adds no wire fields, ABI versions or schema changes. VERSION output spans
remain the spans registered by the request, not newly granted response spans.
The VERSION completion contains its canonical result record; external output
storage becomes visible when transport completion is published.

The codec checks fixed identity/digest, exact sizes and table offsets, reserved
fields, status, session and the inline descriptor. It does **not** decide which
result record is authorized for a command. Its caller must check the record ID,
length, reserved fields and semantics against the privately retained request.
Generation and correlation belong to the outer transport envelope and must
also match the outstanding request. A session match alone is insufficient.

All encode inputs and decode snapshots are caller-owned and immutable during
the call. Encode input data must not overlap the destination. Decoded data
borrows the input snapshot. Errors leave output values and encode destination
unchanged. The API allocates nothing and retains no state.

`kobox2.gpu_protocol` tests normal/error completions, truncation, stale session,
descriptor corruption, unsupported response content and unchanged decode
outputs. Host integrations additionally test command-specific result validation.
