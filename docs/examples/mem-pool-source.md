# A long-lived bounded seam has to recycle (L0 substrate)

`heap_source_t` recycles but is unbounded. [`bump_source_t`](mem-bump-source.md) is bounded but
never recycles. A seam that outlives one unit of work — a graph's control source, a receiver's
decode arena — needs both, and until `tr::mem::pool_source_t` existed such a seam could not be
bounded at all
([ADR-0067](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md)).

It is segregated free lists over a slab the caller owns, with **no per-block header** — the
seam's [sized `release`](mem-block-source.md) makes one unnecessary.

## What to notice

- **`used()` is a high-water mark, not a running total.** That one sentence is the whole
  example: ten thousand alloc/release rounds over a 512-byte slab settle at 128 bytes, because
  once every *live* block has been carved once, a round costs nothing. A bump source in the same
  position would have wanted 1.28 MB and refused after the first eight rounds.
- **Both bounds are injected.** The caller supplies the slab **and** the span of `size_class_t`
  slots, so neither the byte ceiling nor the class count is a constant inside the library — a
  bounded node is a property the deployer states, not one the library grants.
- **Recycling is LIFO on the exact shape.** A released block is the next one handed out, which
  the example pins by pointer identity — the cheapest possible reuse, and the reason the free
  list can live *inside* the free blocks.
- **Exact packing is the visible consequence of the header-free scheme.** A 512-byte slab holds
  exactly eight 64-byte blocks. No rounding, no bookkeeping, nothing to subtract.
- **Full is still `nullptr`.** A bounded source never falls back to the platform heap and never
  aborts; the ninth block is a refusal the caller answers with backpressure.
- **Own one per receiver — do not share one across receive threads.** A shared free-list pool
  collapses to roughly **a fifteenth** of its own single-thread rate on a 12-core host
  (8.3 M → 1.36 M ops/s, p50 60 ns → 3587 ns) *while the platform heap scales*, so the problem is
  the shared cacheline and not the flavour of the guard — a lock-free CAS on the list head
  replaces one contended word with the same word
  ([ADR-0060 erratum 1](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md)).
  A `Sync` policy belongs only at *wiring* frequency, never per frame.
- **Nothing here is conditional** — the target builds and runs under every CI leg, and it uses
  no threads.

## Source

```{literalinclude} /core/examples/mem_pool_source.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) ·
[memory substrate reference](../reference/09-memory-substrate.md) ·
[classes do not share](mem-size-classes.md) ·
[concurrency & scaling reference](../reference/15-concurrency-and-scaling.md).
