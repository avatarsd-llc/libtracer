# Changelog — libtracer (Rust binding)

All notable changes to the **public API** of the `libtracer` Rust crate are
recorded here, per [CONTRIBUTING](../../.github/CONTRIBUTING.md) / [CLAUDE.md](../../CLAUDE.md).
The crate is pre-1.0; the first cut release is `[0.3.0]`, below.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

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
