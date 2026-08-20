# The `std::pmr` adapter: placement, not failability (L0 substrate)

`tr::mem::source_resource_t` is a `std::pmr::memory_resource` holding one `block_source_t*`:
`do_allocate` forwards to `try_alloc`, `do_deallocate` forwards to the seam's **sized**
`release` — `std::pmr` carries the original size and alignment into `deallocate`, which is
exactly the pair a header-free pool needs — and `do_is_equal` is address identity
([ADR-0079](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md)).

:::{warning}
**This delivers placement and bounding, not failability.** `std::pmr`'s only exhaustion signal is
a throw, so the adapter's boundary is a `std::bad_alloc` on a hosted build and a `std::abort()`
under `-fno-exceptions` — byte-for-byte what libstdc++ produces for the same throw on that
profile. A **peer-provoked** store does not become safe by moving onto a `std::pmr` container
over a bounded slab; it migrates onto [`block_array_t`](mem-block-array.md) and fails by value.
The example deliberately never provokes that boundary, because there is nothing to demonstrate
there but the abort.
:::

## What to notice

- **Reach for it in exactly one case:** a `std::pmr` container whose element type is neither
  trivially copyable nor trivially destructible, so `block_array_t`'s two static assertions
  reject it and the "invert the ownership at the public type" rule cannot be applied to a type
  the store does not own. `tr::net::can_reassembly_t` — whose slice map holds a refcounted
  `tr::view::view_t` — is the shipped example, which is why the adapter was owed to
  [#873](https://github.com/avatarsd-llc/libtracer/issues/873)'s family 5 rather than built
  speculatively.
- **What you get is worth having anyway.** The container's bytes come from the deployer's slab
  instead of the global heap, and the slab's size *is* the bound. The example checks the
  `std::pmr::vector`'s element pointer lands inside the caller's array and that the pool counted
  the bytes.
- **`std::pmr`'s sized `deallocate` maps 1:1 onto the seam's sized `release`**, so a
  [`pool_source_t`](mem-pool-source.md) recycles these blocks with no per-block header — the
  example destroys the vector and finds the freed block is the very next one the adapter hands
  out.
- **It runs one direction, and the reverse must never be added.** Wrapping a
  `std::pmr::memory_resource` so it could be used *as* a `block_source_t` would have to answer
  `nullptr` from an `allocate` annotated `returns_nonnull` that signals only by throwing — the
  exact defect the block seam exists to escape, and `core/tests/mem_source_pmr_test.cpp` asserts
  the two types stay non-interconvertible.
- **Two adapters over the *same* source compare unequal.** `do_is_equal` is address identity
  rather than a `dynamic_cast` (the reference node ships `-fno-rtti`), so containers built over
  two adapters will copy rather than steal storage on a move-assign. Construct **one adapter per
  source** and pass it around.
- **One source per receiver still applies.** Wrapping a shared `pool_source_t` in a
  `memory_resource` inherits its ~15× multi-thread collapse unchanged; the adapter adds no shared
  state of its own and fixes nothing.
- **It costs nothing to a target that does not name it.** No library translation unit includes
  the header and it is absent from the `tracer.hpp` umbrella, so a build that never mentions
  `source_resource_t` produces byte-identical objects.
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/mem_source_resource.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) ·
[memory substrate reference](../reference/09-memory-substrate.md) ·
[a container that fails by value](mem-block-array.md) ·
[a long-lived seam has to recycle](mem-pool-source.md).
