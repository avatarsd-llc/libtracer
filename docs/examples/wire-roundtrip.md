# Wire codec round-trip (L2/L3)

The [frame codec](../modules/frame-codec.md) is where a `tlv_t` model becomes bytes and
bytes become a validated `tlv_node_t`. This example builds a packed `PATH` TLV for
`/sensor/temp` (RFC-0018) with a CRC trailer, `encode`s it to wire bytes, reads those bytes
back with `tlv_node_t::over`, and proves the round-trip is exact.

## What to notice

- **The read borrows, never copies** — the node's packed body is a `std::span` that
  points *into* the encoded buffer; the example checks the payload address lies inside
  `wire`. Keeping a node means keeping its backing bytes alive (that is what
  [views](../modules/views.md) provide).
- **The CRC trailer is verified on read** — `opt.cr` makes `encode` append a CRC-32C over
  the body; `over` recomputes and checks it, and `trailer()` surfaces the parsed `crc_t`.
- **The round-trip invariant** — re-encoding what was read reproduces the *exact* wire
  bytes (`encode(decode(bytes)) == bytes`), CRC and all.

## Source

```{literalinclude} /core/examples/wire_roundtrip.cpp
:language: cpp
:linenos:
```

See also: [frame-codec module](../modules/frame-codec.md) ·
[bit-level wire walkthrough](../modules/wire-format-bits.md) ·
[data-format reference](../reference/01-data-format.md).
