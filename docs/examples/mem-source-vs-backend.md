# Two L0 seams, and the question that picks (L0 substrate)

L0 has two injection points, not one, and reaching for the wrong one is the most common way a
first embedding goes sideways. `tr::mem::mem_backend_t` vends a **refcounted**
[`segment_t`](view-segment-refcount.md) — bytes plus a control block plus an intrusive count —
so many views can share one buffer and the last drop reclaims it.
`tr::mem::block_source_t` vends **raw bytes with a single owner and no header at all**.

The question that picks between them is not "how big" or "how hot". It is **how many owners
the bytes will have** ([reference 09](../reference/09-memory-substrate.md) §the second L0 seam).

## What to notice

- **Payload is shared, so payload is a segment.** A decoded frame's spans, a value published
  to several subscribers, a rope link handed to a transport: all of these are held by more than
  one holder at once, and the refcount is what makes a borrowed (zero-copy) view safe. The
  example copies a handle and watches `use_count()` go to 2 while the byte pointer stays
  identical — copy is a clone, never a second buffer.
- **A registered object has exactly one owner, so it is a source block.** A vertex, a route
  label, a reassembly entry — a refcount on them is pure overhead, and it is measurable
  overhead: a `segment_t` is 20 B on rv32 / 40 B on x86-64 against an 80 B `vertex_t`.
- **"No header" is checkable, and the example checks it.** A 64-byte slab serves exactly one
  64-byte block and then nothing. Any per-block bookkeeping would show up as a short slab
  immediately.
- **Reclaim is a *call* on one seam and a *consequence* on the other.** A source block comes
  back because its one owner said so; a segment comes back because the last handle dropped.
  Mixing the two models is what produces either a leak or a use-after-free, and the type system
  is what keeps them apart — there is no conversion between the seams in either direction.
- **They are injected independently.** A node may point both at the same underlying store
  ("one slab, whole stack") or split them — a bounded value backend for payload, a bounded
  control source for registration.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/mem_source_vs_backend.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) · [segment module](../modules/segment.md) ·
[memory substrate reference](../reference/09-memory-substrate.md) ·
[the segment, and its refcount](view-segment-refcount.md).
