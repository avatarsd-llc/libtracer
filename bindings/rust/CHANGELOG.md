# Changelog — libtracer (Rust binding)

All notable changes to the **public API** of the `libtracer` Rust crate are
recorded here, per [CONTRIBUTING](../../.github/CONTRIBUTING.md) / [CLAUDE.md](../../CLAUDE.md).
The crate is pre-1.0; the first cut release is `[0.3.0]`, below.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Changed

- **SEMVER-VISIBLE — `MAX_SEGMENTS` repriced 32 → 255 (RFC-0023, accepted; #767).** The exported
  constant (`libtracer::MAX_SEGMENTS`) changes value, so any consumer that mirrors it in an array
  size or a message must be recompiled. `split_path` / `tlv_builders::path` / `tlv_to_path` accept
  33–255 segments where they previously answered `BuildError::TooManySegments`; the error variant
  itself is unchanged and still maps to `tr::path::invalid`. Under the current NAME-TLV body
  encoding the `MAX_PATH_BYTES` (1024) check binds first at 204 segments, so `path_to_tlv` answers
  `BuildError::PathTooLong` — not `TooManySegments` — for a 205-segment path.
  ~~**Known misplacement, repriced not relocated:**~~ relocated below (#773).

- **SEMVER-VISIBLE — the accumulative PATH bounds left the decode tier (RFC-0023 §5.6; #773).**
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

### Added

- **`path::admit_path_tlv(&Tlv) -> Result<(), BuildError>` (#773)** — the construction/admission
  gate for a *decoded* PATH: the encode-time MUST of `tlv_builders::path`
  (`children ≤ MAX_SEGMENTS` ∧ each child a valid NAME ∧ encoded body ≤ `MAX_PATH_BYTES`)
  evaluated over an already-parsed tree. Call it where a foreign PATH becomes an address a
  resolver will spell; do **not** call it on a `src` return route being forwarded on. The 255
  bound is unchanged and exact — the same inputs it accepted and rejected at the decode tier it
  accepts and rejects here.

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
