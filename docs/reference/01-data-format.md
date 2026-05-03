# Reference 01 — Data Format

> **Status**: stub. Canonical content lives in [../plans/03-wire-format-and-data-model.md](../plans/03-wire-format-and-data-model.md) until promoted.

## What goes here when filled

- TLV header byte layout (`type:u8`, `opt:u8`, `crc:u32`, `length:varint`), endianness, alignment.
- `opt` bitfield: VR (version), PL (LIST payload), TS (embedded timestamp), FP (finite-pool length), CR (CRC variant), reserved bits.
- LEB128 varint encoding rules; finite-pool slot classes (8 / 32 / 128 / 512 / 2K / 8K / 32K / 128K).
- CRC-32C algorithm (Castagnoli polynomial `0x1EDC6F41`), software fallback, hardware acceleration paths (x86 SSE 4.2, ARMv8 `crc32x`).
- Type code registry table (`0x00–0x1F` core defined, `0x20–0x7F` reserved for future core, `0x80–0xFF` user-defined).
- Hex example for every core type code: bytes on the wire ↔ logical content.
- Forward/backward compatibility rules across version bumps.
