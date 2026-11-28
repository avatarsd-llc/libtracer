# framing/length-oversize-reject

Negative vector (the input file is `reject.bin`, not `input.bin`): a VALUE whose long-form
(`LL = 1`) length is `0xFFFFFFFF`, with no payload after the header. The stable harness error
name is `FRAME_INVALID` (`tr::frame::invalid`).

```
01 08 FF FF FF FF
```

```
01 08 FF FF FF FF   VALUE, opt = 0x08 (LL = 1), length = 4294967295; the frame ends here
```

6 bytes total.

annex 01 §Truncation handling, RFC-0001 §C.3 and RFC-0002: a header that declares
`length > MAX_TLV_BYTES` (implementation-defined, recommended 16 MiB) is rejected as
`tr::frame::invalid` without allocating the segment. `0xFFFFFFFF` is above any bound a
receiver can hold, so the answer is `INVALID` and not `TRUNCATED`, though the bytes also end
early. The companion `framing/truncated-length-max-u16` declares `0xFFFF`, which is below the
bound, and stays `FRAME_TRUNCATED`.

The bound applies to every header: a TLV declaring more than `MAX_TLV_BYTES` is `INVALID` even
when all of its bytes are present. These bytes happen to be short too, and the answer is still
`INVALID`, because the header check comes before the fit check.

The harness checks the error name itself. The C++ codec's two size widths are bound by
`core/tests/grammar_bounds_test.cpp`.
