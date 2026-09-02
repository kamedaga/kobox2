# kobox2 agent notes

- Keep this repository host-independent. PachaOS role policy and packaging
  belong in the PachaOS repository.
- Do not include Linux headers or implement Linux structure layouts, module
  symbols, subsystem semantics, or `.ko` loading in the Apache-2.0 controller.
- GPL-only code must remain in the `linux-sandbox` submodule and a separate
  runtime process. Never link Apache-2.0 controller code into that process.
- Wire messages must be pointer-free and must not expose host FD numbers or
  host kernel object layouts.
- Files shared with `linux-sandbox` belong under `protocol/` and must retain
  their MIT SPDX identifier.
- Do not invent compatibility guarantees before the first protocol version is
  explicitly frozen.
