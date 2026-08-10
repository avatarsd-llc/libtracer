# backends — the allocator seam (L0)

```{admonition} In one paragraph
:class: tip
**`tr::mem::mem_backend_t`** is a small, user-implementable interface: subclass it
to bind libtracer to *any* memory — a heap, a fixed arena, live registers, a DMA
ring. libtracer never allocates *payload bytes* on its own; it asks a backend. (Its own
bookkeeping — a rope's spilled link chain, the CAN splitter's window vector, the
route-handle label tables — allocates from the global heap or from an injected
`std::pmr::memory_resource`; those seams are
[failable allocation and backpressure](../design/allocation-and-backpressure.md).) Three backends are
provided: **`mem_heap`** (owns malloc'd bytes), **`mem_borrowed`** (wraps your
bytes, frees nothing), and **`mem_pool`** (a bounded fixed-slab,
`alloc`-or-`null`).
```

## What it does

The protocol treats application *data* as opaque, and `mem_backend_t` extends that
to the *memory plane*: libtracer is a **transparent byte router**
([ADR-0012 — memory binding is a modular spectrum](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0012-modular-memory-binding-transparent-router.md)).
A backend declares its own per-architecture contract (alignment, cache
hooks, ISR-safety) and owns reclamation; the layers above see only `segment_t`s. The
interface deliberately makes allocation *optional* (`alloc` may return `nullptr`)
because many substrates — MMIO, hardware FIFOs — cannot allocate at all.

| Backend | Owns | `destroy` does | Use |
| --- | --- | --- | --- |
| `mem_heap` | malloc'd bytes | frees bytes + control block | hosted targets |
| `mem_borrowed` | nothing (your bytes) | frees only the control block | live/raw, MMIO header, ROM |
| `mem_pool` | a caller slab | returns the slot to a free list | bounded / MCU / deterministic |

A fourth, `mem_cuda`, is compiled only when `LIBTRACER_WITH_CUDA` is set
(`core/CMakeLists.txt:289-294`); it needs the CUDA toolkit and is not built in CI.

`mem_pool` is the bounded "custom allocator": it carves a **caller-owned** slab
into fixed slots with the free list threaded *through the slab* (no auxiliary
heap), and returns `nullptr` when full — the BACKPRESSURE signal. `pool_t` is not
synchronized; `synchronized_pool_t<Sync>` (`core/include/libtracer/mem_pool.hpp:172`)
composes over it and guards the free list with a **compile-time synchronisation policy**,
which is what any shared seam needs — a segment self-routes its reclaim on whatever thread
drops the last reference, concurrent with a writer's `alloc`. Two policies ship: the
spinlock `spin_sync_t` for a multi-core host (`sync_pool_t` is the alias for that pairing)
and the interrupt-disable `tr::esp::portmux_sync_t` for a single-core, priority-preemptive
MCU (`integrations/esp-idf/libtracer/include/libtracer_esp/critical_pool.hpp`, aliased
`tr::esp::critical_pool_t` — it needs FreeRTOS headers, so it ships with the ESP-IDF
component rather than in `core/`). The target picks; nothing defaults to either.

Each concrete backend also carries three compile-time traits the module set reads
without a virtual call — `needs_cache_ops`, `is_isr_safe`, `owns_bytes`
([ADR-0047 — build-time closed module sets](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md) §2).
`owns_bytes` is the one a caller must respect: `mem_borrowed` sets it `false`, so a
segment it produced must not be stored durably.

The seam lives at L0 (`tr::mem`); the segments it produces are owned at L1
(`tr::view`). A backend constructs and reclaims `tr::view::segment_t` — the one
sanctioned L0↔L1 boundary type, and the only `tr::view` symbol the L0 interface is
permitted to name
([ADR-0016 — substrate, zero-copy, layer namespaces](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) §2).
`alloc` returns a **raw** `segment_t*` with a refcount of 1; the caller adopts it
with `tr::view::segment_ptr_t::adopt` (`core/include/libtracer/segment.hpp:116`).
The handle-producing conveniences `heap_alloc` / `borrow` / `borrow_const`
therefore live in `tr::view`, not here.

## Interface

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

### The failable-block seam — `block_source_t`

The second L0 seam, and a distinct one
([ADR-0065 — failable allocation gets its own seam](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md),
[reference/09](../reference/09-memory-substrate.md)). `mem_backend_t` above vends
refcounted `segment`s for payload bytes; this one vends **raw single-owner blocks**
and reports exhaustion **by value**, because `std::pmr::memory_resource` structurally
cannot — its `allocate` signals failure only by throwing, and on a `-fno-exceptions`
target that lowers to the toolchain's `abort()` stub, which a peer can provoke.

The policy the seam exists to serve — which allocations a peer can reach, which
status each exhaustion answers with, and how to size a bounded source — is described
in [failable allocation and backpressure](../design/allocation-and-backpressure.md).
This page documents only the API.

```{doxygenclass} tr::mem::block_source_t
:members:
:undoc-members:
```

```{doxygenclass} tr::mem::heap_source_t
:members:
```

```{doxygenclass} tr::mem::null_source_t
:members:
```

```{doxygenclass} tr::mem::bump_source_t
:members:
```

```{doxygenclass} tr::mem::pool_source_t
:members:
```

```{doxygenstruct} tr::mem::size_class_t
:members:
```

```{doxygenstruct} tr::mem::sync_none_t
:members:
```

```{doxygenclass} tr::mem::block_array_t
:members:
```

The bounded reference backend:

```{doxygenclass} tr::mem::pool_t
:project: libtracer
:members:
```

### The heap backend and the space tags

```{doxygenclass} tr::mem::heap_backend_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::mem::heap_backend
:project: libtracer
```

```{doxygenenum} tr::mem::mem_space_t
:project: libtracer
```

```{doxygenenum} tr::mem::backend_tag
:project: libtracer
```

```{doxygenfunction} tr::mem::destroy_dispatch
:project: libtracer
```

```{doxygenfunction} tr::mem::transfer
:project: libtracer
```

### Shared pools and their synchronization policy

A pool shared by more than one thread needs a synchronization policy, and the
policy is a compile-time parameter rather than a runtime flag so a single-threaded
target pays nothing for it. `spin_sync_t` is the multi-core host policy;
`sync_none_t` is the unsynchronized one; a bare-metal target supplies an
interrupt-disable critical section of its own. `sync_pool_t` is the spelling for
the common host case.

The `pool_source_t` seam has its own policy question, and one deliberate
non-answer: `sync_mutex_t` lives in a separate header because the L0 seam is
compiled into a freestanding footprint sentinel where `<mutex>` does not exist,
and because a mutex is the right instrument only for a source shared at *wiring*
frequency. It is not a way to make a per-frame source thread-safe — see
[failable allocation and backpressure](../design/allocation-and-backpressure.md)
for what a shared free list costs under contention.

```{doxygenconcept} tr::mem::pool_sync_policy
:project: libtracer
```

```{doxygenstruct} tr::mem::spin_sync_t
:project: libtracer
:members:
```

```{doxygenclass} tr::mem::synchronized_pool_t
:project: libtracer
:members:
```

```{doxygentypedef} tr::mem::sync_pool_t
:project: libtracer
```

```{doxygenclass} tr::mem::sync_mutex_t
:project: libtracer
:members:
```

### Device memory

```{doxygenfunction} tr::mem::cuda_backend
:project: libtracer
```

```{doxygenfunction} tr::mem::cuda_transfer
:project: libtracer
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

## Consequences

- The same protocol runs against a heap, a caller-sized MCU slab or a live register,
  because the substrate is selected by binding a backend rather than by a build
  variant of the core.
- Memory use with `mem_pool` is exactly the caller's slab: the free list is threaded
  through the slab, so there is no auxiliary heap allocation, and exhaustion is an
  `alloc` returning `nullptr` rather than an OOM.
- `mem_borrowed` puts a segment over bytes the caller already holds, so live data
  reaches the wire with no copy and no CRC imposed; the cost is that those bytes are
  outside libtracer's lifetime control.
- A substrate that cannot allocate at all is still bindable, because `alloc` is
  permitted to return `nullptr` unconditionally — MMIO windows and hardware FIFOs
  bind as read-only borrowed segments.
- Two seams rather than one means two failure contracts to hold in mind: a
  `mem_backend_t` failure is a refcounted-segment allocation that failed, a
  `block_source_t` failure is a single-owner block that failed. Neither throws.

## Pitfalls

- **A raw `segment_t*` that is never adopted leaks.** `alloc` hands back a pointer at
  refcount 1 and the backend does not track it; the value is only safe once
  `segment_ptr_t::adopt` owns it. The `tr::view` helpers (`heap_alloc`, `borrow`,
  `borrow_const`) exist so that the common paths cannot get this wrong.
- **Borrowed bytes must outlive every segment over them.** `borrowed_backend_t::destroy`
  deletes the control block and nothing else (`core/include/libtracer/mem_borrowed.hpp:39`),
  so a borrow over a stack buffer or a scratch frame becomes a dangling read the moment
  that storage goes away. Durable storage of a value wants an owning backend.
- **`bump_source_t` is scope-lifetime only.** Blocks carved from its buffer are never
  individually reclaimed, so a bump source wired as a long-lived seam fills
  monotonically and then refuses everything. Construct it per operation, or `reset` it
  between operations; a long-lived bounded seam wants `pool_source_t`, which recycles.
- **A `bump_source_t` buffer is not a hard bound by default.** Its upstream defaults to
  `heap_source()`, so overflow spills to the platform heap. Passing `null_source()` as
  the upstream is what makes the buffer the limit and turns overflow into a rejection.
- **`block_source_t::release` is sized.** The `bytes` and `align` passed to `release`
  must match the originating `try_alloc` call — that is what lets a bump or pool source
  carry no per-block header. A mismatched pair corrupts the source's accounting rather
  than failing loudly.

See: [segment](segment.md), [views](views.md), [interface map](interface-map.md),
[failable allocation and backpressure](../design/allocation-and-backpressure.md).
