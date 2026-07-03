<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# RFC 0002 — Protocol error model: the `tr::` concept namespace

| Field | Value |
| ---- | ---- |
| **RFC** | 0002 |
| **Title** | Protocol error model: the `tr::` concept namespace |
| **Status** | **accepted** (2026-07-03) |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-06-24 |
| **Comment window** | waived by the maintainer (solo-maintainer project, GOVERNANCE.md window dead ceremony) |
| **Tracking issue** | [#3](https://github.com/avatarsd-llc/libtracer/issues/3) (RFC-0001 umbrella) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |
| **Supersedes** | RFC-0001 §C.1; subsumes RFC-0001 §C.2, §C.3 |

## Summary

This RFC replaces libtracer's flat, numeric error-code registry with a **hierarchical error namespace keyed by stable protocol concept** — `tr::<concept>::<error>` (e.g. `tr::path::not_found`). An error's wire **identity** is either a compact **registered code** (a single integer the frozen registry assigns to a built-in path) or the literal **string** path (for unbounded third-party extensions); optional **structured detail** may attach to either. Per-error **severity** and **disposition** live in the registry, not on the wire. The namespace is **protocol-only** — applications never emit protocol errors (ADR-0010). The supporting decisions are [ADR-0009](../../adr/0009-built-in-error-model-tr-concept-namespace.md) (shape) and [ADR-0010](../../adr/0010-closed-protocol-error-boundary.md) (closed boundary); this RFC is the spec-domain change that applies them and fixes the wire layout of `ERROR` (`0x08`).

## Motivation

RFC-0001 §C.1–§C.3 treated `ERROR` as a flat list of byte codes (`0x00`–`0x7F` core, `0x80`–`0xFF` user) with the code carried as a leading child `VALUE`. A maintainer review found three problems that a 20-year-frozen v1 cannot ship with:

1. **The module/source set is unbounded.** A flat byte registry has no room for the ~50 catalogued modules plus future and third-party ones; reserving a numeric range per module does not scale.
2. **Numeric/module identity is not implementation-independent.** Keying an error to the C reference's modules (`resolver`, `dispatcher`, …) is meaningless to a second implementer in another language and brittle under refactor. The reference suite's whole premise is implementation independence.
3. **There is no such thing as an application protocol-error.** Applications signal failure as ordinary data, self-described by their schema — the same way the protocol defines no application *data types*. A `0x80–0xFF` "user error" range is a category error.

## Proposed change

### A. The namespace (ADR-0009)

An error is identified by a path in a hierarchy keyed by **stable protocol concept**:

```
tr::<concept>::<error>
```

`<concept>` is drawn from a small, frozen set that names a *protocol* concern (the [CONTEXT.md](../../../CONTEXT.md) vocabulary), **not** an implementation module:

`frame` · `tlv` · `path` · `schema` · `flow` · `access` · `transport` · `version`

The namespace is **prefix-filterable** with the path wildcard ([03-addressing.md](../../reference/03-addressing.md) §index forms): a subscriber MAY watch `tr::flow::*` or `tr::transport::*`.

### B. Identity forms (ADR-0009)

An error's identity is carried in exactly one of two forms:

- **Registered code** — a single `u16` the registry below assigns to the whole built-in path. The hot, finite, built-in case.
- **String** — the literal UTF-8 `tr::…` path. For modules **not** in the frozen registry (third-party stack extensions, `tr::<vendor>::…`) — the unbounded case. No registry entry, no RFC required.

The registered-vs-string choice **is** the built-in-vs-extensible split. Optional **structured detail** (a human `DESCRIPTION`, a concept-specific `VALUE`, …) MAY follow either identity.

### C. Wire layout of `ERROR` (`0x08`)

`ERROR` is a structured TLV (`opt.PL=1`) in **all** cases — it is never special-cased, and a generic `PL=1` walker handles it. Its **first child is the identity**, distinguished by type:

| First child | Identity form | Payload |
| ---- | ---- | ---- |
| `VALUE` (`0x01`) | registered code | `u16` LE registered code |
| `NAME` (`0x02`) | string | UTF-8 `tr::…` path (no NUL) |

Subsequent children are optional detail (`DESCRIPTION` `0x03`, `VALUE` `0x01` binary detail, or concept-specific TLVs). A reader walks children; the first child's type alone selects the identity form. This keeps the universal "`PL=1` ⇒ purely concatenated child TLVs" rule (ADR-0003) — there is no raw prefix byte.

Worked bytes — `tr::path::not_found` (registered code `0x0020`), code-only:

```
08 40 06 00   01 00 02 00 20 00
└ERROR hdr┘   └── VALUE child ──┘     ERROR: type=08 opt=40(PL=1) len=6
              type=01 opt=00 len=2     VALUE payload = 20 00 (u16 LE = 0x0020)
= 10 bytes        (14 wrapped in STATUS: 09 40 0A 00 + the 10 above)
```

`ERROR` continues to appear as a `STATUS` (`0x09`) child; a **bare `ERROR`** (no STATUS wrapper) is permitted for protocol-stack reporting where there is no request to answer.

### D. Registry (ADR-0009)

Normative for protocol v1; the built-in set below is frozen, and additions are RFC-gated. `severity` ∈ `warn | error | critical`; `disposition` ∈ `transient` (retry) · `permanent` (don't retry this request) · `fatal` (tear down the peer).

| Code | Path | Severity | Disposition | Was (RFC-0001 flat) |
| ---- | ---- | ---- | ---- | ---- |
| `0x0001` | `tr::frame::truncated` | error | transient | `0x0C TRUNCATED` |
| `0x0002` | `tr::frame::invalid` | error | permanent | `0x0F INVALID` |
| `0x0003` | `tr::frame::crc_fail` | error | transient | `0x05 CRC_FAIL` |
| `0x0010` | `tr::tlv::nesting_too_deep` | error | permanent | `0x0D NESTING_TOO_DEEP` |
| `0x0020` | `tr::path::not_found` | warn | permanent | `0x01 NOT_FOUND` |
| `0x0021` | `tr::path::invalid` | warn | permanent | `0x03 INVALID_PATH` |
| `0x0022` | `tr::path::in_use` | warn | permanent | `0x0E PATH_IN_USE` |
| `0x0030` | `tr::schema::type_mismatch` | error | permanent | `0x04 TYPE_MISMATCH` |
| `0x0031` | `tr::schema::not_found` | warn | permanent | `0x0A SCHEMA_NOT_FOUND` |
| `0x0040` | `tr::flow::backpressure` | warn | transient | `0x07 BACKPRESSURE` |
| `0x0041` | `tr::flow::timeout` | warn | transient | `0x08 TIMEOUT` |
| `0x0042` | `tr::flow::address_shift_gap` | error | permanent | `0x0B ADDRESS_SHIFT_GAP` |
| `0x0050` | `tr::access::denied` | error | permanent | `0x02 PERMISSION_DENIED` |
| `0x0060` | `tr::transport::down` | error | transient | `0x09 TRANSPORT_DOWN` |
| `0x0070` | `tr::version::mismatch` | critical | fatal | `0x06 VERSION_MISMATCH` |

`OK` is removed — an empty `STATUS` already means OK. `tr::version::mismatch` is a discovery/bridge-level outcome (RFC-0001 §C.2), not a frame-parse result. `tr::frame::invalid` covers reserved-bit-set / `type=0x00` / oversize length (RFC-0001 §C.3).

### E. Closed boundary (ADR-0010)

There is **no** user/application error range. An application failure is ordinary data written into the graph and described by the application's schema. The protocol interprets neither application data nor "application errors."

### F. Conformance (§4)

New vectors under `tests/conformance/vectors/v1/errors/`: a registered code-only error (`tr::path::not_found`), a registered error with a `DESCRIPTION` detail child, a string-form error (`tr::acme::widget::jammed`), and `tr::frame::invalid` for a reserved-bit-set input. `docs/spec/v1.md` §3 (once it incorporates `reference/05` per RFC-0001 §A.2) inherits the `ERROR` layout from §C above.

## Compatibility

- **Breaks protocol-v1 implementations?** No released v1 exists; this is a draft refinement *before* the freeze gate. The flat byte registry was never shipped.
- **Supersession:** RFC-0001 §C.1 (leading-child-`VALUE` code) is **withdrawn**; §C.2 (`VERSION_MISMATCH` reword) and §C.3 (`INVALID`) are **subsumed** here as `tr::version::mismatch` / `tr::frame::invalid`. RFC-0001 should be edited to point here for the `ERROR`/registry items; its other sections (versioning, `LIST`, `io_dir_t`, API, address-shift) are unaffected.
- **New conformance vectors:** the `errors/` set above.

## Alternatives considered

- **Per-module numeric `{module:u8, code:u16}`** and **module-named paths** (`tr::graph::resolver::…`) — rejected in [ADR-0009](../../adr/0009-built-in-error-model-tr-concept-namespace.md): unbounded and implementation-coupled.
- **`ERROR` as `PL=0` opaque with the `u16` code as the whole payload** (6 bytes, cheapest registered form). Tenable, and ~4 bytes cheaper per error, but it makes `ERROR` special (a reader must branch on `opt.PL` to find the identity) and complicates the string/detail forms. Rejected in favour of always-`PL=1` (one mental model, `ERROR` never special). Re-openable in comment if the embedded byte cost proves to matter on a hot error path.
- **A user/application error range** — rejected in [ADR-0010](../../adr/0010-closed-protocol-error-boundary.md).
- **`severity`/`disposition` on the wire** — rejected: registry-resident, derived on receipt.

## Discussion

Accepted 2026-07-03 with the comment window waived (maintainer call — a solo-maintainer window has no commenters). Points that were open for comment, resolved as proposed: the §C `PL=1`-always vs `PL=0`-cheap layout; the §D code assignments and the `severity`/`disposition` scales; whether the string form's `tr::…` path should be one `NAME` or a `/`-style segmented structure for finer prefix matching.
