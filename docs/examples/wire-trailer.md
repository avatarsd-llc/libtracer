# The trailer: an opt-in CRC and an opt-in timestamp (L2/L3 codec)

Integrity rides at the **end** of a TLV, and it is **optional**
([reference 01](../reference/01-data-format.md) §trailer). `opt.cr` says a CRC-32C follows the
body — `encode` computes it, `decode` checks it. `opt.ts` says a wire-time stamp follows.
Neither is in the header, and a TLV that wants neither pays for neither, which is what makes
the codec usable as a transparent byte router over live memory.

## What to notice

- **A flipped byte is a verdict, not a guess.** One XOR into the payload turns the frame into
  `err_t::FRAME_CRC_FAIL`. The example flips a byte it wrote itself, so the failure is about
  the *bytes* — a receiver never learns anything about intent from a CRC.
- **`stamp_ts` sets the bit and the value together.** That is not a convenience; it is what
  keeps a stamped TLV away from `encode`'s refusal below. It writes the **absolute** (TF=0)
  form only, deliberately: the relative form is anchored to the parent's stamp, and the
  anchorless-reject rule is a conformance gap the reference codec has not closed.
- **The refusal is loud.** A TLV whose `opt.ts` is set by hand with no trailer value behind it
  makes `encode` return an **empty vector**
  ([#1109](https://github.com/avatarsd-llc/libtracer/issues/1109)) rather than emit a silently
  zero stamp. Empty is unambiguous: a well-formed TLV always carries at least its four-byte
  header, so nothing valid encodes to nothing.
- **The trailer timestamp is *transport* time.** Sample-acquisition or control-deadline time
  is application-domain and rides the payload as a `TIME` child instead — a different clock
  with different meaning.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/wire_trailer.cpp
:language: cpp
:linenos:
```

See also: [frame codec](../modules/frame-codec.md) ·
[wire-format bits](../modules/wire-format-bits.md) ·
[data-format reference](../reference/01-data-format.md) ·
[wire codec round-trip](wire-roundtrip.md).
