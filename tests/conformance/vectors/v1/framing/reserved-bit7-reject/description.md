# framing/reserved-bit7-reject

Negative vector (note the input file is `reject.bin`, not `input.bin`): a 1-byte VALUE whose opt byte is 0x80 — reserved bit 7 set, every defined bit clear. Reserved bits in `opt` non-zero MUST be rejected as INVALID (hard error, no silent semantic drift); the stable harness error name is `FRAME_INVALID`.

A core that decodes this frame successfully has its reserved-bit mask wrong (or missing) and fails the vector.

```
01 80 01 00 AA
```

5 bytes total.
