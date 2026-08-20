# The TLV header and the `opt` byte (L2/L3 codec)

Every TLV on the wire opens with four bytes: a type code, the one-byte `opt` bitfield, and a
little-endian `u16` length — widening to a `u32`, six bytes in all, once the body exceeds
`0xFFFF` ([reference 01](../reference/01-data-format.md) §header + opt). There is no varint
and no checksum in the header; integrity is a [trailer](wire-trailer.md), and it is optional.
This is the first thing to read before any other page in this domain.

## What to notice

- **`opt` is a bitfield with two reserved bits.** `opt_t::encode` / `opt_t::decode` round-trip
  the six meaningful bits exactly, and bits 7 and 0 are reserved-MUST-be-zero. `reserved_set`
  answers that question about a *raw* byte, before anything has been parsed — a set reserved
  bit makes the frame invalid ([wire-format bits](../modules/wire-format-bits.md)).
- **The length width is `emit_tlv`'s decision, not the caller's.** The example passes
  `opt_t{.ll = false}` with a 64 KiB body and gets a six-byte header with `LL` set anyway.
  Before [#924](https://github.com/avatarsd-llc/libtracer/issues/924) a programmatically built
  tree could serialize a length truncated to `size & 0xFFFF`; the widen rule now lives with
  whoever writes the header.
- **`emit_header` is one level below, and does *not* decide.** It writes the width `opt.ll`
  names, verbatim. A byte-builder that reaches for it owns the width decision itself — which
  is safe when a grammar bound already caps the length, and a bug otherwise.
- **Nothing here is conditional.** The target builds and runs identically under every CI leg,
  net plane on or off.

## Source

```{literalinclude} /core/examples/wire_tlv_header.cpp
:language: cpp
:linenos:
```

See also: [frame codec](../modules/frame-codec.md) ·
[wire-format bits](../modules/wire-format-bits.md) ·
[data-format reference](../reference/01-data-format.md).
