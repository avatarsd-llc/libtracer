# Wire codec round-trip (L2/L3)

The [frame codec](../modules/frame-codec.md) is the one place bytes become a `tlv_t` tree
and back. This example builds a structured `PATH` TLV for `/sensor/temp` (two `NAME`
children) with a CRC trailer, `encode`s it to wire bytes, `decode`s those bytes into a
fresh tree, and proves the round-trip is exact.

## What to notice

- **Decode borrows, never copies** — the decoded `NAME` payloads are `std::span`s that
  point *into* the encoded buffer; the example checks the payload address lies inside
  `wire`. Keeping a decoded `tlv_t` means keeping its backing bytes alive (that is what
  [views](../modules/views.md) provide).
- **The CRC trailer is verified on decode** — `opt.cr` makes `encode` append a CRC-32C over
  the body; `decode` recomputes and checks it, and surfaces the parsed `crc_t`.
- **The round-trip invariant** — re-encoding the decoded tree reproduces the *exact* wire
  bytes (`encode(decode(bytes)) == bytes`), CRC and all.

## Source

```{literalinclude} /core/examples/wire_roundtrip.cpp
:language: cpp
:linenos:
```

See also: [frame-codec module](../modules/frame-codec.md) ·
[bit-level wire walkthrough](../modules/wire-format-bits.md) ·
[data-format reference](../reference/01-data-format.md).
