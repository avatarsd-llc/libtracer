# Classes do not share, and the pool says how to size them (L0 substrate)

[`pool_source_t`](mem-pool-source.md) recycles through segregated free lists keyed on the whole
`(bytes, align)` pair. So a freed 64-byte block cannot serve a 128-byte request, and a block
carved on an 8-byte boundary is never handed back out to an `align=64` caller. That is the price
of the header-free scheme, and the reason the class **span** is a sizing decision a deployment
has to make rather than a detail it can ignore.

## What to notice

- **The limitation is measured, and it still wins.** Replaying 70,937 recorded allocations from
  the host suite — 12 distinct sizes, three of which cover 99.8 % of them — this policy needed
  26,176 B against a 23,552 B peak-live floor (**+11.1 %**), where first-fit with boundary-tag
  coalescing needed 27,448 B (+16.5 %) and TLSF 28,440 B (+20.8 %). Re-running with the header
  zeroed decomposes the gap as **1,088 B of external fragmentation against only 184 B of
  header**: splitting a remainder under geometric growth rarely produces the size of the next
  request
  ([ADR-0067](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)).
  Note what that says about the usual argument for a header-free pool — here the header is worth
  0.7 % of the difference, so it is *not* the reason to choose this shape.
- **Alignment is part of the key, not folded away.** It has to be: a block carved on an 8-byte
  boundary cannot be promised to a caller who asked for 64. The example allocates at both and
  watches the two classes stay separate.
- **`classes_used()` is the number to size the span against.** It reports the distinct shapes
  actually seen, which is a measurement a deployment can take on its own workload instead of
  guessing.
- **`overflowed()` is the alarm, and a non-zero value is safe but lossy.** When the class table
  is full, a released block of an unfiled shape stays carved rather than being written anywhere
  unsafe — bounded and correct, never corrupt — and the loss is counted. The example
  deliberately injects a two-slot span, uses three shapes, and reads the counter.
- **An overflowed table degrades recycling; it does not break the source.** The recorded classes
  keep recycling and the slab keeps serving. Treat a non-zero `overflowed()` as "the injected
  span is too small", not as a fault.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/mem_size_classes.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) ·
[memory substrate reference](../reference/09-memory-substrate.md) ·
[a long-lived seam has to recycle](mem-pool-source.md).
