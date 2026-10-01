# A shared seam needs a thread-safe backend (L0/L1 substrate)

A segment self-routes its reclaim on whatever thread drops the last reference — typically a
subscriber or a transport receive thread, concurrent with a writer's `alloc`. So **any**
`mem_backend_t` injected at a shared seam must tolerate that
([ADR-0060](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md)
§2): a `graph_t`'s value backend, a router's flat, a transport vertex's rx backend.
`tr::mem::synchronized_pool_t` is the bounded answer — one [`pool_t`](view-pool-backend.md)
whose O(1) free-list operations run inside a critical section.

## What to notice

- **The mechanism is a compile-time guard, because only the target knows its concurrency
  model.** Since RFC-0028 slice 10 the pool's guard is the same `tr::guard` trait the
  last-known-value slot uses, and `synchronized_pool_t<>` binds the build's one
  `tr::graph::guard_t`: on a host the `tr::mutex_guard_t` (a short bounded spin, then a
  nap — never a pure spin, so it cannot hang a priority-preemptive scheduler), on ESP-IDF the
  interrupt-masked `critical_guard_t` (`tr::esp::critical_pool_t`). The choice is a template
  argument — no branch, no vtable, no per-alloc indirection.
- **A guard that may pure-spin is refused where spinning is unsafe.** A build that sets
  `tr::mem::kSpinWaitSafe = false` rejects any guard declaring `may_spin = true` at the
  instantiation, so a spinner that outranks the lock holder is a compile error, not a hang.
- **A single thread-safe pool, never per-stripe sharding.** Sharding removes no race and adds
  partition imbalance.
- **ISR-safety and non-blocking are different facts.** The pool forwards its guard's
  `is_isr_safe` / `is_nonblocking` rather than inventing them, and the example checks that.
- **It is opt-in construction only.** No seam defaults to it; `heap_backend()` remains the
  default everywhere.

## Source

```{literalinclude} /core/examples/view_sync_pool.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) · [configuration](../modules/config.md) ·
[concurrency & scaling reference](../reference/15-concurrency-and-scaling.md).
