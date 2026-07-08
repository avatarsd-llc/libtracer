# framing/reserved-bit0-reject

Negative vector (note the input file is `reject.bin`, not `input.bin`): a 1-byte VALUE whose opt byte is 0x01 — reserved bit 0 set, every defined bit clear. Reserved bits in `opt` non-zero MUST be rejected as INVALID; the stable harness error name is `FRAME_INVALID`.

Bit 0 is the low reserved bit: a core whose TF mask is off-by-one toward the LSB would read this as a defined bit instead of rejecting — this vector catches exactly that shift.

```
01 01 01 00 AA
```

5 bytes total.
