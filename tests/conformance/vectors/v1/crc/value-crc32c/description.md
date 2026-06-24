# crc/value-crc32c

A 5-byte VALUE with a CRC-32C trailer. type=0x01 VALUE, opt=0x10 (CR=1, CW=0 means CRC-32C), length=5, payload AA BB CC DD EE, trailer_crc = CRC-32C(payload) = 0x2312C9B6 (little-endian B6C91223). CRC covers payload (+ trailer_ts when present), never the header. CRC-32C: poly 0x1EDC6F41, init 0xFFFFFFFF, final XOR 0xFFFFFFFF. 13 bytes total.

```
01 10 05 00 AA BB CC DD EE B6 C9 12 23
```
