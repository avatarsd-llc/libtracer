# Length is fixed-width LE selected by opt.LL (u16 / u32), capped at u32

Status: accepted

The TLV `length` field is **fixed-width little-endian**, selected per-TLV by `opt.LL`: **u16** (2 bytes, ≤ 65535) by default, **u32** (4 bytes, ≤ 4 GiB−1) when `opt.LL=1`. This fixes the header at **4 bytes** (6 with `LL=1`) and the minimum frame at 4 bytes. There is no u64 length.

## Considered options

- **LEB128 / varint length** (the legacy glossary still calls it "the default"). Rejected: branchy parse, unpredictable payload offset, hostile to streaming and SIMD.
- **u64 length** and a **finite-pool wire mode**. Rejected: multi-gigabyte single frames are handled by address-shift slicing across `ep[0..N]` instead; the slot-class concept survives only as an internal receive-buffer pooling convention, never on the wire.

## Consequences

- Branchless parse at offset 2 (read 2 or 4 bytes off one bit); minimum-feature implementations can preallocate worst-case buffers.
- The `core/` code's hardcoded `uint32_t`-only length and 8-byte header (offsets at +8 where the reference reads payload at +4) are non-conformant.
