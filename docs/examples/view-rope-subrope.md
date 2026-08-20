# `subrope` and the iovec egress (L1 views)

A rope's sub-range is taken by trimming the covering links with
[`subview`](view-subview.md) and re-chaining them, so a window whose start falls **mid-link**
costs one arithmetic pass and a few refcount bumps ([reference 08](../reference/08-views-and-ownership.md)).
This is the region primitive of the lazy decode tier: a child TLV, a routed path suffix, a
payload handed to the next hop is a *subrope* of the inbound frame, never a copy of it.

It also narrows ownership — the sub-rope keeps alive exactly the segments its window touches,
and releases the rest.

## What to notice

- **The window is not link-aligned, and does not need to be.** `subrope(2, 7)` over three
  4-byte links starts two bytes into the first and stops one byte into the third: the first
  link is trimmed to its tail, the last to its head, and the trimmed link still addresses the
  **original** segment's bytes.
- **`walk()` is what a parser or a CRC does.** It visits each link's contiguous bytes in order,
  so a logically contiguous read never requires physically contiguous storage.
- **`to_iovec` is the transport-agnostic scatter-gather form.** One span per link, straight
  into `writev`/`sendmsg`. Each transport *lowers* the rope to its native DMA — the substrate
  does not choose it.
- **`try_to_iovec` is the nothrow twin, and it exists for a reason.** `to_iovec`'s `reserve`
  throws on OOM, which under `-fno-exceptions` is an `abort()`; a terminus that builds this
  table per send would take the node down on a fragmented heap. The nothrow form refills a
  caller's vector and soft-fails instead.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/view_rope_subrope.cpp
:language: cpp
:linenos:
```

See also: [views module](../modules/views.md) · [rope scatter-gather](rope-scatter.md) ·
[views & ownership reference](../reference/08-views-and-ownership.md).
