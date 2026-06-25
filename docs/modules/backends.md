# backends — the allocator seam (L0)

```{admonition} In one paragraph
:class: tip
**`MemBackend`** is a small, user-implementable interface: subclass it to bind
libtracer to *any* memory — a heap, a fixed arena, live registers, a DMA ring.
libtracer never allocates on its own; it asks a backend. Three ship today:
**`mem_heap`** (owns malloc'd bytes), **`mem_borrowed`** (wraps your bytes, frees
nothing), and **`mem_pool`** (a bounded fixed-slab, `alloc`-or-`null`).
```

## What it does

The protocol treats application *data* as opaque, and `MemBackend` extends that to
the *memory plane*: libtracer is a **transparent byte router** ([ADR-0012] in the
repo). A backend declares its own per-architecture contract (alignment, cache
hooks, ISR-safety) and owns reclamation; the layers above see only `Segment`s. The
interface deliberately makes allocation *optional* (`alloc` may return `nullptr`)
because many substrates — MMIO, hardware FIFOs — cannot allocate at all.

| Backend | Owns | `destroy` does | Use |
| --- | --- | --- | --- |
| `mem_heap` | malloc'd bytes | frees bytes + control block | hosted targets |
| `mem_borrowed` | nothing (your bytes) | frees only the control block | live/raw, MMIO header, ROM |
| `mem_pool` | a caller slab | returns the slot to a free list | bounded / MCU / deterministic |

`mem_pool` is the bounded "custom allocator": it carves a **caller-owned** slab
into fixed slots with the free list threaded *through the slab* (no auxiliary
heap), and returns `nullptr` when full — the BACKPRESSURE signal.

## Interface

```cpp
class MemBackend {                                   // subclass this
    virtual Segment* alloc(std::size_t, std::uint32_t hint);   // nullptr if cannot
    virtual void destroy(Segment*) noexcept = 0;               // reclaim at refcount 0
    virtual void prepare_for_io (Segment*, IoDir) noexcept {}  // cache hooks (DMA)
    virtual void finalize_after_io(Segment*, IoDir) noexcept {}
    virtual std::size_t alignment() const noexcept;
    virtual std::size_t max_segment_size() const noexcept;
};

namespace mem {
    MemBackend& heap_backend();              SegmentPtr heap_alloc(std::size_t);
    SegmentPtr  borrow(std::span<std::byte>);   SegmentPtr borrow_const(std::span<const std::byte>);
    class Pool : public MemBackend {            // over a caller slab; alloc-or-null
        Pool(std::span<std::byte> slab, std::size_t slot_payload, std::size_t align = ...);
        std::size_t capacity() const;  std::size_t available() const;
    };
}
```

## The seam

```{mermaid}
classDiagram
    class MemBackend { <<interface>> +alloc() +destroy() +alignment() }
    MemBackend <|-- HeapBackend
    MemBackend <|-- BorrowedBackend
    MemBackend <|-- Pool
    MemBackend <|-- YourBackend
    Segment --> MemBackend : backend*
    note for YourBackend "bind a DMA ring,\nlwIP pbuf, MMIO, …"
```

## Benefits

- **Don't-limit-the-user** — the same protocol runs on a heap, a 4 KB MCU pool, or
  a live register; you pick the point on the spectrum.
- **Bounded by construction** — `mem_pool` makes memory use exactly the caller's
  slab; exhaustion is a return value, not an OOM.
- **Zero-copy live data** — `mem_borrowed` points a segment at bytes you already
  have (a register, a program variable) with no copy and no CRC imposed.

See: [segment](segment.md), [views](views.md), [interface map](interface-map.md).
