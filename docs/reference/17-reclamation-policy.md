# Reference 17 — Reclamation policy: when a user-code seam may be freed

> Descriptive, and about the **reference implementation's host API**, not the wire. Nothing here changes a byte on the wire or constrains a conforming implementation; it documents the grace-point model libtracer uses to answer one question its C++ API cannot dodge. The decision and its rationale are [ADR-0080](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md).

---

## The question

A subscriber hands libtracer a `{fn, ctx}` pair. It later unsubscribes. **When may it free the `ctx`?**

The question is not rhetorical, and the honest pre-ADR-0080 answer was "we cannot tell you". Delivery works like this ([02-graph-model.md](02-graph-model.md)): a write **snapshots** the source vertex's active edges under a bounded pin, releases the pin, and **dispatches outside every lock** — because a callback may re-enter the graph, publish, or block, and holding a lock across arbitrary user code is how a runtime deadlocks.

That snapshot owns what it needs to survive a concurrent unsubscribe: the target key and the routes are **refcount clones**, the link and caller names are **owning copies**. Exactly one leg is not owned — the subscriber's own `{fn, ctx}` pair, because the `ctx` is a `void*` into the *subscriber's* memory and the library has no way to clone it.

So a snapshot taken a moment before `unsubscribe()` **still invokes the callback after `unsubscribe()` returned success**. A caller that freed its context on that return freed it under a live delivery.

## Why there is no single answer

Three shapes of answer exist in the literature, and each costs something on a path libtracer refuses to spend on:

| shape | what it costs |
| --- | --- |
| refcount the pair (clone it into every snapshot) | an atomic RMW **per dispatch** |
| a per-edge in-flight counter `unsubscribe` waits on | two atomic RMW per dispatch, plus a blocking `unsubscribe` |
| a hazard-pointer scan | the shape that collapsed throughput ×16.6 at 24 threads |
| a prose contract ("quiesce your publishers first") | nothing at runtime — and it pushes the whole problem onto the application, unenforceably |

But notice what the first three have in common: **they are all paying to solve a problem that does not exist on a single-threaded node.** There, publish and `unsubscribe()` run on one thread and cannot overlap. The hazard is real only when one thread dispatches while another unsubscribes.

The bug lives in the **thread configuration**, not in the code. And libtracer already chooses its thread configuration per target. So reclamation is chosen per target too.

## The model: a grace point, closed at build time

A **grace point** is the moment at which a retired `{fn, ctx}` pair is provably named by no live snapshot, and is therefore safe to free. Each policy is a different grace point, selected the same way every other per-target knob is — a `using` in the build's configuration ([ADR-0068](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0068-build-configuration-is-plain-cxx.md)), never a runtime flag:

```cpp
// libtracer/config_override.hpp
#pragma once
namespace tr::graph {
struct my_node_config_t : default_config_t {
    using reclaim_policy_t = reclaim_strict_t;   // or leave it: the default is reclaim_local_t
};
using config_t = my_node_config_t;
}  // namespace tr::graph
```

| policy | grace point | target | dispatch cost | re-entrant `unsubscribe()` |
| --- | --- | --- | --- | --- |
| `reclaim_strict` | `unsubscribe()` returns | WIDE / MCU, non-re-entrant | **zero** | forbidden (debug-asserted) |
| **`reclaim_local`** (default) | this thread's dispatch stack unwinds to depth 0 | WIDE / MCU, re-entrant ok | one non-atomic inc/dec/branch **per publish** | supported |
| `reclaim_qsbr` | every thread has passed a quiescent state | MID / NARROW host | ~zero, deferred batched free | supported |

`reclaim_qsbr` is **specified but not yet implemented**. Until it lands, the shipped policies state their guarantee over **one thread's dispatch domain**; a node that dispatches from several threads at once and unsubscribes from another is the case neither covers.

### The rule that shapes all of them

**The library owns the tracking and signals release. The embedder never polls in-flight state, and never waits.**

That is the whole point of the model, and it is why "the callback may still be invoked until you call `collect()`" is *not* one of the options above. A contract of that shape asks the application to reason about which deliveries are in flight — which is precisely the thing it has no way to observe. So every policy delivers real reclamation, and the signal is an ordinary hook:

```cpp
void on_dead(void* ctx) { delete static_cast<my_sink_t*>(ctx); }

auto sub = graph.subscribe(path, &my_sink_t::deliver, sink);
// ...
(void)graph.unsubscribe(*sub, &on_dead);   // on_dead runs exactly once, at the grace point
```

## What each policy actually does

### `reclaim_local` — the default

The counter is a **per-thread** dispatch depth, incremented once when a fan-out begins and decremented when it ends. Two properties fall out of "per-thread" and both matter: it is non-atomic, so no cache line is ever shared; and it needs no state on the graph at all, so no `graph_t` field moves and the delivery path's code is untouched apart from the counter itself.

`unsubscribe()` then reads it and takes one of two doors:

- **depth 0** (the ordinary case — you are not inside a callback): nothing on this thread can be walking a snapshot, so the hook runs **inline** and `unsubscribe()` returns already quiescent. This is `reclaim_strict`'s guarantee, at `reclaim_strict`'s cost, for the case that dominates.
- **depth > 0** (you are inside a delivery): the pair is **parked** in per-thread storage and the hook runs when the **outermost** delivery unwinds — before the `write()` that started it returns. Not the innermost: an inner fan-out's exit leaves the outer one still walking its own snapshot.

Parking allocates nothing. It uses a bounded per-thread array (`kDeferredReleaseSlots`, default 16), and on overflow the pair is **dropped and its hook never runs** — a deliberate leak, because the alternative is releasing a context a live fan-out still names. `graph_t::deferred_release_drops()` counts every drop, so an undersized bound is visible rather than silent.

### `reclaim_strict` — the opt-in zero

For an MCU deployment that can show it never unsubscribes from inside a delivery. Nothing is tracked, nothing is stored, `unsubscribe()` always releases inline. Re-entrant unsubscribe is a **forbidden operation with a guard on it** (a debug assert), not a warning in a paragraph — but a release build cannot see the violation, which is the trade this policy is for.

### Why `reclaim_local` is the default, and not `reclaim_strict`

Because the two targets must behave **identically**. A host build tolerates re-entrant unsubscribe; if the MCU default forbade it, the *same application code* would be legal on the developer's laptop and illegal on the board — a portability bug that surfaces only on the constrained target, months later, as a corruption. `reclaim_local` buys parity for a counter that measures as free.

## What it costs — measured

Pinned on the bench host (`g++ 13.3.0`, `-O3`), best-of-rounds, against a same-run A/A null of ±0.8 % on p50:

| arm | baseline | `reclaim_local` | Δ |
| --- | ---: | ---: | ---: |
| bare notify, one trivial subscriber (batch-calibrated, fan-out 1) | 102 ns | 102 ns | **+0.00 %** |
| full write + notify + deliver, fan-out 1 | 120 ns p50 | 120 ns p50 | +0.00 % |
| deliver-only, fan-out 1 | 80 ns p50 | 80 ns p50 | +0.00 % |

The counter is bumped **once per fan-out**, never per edge, so it amortizes to nothing as fan-out widens — the wide rows are at or inside the null throughout.

Footprint, on rv32 (`-Os -fno-exceptions -fno-rtti`, GCC 15.2, real `core/src/graph.cpp`):

| arm | flash | static RAM | per-thread TLS |
| --- | ---: | ---: | ---: |
| `reclaim_strict` | +68 B | +8 B | **+0 B** |
| `reclaim_local` | +328 B | +8 B | +136 B |

Of `reclaim_local`'s flash, **+24 B lands in `fan_out`** — the dispatch path — and the rest is the `unsubscribe` overload, the release runner and the accessor, all cold. On the same target `reclaim_strict` produces a **byte-identical dispatch path**: +0 instructions in `fan_out`, only label renumbering. That is what justifies keeping it as a distinct policy rather than folding it into the default.

## The theoretical best — shard, don't reclaim

To know "no reader is using X" one must make readers announce themselves (hot-path cost), wait for a grace period (deferred free), or **never share X across threads** so the freeing thread is the only possible user. The third door is the ideal, and it is reached by architecture rather than a cleverer algorithm: **shard subscription ownership per RX thread**. A subscription only ever dispatched *and* unsubscribed by one thread is instantly quiescent on unsubscribe, and an N-core host degenerates into N independent single-threaded problems. `reclaim_qsbr` is then needed only for the genuine residual — a subscription whose one publish fans callbacks across several threads.

## See also

- [02-graph-model.md](02-graph-model.md) — the snapshot-then-dispatch fan-out this seam exists for.
- [08-views-and-ownership.md](08-views-and-ownership.md) — why the *payload* legs of a snapshot need none of this.
- [15-concurrency-and-scaling.md](15-concurrency-and-scaling.md) — the hardware regimes behind "no atomic on the dispatch path".
