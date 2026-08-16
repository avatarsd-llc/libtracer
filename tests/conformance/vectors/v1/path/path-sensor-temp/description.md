# path/path-sensor-temp

The canonical static PATH handle for /sensor/temp (spec 3.1, reference 05 0x06). Outer: type=0x06 PATH, opt=0x00 (PL MUST be 0 under the packed body of [RFC-0018](../../../../../docs/spec/rfcs/0018-packed-path-segments.md); NOT 0x40, the pre-RFC-0018 spelling, and NOT 0x10, which would add CR), length=12. Two packed segment records, each `[u8 len][utf8]`: `06 "sensor"` (7 bytes), `04 "temp"` (5 bytes). A record has no type byte, no option byte and no trailer, and carries NO NUL terminator. 16 bytes total as graph data — 6 fewer than the 22 the retired NAME-child body cost.

```
06 00 0C 00 06 73 65 6E 73 6F 72 04 74 65 6D 70
```
