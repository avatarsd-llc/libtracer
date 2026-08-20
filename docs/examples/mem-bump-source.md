# A bump source: carve, and never take back (L0 substrate)

`tr::mem::bump_source_t` is the nothrow twin of `std::pmr::monotonic_buffer_resource`: a cursor
over a span the **caller** owns. `try_alloc` moves the cursor forward, `release` of a block from
that span is a **no-op**, and `reset()` is the only way storage comes back — all of it, at once.

That is a lifetime rule rather than a performance note, and it decides where a bump source may
be wired at all.

## What to notice

- **It is a scope-lifetime object.** Construct it, do one unit of work, drop it or `reset()` it.
  The branch-write decode does exactly that with a 4 KiB stack buffer; so does
  [`wire_arena_decode`](wire-arena-decode.md).
- **Parked as a long-lived seam it dies, and the number is arithmetic rather than anecdote.**
  An 8 KiB bump source wired as a router's `rx`, decoding a 53-byte FWD, decoded **6 frames and
  rejected the next 194** — 8192 bytes divided by the arena footprint of one decode of that
  frame, so the figure does not vary with host or build flags. A long-lived bounded seam wants
  [`pool_source_t`](mem-pool-source.md), which recycles.
- **`release` of a bump block returns nothing, and the example asserts it.** `used()` does not
  retreat. This is the same behaviour a monotonic resource has, stated where a reader will trip
  over it rather than in a footnote.
- **`reset()` is deliberately not called `release`.** On a `monotonic_buffer_resource` that name
  means exactly this whole-buffer operation, while on a `block_source_t` it means "return one
  block" — two meanings one token apart at a call site is a defect waiting to be written, so the
  seam refuses to spell them the same.
- **Padding is visible.** `used()` counts alignment padding as well as payload, so an
  over-aligned request costs more than its size. The example asks for 40 bytes and then a
  16-aligned 16, and watches the cursor land on 64.
- **Every block from the buffer dangles after a `reset()`.** It is only safe when the previous
  unit of work is entirely finished — which is what "scope-lifetime" means operationally.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/mem_bump_source.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) ·
[memory substrate reference](../reference/09-memory-substrate.md) ·
[the upstream is the bound](mem-bump-upstream.md).
