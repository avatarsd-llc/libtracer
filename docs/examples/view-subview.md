# `subview`: narrowing a window costs arithmetic (L1 views)

A view is `{owner, offset, length}`, so narrowing one is arithmetic on the offset and the
length plus a refcount bump ([reference 08](../reference/08-views-and-ownership.md)). This is
the primitive every zero-copy slice in the library reduces to: a child TLV's payload, a routed
path suffix, a value handed to a subscriber. The pointers are the proof — a sub-window's
`bytes().data()` is the parent's plus the offset, in the same segment.

## What to notice

- **Nesting composes against the *segment*, not the window.** `whole.subview(4, 8).subview(2, 4)`
  has `offset == 6` — offsets accumulate, and the second call's `2` is relative to the first
  window. Reading byte 0 of the inner view yields byte 6 of the segment.
- **Narrowing extends lifetime.** The sub-window holds its own reference, so it may outlive the
  wide view it came from. That is what lets a decoder hand a caller a payload and forget the
  frame.
- **The window invariant is debug-asserted.** `bytes()` asserts `offset + length <= owner->bytes.size()`
  — zero cost in a release build, and the reason an out-of-bounds window is caught at its
  *source* on the sanitizer CI leg rather than as a mysterious read somewhere downstream.
- **A view is one contiguous window over one segment.** Spanning several segments is the
  [rope](view-rope-compose.md)'s job, and taking a sub-range of one is
  [`subrope`](view-rope-subrope.md) — the same idea one level up.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/view_subview.cpp
:language: cpp
:linenos:
```

See also: [views module](../modules/views.md) · [segment module](../modules/segment.md) ·
[views & ownership reference](../reference/08-views-and-ownership.md).
