# framing/truncated-length-max-u16

Negative vector (the input file is `reject.bin`, not `input.bin`): a VALUE whose short-form
(`LL = 0`) length is `0xFFFF`, the largest a u16 can declare, followed by one payload byte. The
stable harness error name is `FRAME_TRUNCATED` (`tr::frame::truncated`).

```
01 00 FF FF AA
```

```
01 00 FF FF     VALUE, opt = 0x00, length = 65535
   AA           one byte; the frame ends here
```

5 bytes total.

annex 01 §Truncation handling separates two failures: a frame that ends before its declared
length is `tr::frame::truncated`, and a header that declares `length > MAX_TLV_BYTES` is
`tr::frame::invalid`. 65535 bytes is below the recommended 16 MiB `MAX_TLV_BYTES`, so this
frame is short, not oversize. A decoder that reports `INVALID` for every large length fails this
vector. `framing/truncated-length-overrun` is the small-length companion.

The harness checks the error name itself, so this vector needs no separate host test.
