# crc/value-ts-abs-crc32c

TS and CR combined on one TLV (opt=0x30, TS=1/TF=0 absolute + CR=1/CW=0 CRC-32C). type=0x01 VALUE, length=3, payload AA BB CC, trailer_ts = 0x0102030405060708 (72623859790382856 ns, LE `08 07 06 05 04 03 02 01`), trailer_crc = CRC-32C(payload ++ trailer_ts) = 0x3DBAFC8A (LE `8A FC BA 3D`).

The CRC input order is **payload bytes then trailer_ts bytes** (docs/reference/01-data-format.md §CRC); the header is never covered. A core that CRCs only the payload, or ts-then-payload, computes a different value and fails.

```
01 30 03 00 AA BB CC 08 07 06 05 04 03 02 01 8A FC BA 3D
```

19 bytes total.
