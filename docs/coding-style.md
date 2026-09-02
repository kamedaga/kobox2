# Coding style

This document defines conventions for code owned by kobox2 and the boundaries
with `linux-sandbox` and host integrations. It does not override the coding
style of an upstream project.

## Common rules

- Store text as UTF-8 with LF line endings, a final newline, and no trailing
  whitespace.
- Add the correct SPDX license identifier to every new source file:
  - `Apache-2.0` for the kobox2 controller;
  - `MIT` for files under `protocol/`;
  - `GPL-2.0-only` for linux-sandbox code; and
  - the host repository's license for a host adapter (`MIT` in PachaOS).
- Use `snake_case` for C file names, functions, and variables.
- Comments explain reasons, invariants, ownership, or non-obvious constraints.
  Do not repeat what the code already states.
- Make ownership, lifetime, and thread-safety requirements explicit at API
  boundaries. Avoid hidden mutable global state.
- Validate ranges and check integer overflow before performing size or offset
  arithmetic.
- Do not add silent-success stubs. Unsupported operations return an explicit
  error or stop at a documented invariant.
- Keep compiler warnings clean and test both successful and failing paths.
- Keep project summaries in `README.md`, implementation details in `docs/`,
  and mandatory repository rules in `AGENTS.md`.

English documentation is canonical. User-facing kobox2-owned Markdown
documents have a corresponding `-jp.md` translation that is updated in the
same change. Upstream Linux documentation is neither translated nor rewritten.

## kobox2 controller

Controller code uses portable C11. Host-independent code must not depend on
compiler extensions, Linux headers, or host-specific integer and error values.

- Indent with four spaces. Do not use tabs in C source.
- Prefer lines no longer than 100 columns.
- Put opening braces on the same line as functions and control statements.
- Put public headers under `include/kobox2/` and internal declarations under
  `src/`.
- Use include guards for public headers. Do not rely on `#pragma once`.
- Prefix public functions and types with `kb2_`, and public macros and enum
  constants with `KB2_`.
- Keep public structures opaque until their representation is intentionally
  made part of a stable ABI.
- Use `kb2_status_t` and named `KB2_STATUS_*` values at the public boundary.
  Convert host `errno` or OS-specific failures in the host adapter instead of
  leaking them into the controller API.
- Pass allocators and I/O operations explicitly. Do not introduce an implicit
  dependency on `malloc`, process-global state, or a particular host runtime.
- Default to caller-owned state and a single owner for each controller state
  machine. Document any operation that is safe to call concurrently.
- Name C tests `*_test.c` and reusable test fixtures `*_fixture.c`.

Owned controller code is formatted with the repository `.clang-format`. New
code must build as C11 with both Clang and GCC using at least `-Wall -Wextra
-Wpedantic`; project CI may promote these warnings to errors. Host tests should
also run under AddressSanitizer and UndefinedBehaviorSanitizer where supported.

## Shared protocol

The protocol is a serialized wire format, not a shared native C structure.

- Use fixed-width integer fields and explicitly encode them as little-endian.
- Do not put pointers, host file-descriptor numbers, `size_t`, native C enums,
  bit-fields, or host object layouts on the wire.
- Do not cast shared memory directly to a C message structure or rely on
  `packed` layout. Decode and encode fields through bounded helpers.
- Validate every offset, length, alignment, object generation, and completion
  generation before use.
- Send reserved fields as zero and reject nonzero reserved fields unless a
  protocol version explicitly defines them.
- Treat the schema as the source of truth. Do not edit generated headers or
  encoders directly.
- Before the first ABI freeze, prefer a clean breaking correction over an
  accidental compatibility promise. After a freeze, change the explicit
  protocol version.
- Never append an opcode arbitrarily merely to avoid renumbering an unfrozen
  ABI. Keep related values in a coherent order.

## linux-sandbox

All Linux and kobox-specific code in the `linux-sandbox` repository follows
the upstream Linux kernel coding style in
`Documentation/process/coding-style.rst` and the repository's existing
`.clang-format` and `.editorconfig`.

- C indentation uses tabs with a tab width of eight, and lines preferably fit
  within 80 columns.
- Function opening braces go on the following line; control-statement braces
  stay on the same line.
- Use upstream Linux types, helpers, subsystems, and Kbuild integration.
- Do not bulk-format or mechanically rewrite upstream files.
- Keep upstream modifications small and reviewable as patches.
- Never encode Linux structure offsets as numeric constants.
- Keep kobox-owned integration under `kobox/` and keep PachaOS names, policy,
  package paths, and syscall numbers out of this repository.

## Host integrations

Host adapters follow their host repository's established conventions. The
PachaOS `gpud` integration uses portable C11, four-space indentation, attached
opening braces, and a preferred 100-column limit, matching PachaOS userland.

Host adapters translate native handles, errors, process bootstrap state, and
capability policy at the boundary. These representations must not become part
of the kobox2 controller or shared wire protocol.
