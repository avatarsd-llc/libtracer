# A bounded backend: exhaustion by value (L0/L1 substrate)

`tr::mem::pool_t` carves a **caller-owned** slab into equal slots, threading its free list
through the slab itself. There is no auxiliary heap allocation, so total memory use is exactly
the array the caller declared — and running out is `nullptr`, not a throw and not an `abort()`
([reference 09](../reference/09-memory-substrate.md) §pressure). That is what makes it the
deterministic MCU choice: a node's memory ceiling is a `std::array` a reader can point at.

## What to notice

- **Two refusals, two meanings, and the API keeps them apart.** An oversize request is
  **permanent** — `max_segment_size()` says what a slot can hold and no retry changes it. An
  exhausted pool is **transient backpressure**, which is why `rope_t::try_flatten` answers
  `flatten_err_t::NO_MEMORY` there and the very same rope flattens once a slot comes back. The
  example drains the pool, watches the refusal, returns one slot, and retries successfully.
- **Conflating them is a real defect, not a style point.** Before
  [#917](https://github.com/avatarsd-llc/libtracer/issues/917) both collapsed into an empty
  view — indistinguishable from each other *and* from a legitimately empty rope — so a router
  reported a local OOM to a peer as a permanent malformed-frame error. The other refusal,
  `NOT_HOST`, is on [the device rope page](view-device-rope.md).
- **A slot is held by the *last* reference, not the first.** The example keeps its handles in a
  vector; a slot comes back only when every view naming it is gone — the
  [refcount](view-segment-refcount.md) rule, seen from the allocator's side.
- **This pool is not internally synchronized.** Each backend declares its own concurrency
  contract, and this one's is "single-threaded reclamation". A shared seam wants
  [`sync_pool_t`](view-sync-pool.md).
- **Nothing here is conditional** — the target builds and runs under every CI leg.

## Source

```{literalinclude} /core/examples/view_pool_backend.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) · [views module](../modules/views.md) ·
[memory substrate reference](../reference/09-memory-substrate.md).
