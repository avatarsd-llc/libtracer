# tlv-types/subscriber-path

SUBSCRIBER{PATH{`06 "sensor"`, `04 "temp"`}} — emitted by the library encoder (field_write pattern).

The `PATH` body is a packed run of `[u8 len][utf8]` **segment records**
([RFC-0018](../../../../../docs/spec/rfcs/0018-packed-path-segments.md)), so the `PATH` carries
`opt=0x00` (`PL` MUST be 0) and holds no child TLVs; the outer SUBSCRIBER is still structured
(`opt=0x40`, PL=1). 20 bytes total — 6 fewer than the retired `NAME`-child spelling.

```
0440100006000c000673656e736f720474656d70
```
