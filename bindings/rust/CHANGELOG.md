# Changelog — libtracer (Rust binding)

All notable changes to the **public API** of the `libtracer` Rust crate are
recorded here, per [CONTRIBUTING](../../.github/CONTRIBUTING.md) / [CLAUDE.md](../../CLAUDE.md).
The crate is pre-1.0; the first cut release is `[0.3.0]`, below.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- **`fwd::ParsedFwd` learns the RFC-0024 §5-§7 routing surface.** Two new fields —
  `mint_request` (the `op` byte's bit 7) and `dst_bound` (`dst` is a `PATH_REF`, not a `PATH`)
  — plus `fwd_op::OPCODE_MASK` (`0x3F`) and `fwd_op::FLAG_MINT_REQUEST` (`0x80`).
  **Behaviour change, and both halves matter for interop:** `parse_fwd_tlv` now masks the `op`
  byte before comparing it, so a mint-flagged operation parses as its plain opcode instead of
  falling through every arm; and it accepts a `PATH_REF` in the `dst` and `src` positions,
  which it previously rejected as `TypeMismatch`. An element is node-scoped, so this binding
  carries one and never interprets it.

- **`type_code::PATH_REF` (`0x14`) and the bound-path element codec
  ([RFC-0024](../../docs/spec/rfcs/0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §4).**
  New public items: `PathRefElement { index: u32, generation: u32 }`,
  `path_ref(&[PathRefElement]) -> Result<Tlv, BuildError>`,
  `path_ref_element(&[u8], usize) -> Option<PathRefElement>`, and the constants
  `PATH_REF_ELEMENT_BYTES` (8) and `MAX_PATH_REF_ELEMENTS` (255). `path_ref` answers
  `BuildError::TooManySegments` past the bound rather than truncating.
- **`decode` enforces the `PATH_REF` body shape.** For type `0x14`, `opt.pl` and `opt.ll` must
  be clear, the length must be a multiple of 8, and the element count must be ≤ 255; a
  violation is `Error::FrameInvalid`. **Behaviour change:** bytes that previously decoded as an
  opaque unknown-type TLV with type `0x14` now reject. `0x14` was unassigned, so no frame any
  libtracer version has emitted is affected.

## [0.7.0] — 2026-08-02

### Added

- **`path::admit_path_tlv(&Tlv) -> Result<(), BuildError>` (#773)** — the construction/admission
  gate for a *decoded* PATH: the encode-time MUST of `tlv_builders::path`
  (`children ≤ MAX_SEGMENTS` ∧ each child a valid NAME ∧ encoded body ≤ `MAX_PATH_BYTES`)
  evaluated over an already-parsed tree. Call it where a foreign PATH becomes an address a
  resolver will spell; do **not** call it on a `src` return route being forwarded on. The 255
  bound is unchanged and exact — the same inputs it accepted and rejected at the decode tier it
  accepts and rejects here.

- **`structured::DeliveryPolicy`, `structured::subscriber_policy`,
  `structured::subscriber_with_policy` and `structured::DELIVERY_POLICY_KEY`
  ([RFC-0022](../../docs/spec/rfcs/0022-qos-restructure.md) §3.A; #758)** — read and write one
  subscription's packed 16-bit delivery-policy word, the `SETTINGS{ NAME "delivery_policy"
  VALUE u16 }` child of a SUBSCRIBER. Delivery policy describes a **producer→subscriber
  relationship**, not the producer, so it travels on the subscribe-write rather than on the
  vertex. The bit layout mirrors the C++ core's `delivery_policy_t` byte for byte — bits 0–1
  reliability, 2–4 priority, bit 5 `durability_request` (`DeliveryPolicy::DURABILITY_REQUEST`),
  6–15 reserved. Reserved bits are round-tripped **verbatim and never interpreted** (§3.A says
  ignore, not reject), so a future sender's bits survive a hop through this binding.
  `subscriber_policy` reads the word **by NAME**, never by position — a `SETTINGS` child naming
  a different key (e.g. `delivery_compact`) is not the policy and yields the default.
  `subscriber_with_policy` emits **no** `SETTINGS` child for an all-zero policy, so a caller
  that does not ask for a policy produces bytes identical to `tlv_builders::subscriber`'s and
  to a pre-RFC-0022 sender's. Pinned to the `subscriber/policy-{absent,durability,reserved-bits}`
  conformance vectors, with the two ablations that bite recorded in the commit that added them.

### Changed

- **SEMVER-VISIBLE — `MAX_SEGMENTS` repriced 32 → 255 (RFC-0023, accepted; #767).** The exported
  constant (`libtracer::MAX_SEGMENTS`) changes value, so any consumer that mirrors it in an array
  size or a message must be recompiled. `split_path` / `tlv_builders::path` / `tlv_to_path` accept
  33–255 segments where they previously answered `BuildError::TooManySegments`; the error variant
  itself is unchanged and still maps to `tr::path::invalid`. Under the current NAME-TLV body
  encoding the `MAX_PATH_BYTES` (1024) check binds first at 204 segments, so `path_to_tlv` answers
  `BuildError::PathTooLong` — not `TooManySegments` — for a 205-segment path.
  ~~**Known misplacement, repriced not relocated:**~~ relocated below (#773).

- **SEMVER-VISIBLE — the accumulative PATH bounds left the decode tier (RFC-0023 §5.6; #773,
  landed in #778).**
  `path::tlv_to_path` no longer answers `BuildError::TooManySegments` or
  `BuildError::PathTooLong`: `docs/reference/05-protocol-tlvs.md` §"Enforcement of the PATH
  constraints" places the segment-count and total-length limits where an address is
  **constructed or admitted**, and states that "the codec does not enforce these constraints,
  and is not expected to". A caller that relied on `tlv_to_path` (or `fwd::fwd_dst_path` /
  `fwd::fwd_src_path` / `structured::subscriber_target_path`, which route through it) to reject
  an over-limit PATH must call the new `path::admit_path_tlv` instead. This closes RFC-0019
  §10(b): an accumulated `src` return route is legal at any byte-reachable depth, and decoding
  one no longer fails. The per-segment rules (1..64 bytes, no reserved character, NAME child
  type, UTF-8) stay in `tlv_to_path` — they are not accumulative, and they are what makes the
  rendered string re-splittable.

- **The `:field` selector examples no longer name a knob that was deleted
  ([RFC-0022](../../docs/spec/rfcs/0022-qos-restructure.md) §5 + amendment 1; #758).**
  `field::FieldLevel`'s module docs illustrated the two-scalar-level shape with
  `":settings.deadline_ns"`, then `":settings.history_keep_last"`; both names were removed from
  the protocol's `settings` container, and the example is now `":settings.app"` — the only
  two-level `settings` field that still resolves. **No API changed here**, and the FIELD grammar
  is untouched: `parse_field` still parses any syntactically valid selector. What changed is on
  the wire — a peer answers `tr::field::not_found` for the removed names, on read *and* on
  write, independently of the caller.

- **`path`'s module header names the byte bound's real string-side enforcer.** A doc-only
  correction following the accumulative-bounds relocation above; no behaviour change.

### Fixed

- **The three `subscriber/policy-*` conformance vectors were scored `ok` by a core that
  implemented none of them (#758).** The polyglot harness contract is
  `encode(decode(input.bin)) == input.bin`, which any well-formed TLV satisfies, so this
  binding — which contained no delivery-policy code at all — passed the policy vectors on
  structure alone. The behaviour is now claimed where it can be false: `DeliveryPolicy` above,
  plus three vector-bound tests and a bit-layout pin in `tests/conformance_vectors.rs`.

## [0.6.0] — 2026-07-23

## [0.5.0] — 2026-07-21

## [0.4.0] — 2026-07-09

### Removed

- **BREAKING — `MAX_DEPTH` is removed (RFC-0006).** Nesting depth is
  receiver-resource-bounded, never a constant: `decode`'s explicit work stack is
  a growable `Vec` (the host heap is this binding's decode resource), so the
  depth-cap reject is gone. `Error::TlvNestingTooDeep` remains registered
  ("exceeds the receiver's decode resources") for harness parity and peers'
  `ERROR` frames; `decode` itself no longer produces it.

## [0.3.0] — 2026-07-07

### Added

- **Typed protocol CODEC tier on top of the wire codec** — mirrors what the
  TypeScript client provides; every builder/parser reproduces and interprets the
  shared conformance vectors byte-for-byte with the C++ core.
  - `mod tlv_builders` — typed TLV builders (`value` / `value_opts` /
    `value_u8..u64`, `name`, `path`, `subscriber`, the `Opt::structured()` /
    `opt(..)` helpers), the `ValueOptions` selector, `BuildError`, and
    child-lookup accessors on `Tlv` (`first_child`, `children_of_type`,
    `child_at`, `payload_str`, `payload_uint`).
  - `mod path` — PATH string ⇄ TLV (`split_path`, `path_to_tlv`, `tlv_to_path`)
    with the addressing rules (1..64-byte segments, reserved chars `/ : . [ ] * ?`,
    ≤32 segments, ≤1024 total encoded bytes), cross-checked against
    `core/src/path.cpp`.
  - `mod error_registry` — the 15-entry RFC-0002 `tr::<concept>::<error>` registry
    (`ErrCode` with `code`/`path`/`from_code`/`from_path`/`severity`/`disposition`),
    ERROR-TLV parse/encode in both identity forms (`error_code`, `error_string`,
    `parse_error`), and STATUS helpers (`status_ok`, `status_with_errors`,
    `status_errors`). Kept separate from the codec-fault `Error` enum.
  - `mod field` — the `:field` selector: `parse_field` / `encode_field` /
    `field_tlv` / `parse_field_tlv`, `FieldLevel`, `FieldMode`.
  - `mod structured` — typed read/build for POINT, SETTINGS, ACL (NFSv4-style
    `Ace`), SUBSCRIBER, SPEC, and a generic NAME-tagged field reader
    (`named_fields`) covering the ROUTER envelope.
  - `mod fwd` — the FWD remote-operation envelope: `FwdRequest` / `encode_fwd` /
    `decode_fwd` / `parse_fwd_tlv`, `fwd_op` / `fwd_kind`, reply error
    code/path accessors, and the FWD error-path table (RFC-0004 §B/§D).
  - `tests/conformance_vectors.rs` — an in-crate structural conformance suite that
    loads each vector's `input.bin` **and** `expected.json` and asserts the decoded
    typed structure (closing the previous round-trip-only gap; 31 tests).

- **`type_code::FWD` (`0x0F`) and `type_code::FIELD` (`0x10`)** registered in the
  type-code registry (RFC-0004 / ADR-0035, slice 1). The two remote-operation
  frames are structured TLVs the generic codec already round-trips; no codec change.
  New cross-core conformance vectors under `tests/conformance/vectors/v1/{fwd,field}/`
  match the C++ and TypeScript cores byte-for-byte and are exercised by `diff_fuzz.py`.

- **Native, pure-Rust wire codec — a third independent core**
  ([#57](https://github.com/avatarsd-llc/libtracer/issues/57),
  [ADR-0028](../../docs/adr/0028-native-cores-kept-consistent-by-conformance-vectors.md)).
  A from-scratch `#![no_std]` (`alloc`-only, zero external dependencies) port of the
  protocol-v1 wire format — not an FFI shim over the C++ core. It cross-matches the
  C++ and TypeScript cores byte-for-byte on the shared conformance vectors
  (`tests/conformance/run-all.py`) and on random differential-fuzz frames
  (`tests/conformance/diff_fuzz.py`).
  - Public API: `decode(&[u8]) -> Result<Tlv, Error>`, `encode(&Tlv) -> Vec<u8>`,
    the CRC primitives `crc32c` / `crc16_ccitt`, and the owned model
    (`Tlv`, `Opt`, `Trailer`, `Timestamp`, `Crc`, `CrcWidth`, `Error`, `type_code::*`,
    `MAX_DEPTH`).
  - Decoder is bounds-/overflow-safe: an over-long length on a short buffer returns
    `Error::FrameTruncated` rather than reading out of bounds; nesting is parsed
    iteratively and capped at `MAX_DEPTH` (32).
  - `examples/conformance.rs` implements the shared harness contract (`--tap` /
    default TAP mode and `--roundtrip` differential-fuzz mode).
  - `examples/perf.rs` is the Rust core's codec perf bench — the **core-impl-lang**
    axis (Rust) of the cross-core perf matrix ([#96](https://github.com/avatarsd-llc/libtracer/issues/96),
    [ADR-0032](../../docs/adr/0032-continuous-cross-core-perf-conformance-matrix.md)).
    It times decode→encode roundtrips over the shared conformance vectors and emits
    the same 12-field `RESULT` line per vector as the C++ (`bench/bench_libtracer.cpp`)
    and TypeScript (`perf.mjs`) benches, with `system="rust-core"`, `mode="codec"`.
    Run with `cargo run --release --example perf -- tests/conformance/vectors/v1`.
