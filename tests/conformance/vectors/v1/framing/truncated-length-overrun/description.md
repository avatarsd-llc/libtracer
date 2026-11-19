# framing/truncated-length-overrun

Negative vector (the input file is `reject.bin`, not `input.bin`): a VALUE whose header
declares four payload bytes while only two follow. The stable harness error name is
`FRAME_TRUNCATED` (`tr::frame::truncated`).

```
01 00 04 00 AA BB
```

```
01 00 04 00     VALUE, opt = 0x00, length = 4
   AA BB        two bytes; the frame ends here
```

6 bytes total.

annex 01 §Truncation handling: an implementation MUST validate `length` against the bytes it
has before it reads any payload byte, and on a datagram transport the short frame is
`ERROR{tr::frame::truncated}`. A stream transport waits for more bytes instead. That is the
link's framing, not the codec's, and #2402 tracks it. A decoder that reads the missing two bytes
has overrun its buffer, so a successful decode is a conformance failure.

The harness checks the error name itself, so this vector needs no separate host test.

The oversize-length case has no vector yet. TODO(#2885): bank `01 08 FF FF FF FF`, a
long-form (`LL = 1`) length of `0xFFFFFFFF`, as `FRAME_INVALID` (`tr::frame::invalid`) once the
codec's compile-time `MAX_TLV_BYTES` bound lands. `framing/truncated-length-max-u16` is the
short-form case: `0xFFFF` is below the bound and stays `FRAME_TRUNCATED`.
