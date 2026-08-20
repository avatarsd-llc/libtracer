# A shared seam needs a thread-safe backend (L0/L1 substrate)

:::{admonition} Applies to builds where `tr::mem::kSpinWaitSafe` is true
:class: important

This is the one **build-configuration-dependent** example in the wire and view domains. A
single-core priority-preemptive target sets `tr::mem::kSpinWaitSafe = false` in
`libtracer/config_override.hpp`; there a spinner that outranks the lock holder never yields the
CPU the holder needs, so binding `spin_sync_t` is not "slower" — it is a hang, and
`synchronized_pool_t` refuses the instantiation outright. The target is still always built and
always linked: it follows the binding with `if constexpr`, prints
`skipped: this build sets tr::mem::kSpinWaitSafe = false …` and passes.

**An example a binding does not apply to must skip at run time, never fail to compile.** The
mechanics differ from [`sub_unsubscribe_from_dispatch`](sub-unsubscribe-from-dispatch.md), and
the difference is a two-layer trap worth stating outright. First, `if constexpr` alone is not
enough: in a **non-template** function the discarded branch is still fully instantiated, so
declaring the pool inside a discarded branch of `main` would trip the assert regardless.
Second — and this is the one that is easy to miss — **being a template is not enough either**:
a template's *non-dependent* constructs are checked at definition time, so spelling the
concrete `tr::mem::sync_pool_t` inside the discarded branch trips it too. The pool type has to
**depend on a template parameter**, which is why the helper takes the sync policy as one. Line
1 of a run prints the bound value, so the output says which arm it took. Every other example in
these two domains is unconditional.
:::

A segment self-routes its reclaim on whatever thread drops the last reference — typically a
subscriber or a transport receive thread, concurrent with a writer's `alloc`. So **any**
`mem_backend_t` injected at a shared seam must tolerate that
([ADR-0060](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0060-lkv-copy-store-injected-value-backend.md)
§2): a `graph_t`'s value backend, a router's flat, a transport vertex's rx backend.
`tr::mem::synchronized_pool_t` is the bounded answer — one [`pool_t`](view-pool-backend.md)
whose O(1) free-list operations run inside a critical section.

## What to notice

- **The mechanism is a compile-time policy, because only the target knows its concurrency
  model.** A multi-core host wants the `spin_sync_t` spinlock: negligible contention on an O(1)
  section, and it avoids the ~2 µs OS-mutex round trip that would dominate a ~120 ns free-list
  operation. A single-core MCU wants an interrupt-disable critical section instead. The choice
  is a template argument — no branch, no vtable, no per-alloc indirection.
- **`tr::mem::sync_pool_t` is the host spelling**, and it is the *discoverable* short name —
  which is why a build that forbids spin-waiting rejects it loudly rather than shipping a hang.
- **A single thread-safe pool, never per-stripe sharding.** Sharding removes no race and adds
  partition imbalance.
- **ISR-safety and non-blocking are different facts.** A spinlock is `is_nonblocking` but not
  `is_isr_safe`, and the example asserts the latter — the traits forward the *policy's*
  guarantees rather than inventing them.
- **It is opt-in construction only.** No seam defaults to it; `heap_backend()` remains the
  default everywhere.

## Source

```{literalinclude} /core/examples/view_sync_pool.cpp
:language: cpp
:linenos:
```

See also: [backends](../modules/backends.md) · [configuration](../modules/config.md) ·
[concurrency & scaling reference](../reference/15-concurrency-and-scaling.md).
