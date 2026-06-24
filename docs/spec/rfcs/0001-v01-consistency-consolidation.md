<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
-->

# RFC 0001 — Protocol-v1 wire-format consistency consolidation

| Field | Value |
| ---- | ---- |
| **RFC** | 0001 |
| **Title** | Protocol-v1 wire-format consistency consolidation |
| **Status** | draft |
| **Author(s)** | AvatarSD (maintainer) |
| **Created** | 2026-06-24 |
| **Comment window closes** | 2026-07-08 (≥ 14 days) |
| **Tracking issue** | [#3](https://github.com/avatarsd-llc/libtracer/issues/3) |
| **Target spec version** | v1 (draft refinement — no released v1 yet, so no v2 needed) |

## Summary

A multi-agent consistency audit of the protocol-v1 draft (57 confirmed inconsistencies, on top of the prior `r1` analysis) found that the **reference suite (`docs/reference/01`, `05`) is the most coherent, modern layer**, while the legacy glossary and the extracted C++ taught the retired v0.0 model, and — more importantly — that the reference itself carries a handful of **internal contradictions**. This RFC consolidates the wire format and conformance surface onto one normative source and fixes the reference-internal cracks, so that `docs/spec/v1.md` can incorporate `reference/01` + `05` by reference without inheriting contradictions. The supporting decisions are recorded in [ADR-0002…0007](../../adr/); this RFC is the spec-domain change that applies them.

## Motivation

`docs/spec/v1.md` is ~90% stubbed; the byte-precise wire format lives only in `docs/reference/`, which is labelled "descriptive." So conformance (§4) currently has no normative anchor. Meanwhile the same load-bearing surfaces are defined 2–4 incompatible ways across spec / reference / glossary / code:

- the `opt` byte, CRC (algorithm + width + **placement**), length encoding, header size, `LIST`, and version identity each disagree across layers;
- the normative gate (`tests/conformance/vectors/v1/`, the RFC directory) pointed at artifacts that did not exist;
- and the reference is **not internally clean**: `VERSION_MISMATCH (0x06)` references a deleted `opt.VR` bit; `ERROR (0x08)` packs a raw byte before its `PL=1` children (breaking the universal "`PL=1` ⇒ concatenated child TLVs" rule); `0x05`/`LIST` is retired in `01`/`05` but still used in `03`/`06`; `io_dir_t` is defined three incompatible ways.

Per `GOVERNANCE.md` and `CLAUDE.md` precedence (spec > reference > (the now-removed plans/glossary); code least authoritative), the reference wins and the rest are reconciled to it — except the few genuine forks decided in the interview that produced the ADRs.

## Proposed change

### A. Versioning & normative structure (ADR-0002, ADR-0007)

1. **Version axes.** The **protocol** is the integer **v1** (frozen-immutable once finalized; a wire-incompatible change is protocol v2, versioned at the discovery layer — no per-frame version field). The **release** version is independent semver (`library.json` etc.). Edit `00-overview.md` §versioning and `01-data-format.md` §versioning so every "v0.1 is the wire format" reads "protocol v1 is the wire format." Reconcile `docs/spec/v1.md` §5 ("versions are integers") with this framing.
2. **Normative by incorporation.** `docs/spec/v1.md` §2 (Terminology) points at `CONTEXT.md`; §3 (Wire format) **normatively incorporates** `reference/01-data-format.md` and `05-protocol-tlvs.md`; those two files' status line changes `descriptive` → `normative`. §1 (Scope) is written. §3.1 (path handles) stays as-is.

### B. Wire-format ratification (ADR-0004, ADR-0005)

Already correct in `reference/01`; this RFC makes them normative for protocol v1:

- **`opt` byte** = `R│PL│TS│CR│LL│CW│TF│R` (bits 7→0); bits 7 and 0 reserved-MUST-be-zero. No `VR`, no `FP`.
- **Header** 4 bytes (`type` u8, `opt` u8, `length` u16 LE), 6 bytes when `opt.LL=1` (`length` u32 LE). **Length** fixed-width, no LEB128 / u64 / finite-pool.
- **CRC** in the optional trailer (`opt.CR`), CRC-32C default / CRC-16-CCITT (`opt.CW`), over `payload + trailer_ts`. **NAME** carries no NUL terminator.

### C. Reference-internal crack fixes (normative)

1. **`ERROR (0x08)`** — **⚠ withdrawn; superseded by [RFC-0002](0002-protocol-error-model.md).** The leading-child-`VALUE` code shape is replaced by the `tr::<concept>::<error>` identity model (registered code *or* string); see RFC-0002 §C for the `ERROR` byte layout and §D for the registry.
2. **`0x06 VERSION_MISMATCH`** is redefined as a **discovery/bridge-level** error ("peer advertised an incompatible protocol version"); strike the `opt.VR` wording. (Edit `05` §0x08 registry.) **(Subsumed by [RFC-0002](0002-protocol-error-model.md) as `tr::version::mismatch`.)**
3. **`0x0F INVALID`** is added to the registry (general structural invalidity: reserved-bit set, `type=0x00`, oversize length) — the code `01` already names but `05` did not define. **(Subsumed by [RFC-0002](0002-protocol-error-model.md) as `tr::frame::invalid`.)**
4. **`LIST` retirement sweep:** remove the four surviving "LIST" references in `03-addressing.md` / `06-user-data-packing.md` and the dead `01:273` cross-ref. An array-whole read returns a `PL=1` reply whose children are the element TLVs; an atomic multi-field write is a **SETTINGS (`0x0B`)**. (ADR-0003.)
5. **Address-shift:** group key is **`(origin_peer_id, ts)`** (matching `02`'s in-flight identity), and `03`'s loss-detection claim is narrowed — tail-slice loss is undetectable without an explicit `expected_count`. Implementation note: the assembler must retain `origin_peer_id` after a bridge sheds the ROUTER.
6. **`io_dir_t`** canonical spelling is `IO_DIR_DEVICE_TO_CPU` / `IO_DIR_CPU_TO_DEVICE` across `08`/`09`/`10` (module-ABI; Normal-bar, included here for completeness).

### D. API surface (ADR-0006)

The conformance-mandated API is **`read` / `write` / `await`** plus a **field-write** control surface (`:`-addressed fields); there is no `connect` / `disconnect` / `subscribe` primitive. Subscribing is writing a SUBSCRIBER into `:subscribers[]`.

### E. Conformance (§4)

Conformance vectors live under `tests/conformance/vectors/v1/<category>/<case>/{input.bin, expected.json, description.md}` (categories `framing`, `path`, `tlv-types`, `errors`, `crc`, `address-shift`, `router-dedup`). Seed vectors for the worked frames are added (empty STATUS=OK, VALUE bool, PATH `/sensor/temp`, VALUE+CRC-32C). `docs/spec/v1.md` §4 references this path; the `path_canonical/` (reference 02) and `encode/decode/roundtrip/` (old README) conventions are superseded.

### F. Non-normative housekeeping (already landed alongside this RFC)

FYI — these are Low/Normal-bar changes made directly, not part of the spec change: demote `99-glossary.md` to a `CONTEXT.md` redirect + repoint `CLAUDE.md`; delete the v0.0 `core/` headers (rebuild fresh, ADR-0001) + fix `CMakeLists`/`library.json`; create this `rfcs/` template; seed the vector directory; fix the `avatarsd` → `avatarsd-llc` and `../../libtracer/` → `../../core/` path drift.

## Compatibility

- **Does this break protocol-v1 implementations?** No released v1 implementation exists; these are draft refinements *before* the freeze gate. The extracted C++ is being rebuilt, not migrated.
- **v0.0 interop:** none — no v0.0 peer interoperates with protocol v1 (header 8→4/6, CRC trailer + CRC-32C, length fixed-width, `opt` redefined, `LIST` gone, NAME drops NUL). This is acceptable: v0.0 was never released.
- **New conformance vectors:** yes — the seed set above, plus `errors/` vectors for the `ERROR 0x08` leading-child shape and `0x0F INVALID`, and an `address-shift/` vector for the `(peer_id, ts)` key.
- **Migration:** implementers track the protocol-v1 draft; the freeze gate (`reference/README` promotion rule) requires these vectors pass before any "frozen" claim.

## Alternatives considered

Each decision's rejected options are recorded in its ADR: per-frame `VR` bit ([0002](../../adr/0002-versioning-protocol-vs-release-no-per-frame-version.md)); a generic `LIST` container ([0003](../../adr/0003-retire-list-type-code-0x05.md)); header-resident CRC ([0004](../../adr/0004-crc-in-optional-trailer.md)); LEB128 / u64 length ([0005](../../adr/0005-fixed-width-length-opt-ll.md)); named subscribe/connect verbs ([0006](../../adr/0006-read-write-await-api-no-connect.md)); promoting the wire format into a self-contained spec vs incorporating the reference ([0007](../../adr/0007-normative-wire-format-by-incorporation.md)). For `ERROR 0x08`, a PL=0-opaque form and a documented 0x08 prefix-exception were rejected in favour of the leading child TLV (keeps the `PL=1` rule universal).

## Discussion

Per [GOVERNANCE.md](../../../GOVERNANCE.md), the tracking issue stays open at least 14 days (until 2026-07-08) for implementer feedback before this document is merged. Record sustained objections and their resolution here.
