# The segment, and its refcount (L1 views)

Almost every other example on this site opens by allocating a **segment** and putting a
**view** over it. This page is what that line means.

A segment is the L0↔L1 boundary object: real bytes, the backend that reclaims them, and an
intrusive refcount ([reference 08](../reference/08-views-and-ownership.md)). A view is a
`{owner, offset, length}` window over one segment, and it holds a `segment_ptr_t` — so copying
a view **clones the reference** and the bytes stay alive as long as any view names them. That
is the entire safety argument behind zero-copy fan-out: a decoded TLV's spans remain valid
because the view that produced them is still holding its segment.

## What to notice

- **Copy means clone, not copy.** `use_count()` moves 1 → 2 → 3 as a view is taken and then
  copied, and both windows report the same `bytes().data()`. No payload byte is touched.
- **The last drop is what reclaims.** Reclaim is not a destructor on the view; it is the
  backend's `destroy`, fired when the refcount reaches zero — wherever that happens, on
  whatever thread. The example uses a `pool_t` over a stack slab so the free slot *count*
  moves visibly; the heap backend reclaims just as correctly and invisibly.
- **That "whatever thread" is a real obligation.** A subscriber or a transport receive thread
  is a normal place for a last reference to die, which is why a backend at a shared seam must
  be thread-safe — see [`view_sync_pool`](view-sync-pool.md).
- **`use_count()` is diagnostics, not synchronization.** It is an acquire load for a human, and
  reading it never makes a decision safe.
- **`view_t::over` takes the handle by value.** Passing a named handle *copies* it (the count
  goes up); `std::move`-ing it transfers. The example passes a copy deliberately so that `seg`
  keeps its own reference to inspect.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/view_segment_refcount.cpp
:language: cpp
:linenos:
```

See also: [segment module](../modules/segment.md) · [views module](../modules/views.md) ·
[views & ownership reference](../reference/08-views-and-ownership.md).
