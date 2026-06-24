# path/path-sensor-temp

The canonical static PATH handle for /sensor/temp (spec 3.1, reference 05 0x06). Outer: type=0x06 PATH, opt=0x40 (PL=1 only; NOT 0x50, which would add CR), length=18. Two NAME children with no inner trailer: NAME sensor (02 00 06 00 + utf8), NAME temp (02 00 04 00 + utf8). NAMEs carry NO NUL terminator. 22 bytes total as graph data.

```
06 40 12 00 02 00 06 00 73 65 6E 73 6F 72 02 00 04 00 74 65 6D 70
```
