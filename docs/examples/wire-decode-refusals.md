# What `decode` refuses, and who each refusal accuses (L2/L3 codec)

`decode` answers `std::expected<tlv_t, err_t>`, and the error side is the RFC-0002 registry
([status module](../modules/status.md)) rather than a decode-private vocabulary — so
`err_path`, severity and disposition come for free. Four verdicts are reachable, and they do
not all mean the same *kind* of thing.

Three are permanent accusations about the bytes: `FRAME_TRUNCATED` (the frame stops mid-TLV),
`FRAME_INVALID` (a clean TLV with bytes left over — or a reserved `opt` bit set) and
`FRAME_CRC_FAIL` (see [the trailer](wire-trailer.md)).

## What to notice

- **`decode` consumes the *whole* input.** A single trailing byte after a well-formed TLV is
  `FRAME_INVALID`, not a successful parse of the prefix. A stream reader frames first and
  decodes exactly one TLV's worth.
- **A reserved `opt` bit is checked before anything is believed.** Bits 7 and 0 are
  MUST-be-zero; a peer that sets one has said something this version cannot interpret, and the
  frame is refused rather than partially honoured.
- **`TLV_NESTING_TOO_DEEP` is different in kind.** It means "exceeds **this receiver's** decode
  resources"
  ([RFC-0006](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0006-resource-bounded-nesting-depth.md),
  [CONTEXT.md](../../CONTEXT.md) §Resource bound). The structural walk starts in inline stack
  slots and spills into a caller-injected `block_source_t`; `decode(bytes, mem::null_source())`
  is the spelling of "no spill at all", and the **same bytes** decode once the caller injects a
  source that can serve. The bound is the injected resource, and two receivers may legitimately
  disagree about one frame.
- **The example asserts the verdict, never a depth number.** There is no constant to assert.
  The inline slot count is a tuning knob whose overflow changes cost, not behaviour — see
  [the arena decode](wire-arena-decode.md) for the same seam used deliberately, and note that
  neither a "depth cap" nor a `kMaxDepth` exists to point at.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/wire_decode_refusals.cpp
:language: cpp
:linenos:
```

See also: [frame codec](../modules/frame-codec.md) · [status module](../modules/status.md) ·
[data-format reference](../reference/01-data-format.md).
