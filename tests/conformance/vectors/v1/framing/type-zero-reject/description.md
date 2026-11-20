# framing/type-zero-reject

Negative vector (the input file is `reject.bin`, not `input.bin`): a TLV with `type = 0x00`
whose header is otherwise well formed. The stable harness error name is `FRAME_INVALID`
(`tr::frame::invalid`).

```
00 00 01 00 AA
```

```
00 00 01 00     type = 0x00 (reserved sentinel), opt = 0x00, length = 1
   AA           one payload byte
```

5 bytes total.

annex 01 §Type code registry: `0x00` is never assigned. It indicates a zeroed buffer or framing
corruption, and receivers MUST treat it as INVALID. The opt bits, the length and the payload are
all legal here, so the type byte is the only thing that can fail the frame. A decoder that treats
`0x00` as just another unknown code and skips it by length fails this vector.

The harness checks the error name itself, so this vector needs no separate host test.
