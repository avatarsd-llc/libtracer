# framing/opt-all-bits-nested

Every defined opt bit set simultaneously on one TLV: the child's opt is **0x7E = PL|TS|CR|LL|CW|TF** with both reserved bits clear. The parent POINT (opt=0x60, PL=1 + absolute ts) anchors the child's relative timestamp.

Child: type=0x07 POINT, 6-byte header (LL=1, length u32 = 5), structured payload = one NAME "t" (`02 00 01 00 74`), trailer_ts = i32 +100 ns (LE `64 00 00 00`), trailer_crc = CRC-16-CCITT(payload ++ trailer_ts) = 0x61D7 (LE `D7 61`). Child total = 6 + 5 + 4 + 2 = 17 bytes; parent length = 17; parent trailer = absolute u64 ts.

This is the strongest shifted-mask catcher in the corpus: shifting the whole mask table by one bit in either direction lands on a reserved bit (mandatory INVALID reject, see the reserved-bit vectors) — and any single wrong mask breaks the 6-byte header, the child-walk, the 4-byte relative ts, or the 2-byte CRC arithmetic, so the frame cannot round-trip.

```
07 60 11 00
   07 7E 05 00 00 00
      02 00 01 00 74
   64 00 00 00 D7 61
08 07 06 05 04 03 02 01
```

29 bytes total.
