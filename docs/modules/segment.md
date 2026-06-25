# segment — refcounted bytes (L0)

```{admonition} In one paragraph
:class: tip
A **`Segment`** is real bytes owned by a backend, plus an intrusive atomic
refcount. The **`SegmentPtr`** handle threads that one buffer's lifetime through
fan-out: copying a handle is a relaxed increment (a *clone*), dropping the last one
runs the backend's reclaim. This is what lets many views share one buffer with no
copies, and what makes a decoded TLV safe to hold past the receive call.
```

## What it does

L0 is "real bytes in real memory owned by some real allocator." `Segment` is the
control block over one such buffer; `SegmentPtr` is the owning handle. The refcount
lives **inside** the segment (not in a side `shared_ptr` block) so a static MMIO
descriptor, a pool slot, and a heap allocation all carry their own count. When the
last `SegmentPtr` drops, the segment's `backend->destroy(seg)` returns the bytes to
wherever they came from.

The atomic orderings are the **canonical intrusive_ptr** pattern, mandated by the
spec (`reference/02` §required atomic operations): increment `relaxed` (the caller
already holds a reference), decrement `acq_rel` (publish writes before the count
drops; synchronize on reaching zero). For single-threaded / Cortex-M0 builds,
`-DLIBTRACER_NO_ATOMIC` swaps the atomic for a plain integer.

## Interface

```cpp
struct Segment {
    detail::RefCount refcount;       // atomic (or plain under LIBTRACER_NO_ATOMIC)
    MemBackend*      backend;        // reclaims on last release
    std::span<std::byte> bytes;      // the real buffer
};

class SegmentPtr {                   // intrusive owning handle
    static SegmentPtr adopt(Segment*);   // take an existing ref (e.g. from alloc), no bump
    static SegmentPtr retain(Segment*);  // take a NEW shared ref (relaxed ++)
    SegmentPtr(const SegmentPtr&);       // copy == clone == relaxed ++
    void reset();                        // acq_rel --; backend->destroy on zero
    Segment* operator->() const;  Segment& operator*() const;
    std::uint_least32_t use_count() const;   // acquire load — debug/metrics only
};
```

## Refcount lifecycle (fan-out)

```{mermaid}
sequenceDiagram
    participant TX as producer
    participant V as views
    participant S1 as subscriber 1
    participant S2 as subscriber 2
    participant B as backend
    TX->>V: make segment (count=1)
    V->>S1: clone (relaxed ++ → 2)
    V->>S2: clone (relaxed ++ → 3)
    TX->>V: drop producer ref (-- → 2)
    S1->>V: release (-- → 1)
    S2->>V: release (acq_rel -- → 0)
    V->>B: backend->destroy(seg) — bytes reclaimed
```

## Benefits

- **Zero-copy fan-out** — N subscribers share one buffer; delivery is N relaxed
  increments, no `memcpy`.
- **Closes M1's lifetime gap** — a decoded `Tlv` borrows segment bytes via spans;
  the `SegmentPtr` keeps them alive exactly as long as some view needs them.
- **No hidden allocation** — the count is in the segment, so MMIO/pool/borrowed
  segments need no separate control block.
- **Portable to MCUs** — `LIBTRACER_NO_ATOMIC` drops to a plain counter where there
  is no cross-thread sharing.

See: [backends](backends.md) (who creates segments), [views](views.md) (who holds
them), and the [interface map](interface-map.md).
