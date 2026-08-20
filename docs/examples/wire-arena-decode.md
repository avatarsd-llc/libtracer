# `decode_into`: a flat arena drawn from a stack slab (L2/L3 codec)

`decode` returns an owning `tlv_t` whose `children` vectors allocate on the global heap by
construction — fine on a host, wrong at a terminus that must not touch the heap on the receive
path. `wire::decode_into`
([ADR-0041](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0041-terminus-arena-decode-span-contract.md))
answers the identical grammar with a flat, pre-order `arena_tlv_t` array whose storage comes
from an injected **block source** ([CONTEXT.md](../../CONTEXT.md) §Block source). Point that at
a `bump_source_t` over a `std::array` and the whole decode allocates nothing.

## What to notice

- **The arena holds structure; the bytes stay where they were.** Every span in a node borrows
  the input buffer, which must outlive the arena. The example asserts a node's `body` points
  *into* the frame, not beside it.
- **Navigation is index arithmetic.** Children of node `i` begin at `i + 1`, and `end` is one
  past the last descendant — so `next_sibling` is a single load, and skipping a subtree costs
  the same as reading one field. No pointer chasing, no per-node allocation.
- **`null_source()` upstream makes the slab a hard bound.** A `bump_source_t` spills to its
  upstream once full; passing the source that serves nothing means an overflowing frame is
  refused (`TLV_NESTING_TOO_DEEP` — see [decode refusals](wire-decode-refusals.md)) instead of
  quietly reaching the heap. That is the seam doing its job: exhaustion by value, never a throw
  and never an `abort()` on a `-fno-exceptions` target.
- **A bump source is scope-lifetime storage.** Blocks are never individually reclaimed, so it
  is constructed per operation (or `reset` between them) — not wired as a long-lived receiver
  seam, where it would monotonically fill and then refuse everything.
- **This is a resolve-scoped object.** Read it, take the ownership copies you need, drop it.
  Storing a borrowed span is the one thing the ADR-0041 contract forbids.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/wire_arena_decode.cpp
:language: cpp
:linenos:
```

See also: [frame codec](../modules/frame-codec.md) · [backends](../modules/backends.md) ·
[memory substrate reference](../reference/09-memory-substrate.md).
