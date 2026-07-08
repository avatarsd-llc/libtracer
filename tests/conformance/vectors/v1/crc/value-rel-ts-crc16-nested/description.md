# crc/value-rel-ts-crc16-nested

Combined-bit case: TS+CR+CW+TF together on one child (opt=0x36). The parent is a POINT (opt=0x60, PL=1 + absolute ts trailer) anchoring the child's relative timestamp, per the TF rule that relative offsets hang off a parent's absolute TS.

Child: VALUE, length=3, payload AA BB CC; trailer_ts = i32 **-1000** ns (negative offsets are legal, LE `18 FC FF FF`); trailer_crc = CRC-16-CCITT(payload ++ trailer_ts) = 0x524A (LE `4A 52`). Child total = 4 + 3 + 4 + 2 = 13 bytes; parent length = 13; parent trailer = absolute u64 ts `08 07 06 05 04 03 02 01`.

Any single-bit shift in a core's opt mask changes the child's trailer arithmetic (8-vs-4-byte ts, 4-vs-2-byte CRC) or the CRC input, so the round-trip or the CRC check fails.

```
07 60 0D 00
   01 36 03 00 AA BB CC 18 FC FF FF 4A 52
08 07 06 05 04 03 02 01
```

25 bytes total.
