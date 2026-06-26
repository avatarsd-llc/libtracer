# backends — the allocator seam (L0)

```{admonition} In one paragraph
:class: tip
**`tr::mem::mem_backend_t`** is a small, user-implementable interface: subclass it
to bind libtracer to *any* memory — a heap, a fixed arena, live registers, a DMA
ring. libtracer never allocates on its own; it asks a backend. Three ship today:
**`mem_heap`** (owns malloc'd bytes), **`mem_borrowed`** (wraps your bytes, frees
nothing), and **`mem_pool`** (a bounded fixed-slab, `alloc`-or-`null`).
```

## What it does

The protocol treats application *data* as opaque, and `mem_backend_t` extends that
to the *memory plane*: libtracer is a **transparent byte router** ([ADR-0012] in the
repo). A backend declares its own per-architecture contract (alignment, cache
hooks, ISR-safety) and owns reclamation; the layers above see only `segment_t`s. The
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

The seam lives at L0 (`tr::mem`); the segments it produces are owned at L1
(`tr::view`). A backend constructs and reclaims `tr::view::segment_t` (the one
sanctioned L0↔L1 boundary type, [ADR-0016] §2) and `alloc` returns a **raw**
`segment_t*` — the caller adopts it with `tr::view::segment_ptr_t::adopt`. The
handle-producing conveniences `heap_alloc` / `borrow` / `borrow_const` therefore
live in `tr::view`, not here.

## Interface

> Generated from the `core/` headers by Doxygen — these are the reference
> implementation's own declarations, not a hand-maintained copy.

```{doxygenclass} tr::mem::mem_backend_t
:project: libtracer
:members:
```

The DMA/allocation enums the seam uses:

```{doxygenenum} tr::mem::io_dir_t
:project: libtracer
```

```{doxygenenum} tr::mem::alloc_hint_t
:project: libtracer
```

The bounded reference backend:

```{doxygenclass} tr::mem::pool_t
:project: libtracer
:members:
```

## The seam

```{mermaid}
classDiagram
    class mem_backend_t { <<interface>> +alloc() +destroy() +before_io() +after_io() +alignment() }
    mem_backend_t <|-- heap_backend_t
    mem_backend_t <|-- borrowed_backend_t
    mem_backend_t <|-- pool_t
    mem_backend_t <|-- YourBackend
    segment_t --> mem_backend_t : backend*
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
