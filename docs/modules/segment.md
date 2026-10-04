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
(`core/include/libtracer/backend.hpp:destroy_dispatch`;
[ADR-0047 — build-time-closed module sets, compile-time seams](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md) §2).
There is no separate `release()` step.

The atomic orderings are the canonical intrusive_ptr pattern, specified once in
[reference/02](../reference/02-graph-model.md) §required atomic operations:
increment `relaxed` (`core/include/libtracer/segment.hpp:count_.fetch_add(1, std::memory_order_relaxed)` — the caller already
holds a reference, so the data dependency travels through it), decrement
`acq_rel` (`core/include/libtracer/segment.hpp:return count_.fetch_sub(1, std::memory_order_acq_rel)` — release the writes before another thread observes the count
drop, acquire on observing the drop to zero), inspect `acquire` (`core/include/libtracer/segment.hpp:return count_.load(std::memory_order_acquire)`). The
decrement returns the value *before* it, so a return of `1` identifies the caller
that dropped the last reference.

On a core with no atomic read-modify-write — Cortex-M0/M0+ (no LDREX/STREX), rv32imc —
the count takes its *guarded* binding instead: a load and a store inside one section of
the build's guard, `config_t::guard_t` (`core/include/libtracer/segment.hpp:inline constexpr bool kNativeRefCount`,
`core/include/libtracer/segment.hpp:class basic_ref_count_t`). The choice is made from the target, the same
way `tr::rmw_counter_t` makes it for a vertex's write sequence; nothing is defined on the
command line. A single-threaded node makes the guard free by binding `tr::no_guard_t` in
its `libtracer/config_override.hpp`, which is what the footprint sentinels do
(`core/tests/footprint/config/libtracer/config_override.hpp`). On a host the substrate test
drives the guarded binding by naming it, under several threads
(`core/tests/substrate_test.cpp:test_refcount_bindings_under_threads`). The old
`LIBTRACER_NO_ATOMIC` macro was removed in #1722 and is refused at compile time.

## API reference

```{doxygenstruct} tr::view::segment_t
:project: libtracer
:members:
```

```{doxygenclass} tr::view::segment_ptr_t
:project: libtracer
:members:
```

The ingress-loan reserve (RFC-0028 §6.9): a receive block at or above the share
threshold carries this many bytes in front of the frame, and the value stored from
that frame is placed in them. `view::alloc_rx` (below, with the other
handle-producing conveniences) sets the segment's `rx_loan` bit.

```{doxygenvariable} tr::view::kRxLoanBytes
:project: libtracer
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
- **Portable to cores without atomics** — the guarded binding counts under the build's
  guard, which a single-threaded node binds to `tr::no_guard_t` for a plain counter.

## Pitfalls

- **`adopt` and `retain` are not interchangeable.** `adopt` takes over an
  existing reference without bumping — the shape `mem_backend_t::alloc` returns
  (a raw `segment_t*` at refcount 1); `retain` adds a new reference to an
  already-live segment (`segment.hpp:segment_ptr_t::adopt`, `segment.hpp:segment_ptr_t::retain`). Adopting a segment twice
  double-frees it; retaining an `alloc` result leaks it, because the reference
  `alloc` already created is never dropped.
- **`use_count` is not a synchronization primitive.** It is an acquire load for
  debug and metrics (`segment.hpp:segment_ptr_t::use_count`). A count of 1 does not mean no other
  thread is about to clone the handle, and branching on it reintroduces the race
  the refcount exists to remove.
- **`tr::no_guard_t` is an application promise, not a portability switch.**
  With it bound, the guarded refcount is a plain load and store, and one cross-thread
  clone or release races the count and corrupts the lifetime silently. Bind it only
  where the application serializes all access to libtracer state.
- **`bytes` is writable at the type level; legality is the backend's contract.**
  A borrow over ROM or a caller's `const` buffer hands out a mutable
  `std::span<std::byte>` all the same (`segment.hpp:segment_t`, `segment.hpp:segment_t::bytes`); writing through
  it is undefined even though it compiles.
- **A `DEVICE` segment must not be CPU-dereferenced.** The span looks ordinary,
  but `space` records that the bytes are not CPU-addressable
  (`segment.hpp:segment_t::space`, `backend.hpp:mem_space_t`); such a segment may back only an opaque
  VALUE payload, with header and trailer kept in `HOST` segments
  ([ADR-0024 — mem_cuda GPU backend, heterogeneous rope](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0024-mem-cuda-gpu-backend-heterogeneous-rope.md)).

See: [backends](backends.md) (who creates segments), [views](views.md) (who holds
them), and the [interface map](interface-map.md).
