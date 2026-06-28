# Changelog — libtracer (Rust binding)

All notable changes to the **public API** of the `libtracer` Rust crate are
recorded here, per [CONTRIBUTING](../../CONTRIBUTING.md) / [CLAUDE.md](../../CLAUDE.md).
The crate is pre-1.0; everything currently lives under `[Unreleased]`.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

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
