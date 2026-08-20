# segment — refcounted bytes (L0↔L1)

```{admonition} In one paragraph
:class: tip
A **`tr::view::segment_t`** is real bytes owned by a backend, plus an intrusive
atomic refcount — the boundary object where L0's bytes acquire L1's ownership.
The **`tr::view::segment_ptr_t`** handle threads that one buffer's lifetime
through fan-out: copying a handle is a relaxed increment (a *clone*), dropping
the last one reclaims the bytes through the backend. This is what lets many
views share one buffer with no copies, and what makes a decoded TLV safe to hold
past the receive call.
```

## What it does

L0 is "real bytes in real memory owned by some real allocator"; L1 adds the
*ownership*. `segment_t` is the control block over one such buffer and the single
sanctioned object on that boundary — L0 backends vend it, L1 views hold it, and
no other type crosses. `segment_ptr_t` is the owning handle. The refcount lives
**inside** the segment (not in a side `shared_ptr` block) so a static MMIO
descriptor, a pool slot, and a heap allocation all carry their own count.

A segment is never copied or moved; it is always handled through
`segment_ptr_t`, and it caches its backend's address space and module-set tag at
construction (`segment_t`, `core/include/libtracer/segment.hpp`). When the last
handle drops, `segment_ptr_t::reset` calls `tr::mem::destroy_dispatch`, which
switches on that tag to a direct call for a linked backend and falls back to the
backend's virtual `destroy` for any other — the result is identical to
`seg->backend->destroy(seg)` for every backend
(`core/include/libtracer/backend.hpp:251-260`;
[ADR-0047 — build-time-closed module sets, compile-time seams](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md) §2).
There is no separate `release()` step.

The atomic orderings are the canonical intrusive_ptr pattern, specified once in
[reference/02](../reference/02-graph-model.md) §required atomic operations:
increment `relaxed` (`core/include/libtracer/segment.hpp:52` — the caller already
holds a reference, so the data dependency travels through it), decrement
`acq_rel` (`:53-55` — release the writes before another thread observes the count
drop, acquire on observing the drop to zero), inspect `acquire` (`:56-58`). The
decrement returns the value *before* it, so a return of `1` identifies the caller
that dropped the last reference.

`LIBTRACER_NO_ATOMIC` replaces the atomic with a plain `uint_least32_t` for
single-threaded and Cortex-M0/M0+ targets that have no LDREX/STREX
(`core/include/libtracer/segment.hpp:21,44`). It is a compile definition, not a
CMake option: the constrained-target footprint build sets it
(`tools/cortexm0_footprint.py:158`) and the substrate test is built a second time
with it (`core/tests/CMakeLists.txt:1608,1623-1624`).

## API reference

```{doxygenstruct} tr::view::segment_t
:project: libtracer
:members:
```

```{doxygenclass} tr::view::segment_ptr_t
:project: libtracer
:members:
```

The handle-producing conveniences live in `tr::view` rather than with the
backends, because what they produce is an L1 handle:

```{doxygenfunction} tr::view::heap_alloc
:project: libtracer
```

```{doxygenfunction} tr::view::borrow
:project: libtracer
```

```{doxygenfunction} tr::view::borrow_const
:project: libtracer
```

```{doxygenfunction} tr::view::borrow_device
:project: libtracer
```

```{doxygenfunction} tr::view::cuda_alloc
:project: libtracer
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
    TX->>V: drop producer ref (acq_rel -- → 2)
    S1->>V: release (acq_rel -- → 1)
    S2->>V: release (acq_rel -- → 0)
    V->>B: destroy_dispatch(seg) — bytes reclaimed
```

## Consequences

- **Zero-copy fan-out** — N subscribers share one buffer; delivery is N relaxed
  increments, no `memcpy`.
- **A decoded TLV outlives its receive call** — a `tlv_t` borrows segment bytes
  via spans; the `segment_ptr_t` keeps them alive exactly as long as some view
  needs them, which is what makes borrowed (zero-copy) decode safe at all.
- **No hidden allocation** — the count is in the segment, so MMIO, pool and
  borrowed segments need no separate control block.
- **Reclaim is devirtualizable** — the cached module-set tag turns per-release
  reclaim into a `switch`, foldable to one direct call on a target that links a
  single backend.
- **Portable to cores without atomics** — `LIBTRACER_NO_ATOMIC` drops to a plain
  counter where the application guarantees no cross-thread sharing.

## Pitfalls

- **`adopt` and `retain` are not interchangeable.** `adopt` takes over an
  existing reference without bumping — the shape `mem_backend_t::alloc` returns
  (a raw `segment_t*` at refcount 1); `retain` adds a new reference to an
  already-live segment (`segment.hpp:116,120`). Adopting a segment twice
  double-frees it; retaining an `alloc` result leaks it, because the reference
  `alloc` already created is never dropped.
- **`use_count` is not a synchronization primitive.** It is an acquire load for
  debug and metrics (`segment.hpp:154-157`). A count of 1 does not mean no other
  thread is about to clone the handle, and branching on it reintroduces the race
  the refcount exists to remove.
- **`LIBTRACER_NO_ATOMIC` is an application promise, not a portability switch.**
  With a plain counter, one cross-thread clone or release races the count and
  corrupts the lifetime silently. Set it only where the application serializes
  all access to segments.
- **`bytes` is writable at the type level; legality is the backend's contract.**
  A borrow over ROM or a caller's `const` buffer hands out a mutable
  `std::span<std::byte>` all the same (`segment.hpp:73-76,81`); writing through
  it is undefined even though it compiles.
- **A `DEVICE` segment must not be CPU-dereferenced.** The span looks ordinary,
  but `space` records that the bytes are not CPU-addressable
  (`segment.hpp:82`, `backend.hpp:57-68`); such a segment may back only an opaque
  VALUE payload, with header and trailer kept in `HOST` segments
  ([ADR-0024 — mem_cuda GPU backend, heterogeneous rope](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)).

See: [backends](backends.md) (who creates segments), [views](views.md) (who holds
them), and the [interface map](interface-map.md).
