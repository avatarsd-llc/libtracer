# Wire codec deep-dive & throughput (L2/L3)

Where [wire codec round-trip](wire-roundtrip.md) proves byte-identity, this example is the
**anatomy and cost** companion. It builds a `POINT` TLV carrying two `VALUE` children with
a CRC trailer, prints the encoded size and the raw header bytes, then times `encode`
(model → bytes), `decode` (bytes → borrowed tree), and the full round-trip over 50,000
iterations (`kIters`, `core/examples/wire_codec.cpp:94`). See the
[frame codec](../modules/frame-codec.md) module and the
[bit-level walkthrough](../modules/wire-format-bits.md) for the byte layout.

## Anatomy and timing

- **The header is small and fixed in shape** — the example prints the leading bytes (type,
  `opt`, the fixed-width length). It also prints the encoded size, read from the buffer at
  run time (`wire.size()`, `core/examples/wire_codec.cpp:71`); that size is reported, not
  checked against a constant, so it is a property of the run and not a documented figure.
- **Decode borrows rather than copies** — each child's payload is a `std::span` over the
  encoded buffer, and the example checks that the first child's payload address lies inside
  `wire`.
- **Encode and decode are timed apart** — encode materializes the byte vector and computes
  the CRC; decode validates and builds the borrowed tree. The `RESULT` line reports the two
  separately plus the round-trip rate, so neither half is hidden inside the other.
- **The correctness properties are gated, the timings are not** — the example returns
  non-zero on any failed check and runs under ctest as `example_wire_codec`
  (`core/examples/CMakeLists.txt:92`), unconditionally of the net plane.

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
