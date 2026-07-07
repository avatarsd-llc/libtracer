# Wire codec deep-dive & throughput (L2/L3)

Where [wire codec round-trip](wire-roundtrip.md) proves byte-identity, this is the
**anatomy and performance** companion. It builds a `POINT` TLV carrying two `VALUE`
children with a CRC trailer, prints the encoded size and the raw header bytes, then
times `encode` (model → bytes), `decode` (bytes → borrowed tree), and the full
round-trip over 50,000 iterations. See the
[frame codec](../modules/frame-codec.md) module and the
[bit-level walkthrough](../modules/wire-format-bits.md) for the byte layout.

## What to notice

- **The header is tiny and fixed** — the example prints the first bytes (type, `opt`, the
  fixed-width length); a `POINT{VALUE,VALUE}` with a CRC trailer is 24 bytes total.
- **Decode is zero-copy** — each child's payload is a `std::span` borrowing the encoded
  buffer; the example checks the payload address lies inside `wire`.
- **Encode/decode are separately timed** — decode (validate + build the borrowed tree) is
  typically cheaper than encode (materialize the byte vector + CRC); the `RESULT` line
  reports both plus the round-trip rate.

```{note}
The absolute nanoseconds come from whatever build ran (CI builds the examples in a debug
configuration); treat them as a *shape*, not a spec number. The canonical, release-build,
CI-published codec figures — including the cross-core cpp/ts/rust comparison — live on the
[performance page](../performance.md).
```

## Source

```{literalinclude} /core/examples/wire_codec.cpp
:language: cpp
:linenos:
```

See also: [frame-codec module](../modules/frame-codec.md) ·
[wire codec round-trip](wire-roundtrip.md) ·
[bit-level wire walkthrough](../modules/wire-format-bits.md).
