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

All three are shipped. The first two state their guarantee over **one thread's dispatch domain**, which is what the WIDE / MCU target needs and all it needs; `reclaim_qsbr` is the one to bind when a node dispatches from several threads at once and may unsubscribe from another — the case neither of the others covers.

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

### `reclaim_qsbr` — the cross-thread grace period

The other two policies decide by asking **this thread** a question. That is exactly what fails when thread A is mid-delivery and thread B unsubscribes: B's own dispatch depth is 0, so a per-thread grace point concludes "nothing is in flight" and frees a context A is still holding. `reclaim_qsbr` replaces the question with one no single thread can answer alone.

It is quiescent-state reclamation in the URCU sense, and the announcement it needs was **already being computed**. The dispatch bracket the other policies use maintains a per-thread depth whose transition to 0 is precisely the proof that this thread holds no snapshot. `reclaim_qsbr` reuses it verbatim and adds, at the *outermost* bracket only:

- **entry**: one relaxed load of a global epoch — a read-mostly line, bumped only on the control plane — and one store into this thread's own cache-line-isolated cell, marking it online at that epoch. **No atomic read-modify-write.**
- **exit**: one store of `0` to the same cell. Zero means "holds nothing", which is what makes a thread idling in `epoll` non-blocking — the classic QSBR liveness hole, closed structurally rather than by a `thread_offline()` verb the embedder must remember to call.

`unsubscribe()` then bumps the epoch once and scans the participant table. A pair retired at epoch `E` is free once every participant is either offline or online at an epoch greater than `E`. **The scan is on the reclaim path only** — that is the precise difference from the hazard-pointer shape in the table above, which put its scan on the *read* path and is why that one collapsed throughput.

Two consequences the API states rather than hides:

- **The retired pairs live in one shared table, not per-thread.** They have to. The case this policy exists for is B retiring while A dispatches; if B parked the pair in B's own storage nothing would ever drain it, because B is not dispatching and "unsubscribe once, then never touch the graph again" is the dominant teardown shape.
- **So the hook may run on a thread other than the `unsubscribe()` caller** — whichever participant's quiescence completed the grace period. Both other policies promise the caller's own thread; a grace period that spans threads structurally cannot, because the only alternatives are to block the caller (which the rule below forbids) or to leak. A release hook under this policy must therefore be thread-safe with respect to its own context. This is the reason `reclaim_qsbr` is opt-in and not the default.

When the same build also binds `hazard_slot_t`, each thread's quiescent point additionally self-drains its **own** private retired LKV list. That is the half of [#897](https://github.com/avatarsd-llc/libtracer/issues/897) ADR-0080 assigns to this policy — `~hazard_slot_t` never has to reach across a live thread's private list — and it costs `lkv_slot.hpp` no code at all: the quiescent point calls the already-shipped `retire_and_flush`, whose own early-out makes it free on a thread that parked nothing. **No `store()`-path atomic is added**, and that is a `cmp` rather than a sentence: every out-of-line `hazard_slot_t` entity is byte-identical between a `reclaim_local` and a `reclaim_qsbr` build.

`graph_t::thread_quiescent()` exists for the thread the automatic path cannot reach — one that displaces LKV nodes without ever dispatching, and the stated teardown precondition before an injected `std::pmr` resource dies. **No policy's guarantee depends on an embedder calling it**; every dispatching thread announces and drains automatically, and under the other two policies it is an empty inline function.

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

For `reclaim_qsbr` the primary instrument is the same one, and the structural half carries the claim (a policy that is off by default must be shown to cost nothing when it is off):

| statement | instrument | result |
| --- | --- | --- |
| the default build pays nothing for this policy existing | object-file `cmp`, before vs after, same build dir | 50 / 51 objects byte-identical; the 51st has identical `.text`, `.bss` and `.tbss` **sizes** and every function's instruction stream identical |
| `fan_out` is untouched in the default build | `objdump` instruction diff | **0** instructions changed |
| the QSBR domain is not emitted where it is not bound | `nm` | **0** `detail_qsbr` symbols; its `.bss` is 0 B |
| only the policy's own translation unit changes when it IS bound | object-file `cmp`, `reclaim_local` vs `reclaim_qsbr` | 49 / 51 identical — the whole wire codec, every transport, the router, the memory substrate and the path layer provably untouched |
| the cost is per fan-out, not per edge | `objdump` of the dispatch loop | the per-edge loop body is **instruction-identical**; +36 instructions total, all in the entry bracket and the exit epilogues |
| a publish nobody subscribed to pays nothing | `objdump` | the announce sits **below** the no-subscriber gate; the first 13 instructions are identical |

The per-edge identity is the load-bearing one: it is what makes the cost independent of subscriber count, which is what ADR-0080 requires of any policy on this path.

The timing half, same commit and same directory, differing only by the override fragment — `taskset -c 2` on both arms, arms interleaved round-robin in one session, first execution discarded, **best-of-rounds** with `[min..max]` beside each figure, against a same-session A/A null of **±1.0 %** on the `inproc-batch` rows:

| `inproc-batch` row | `reclaim_local` | `reclaim_qsbr` | Δ |
| --- | ---: | ---: | ---: |
| fan-out 1 (bare notify, one trivial subscriber) | 101 ns | 106 ns | **+4.95 %** |
| fan-out 8 | 227 ns | 230 ns | +1.32 % |
| fan-out 16 | 310 ns | 302 ns | −2.58 % |
| fan-out 64 | 1,002 ns | 952 ns | −4.99 % |
| fan-out 1024 | 14,490 ns | 13,655 ns | −5.76 % |

**Read the first row as the policy's price and the rest as noise.** Fan-out 1 is the arm ADR-0080 names as decisive, and it costs a reproducible **+4–5 ns** — measured in two independent windows (+3.92 % and +4.95 %), both outside the null. Roughly half of that is the `seq_cst` announcing store itself: rebuilding the arm with a (deliberately unsound) `release` store moves the row to +1.98 %, which is what the full barrier buys and why it is not negotiated away. The favourable wide rows are **not** claimed as a win — a policy binding cannot make a 1,024-way fan-out faster, and they are code-layout luck reported rather than hidden.

The cost is bounded above by construction: it is paid **once per fan-out**, so it amortizes from +5 % at fan-out 1 to inside the null by fan-out 8, and a publish with no subscribers pays nothing at all.

Footprint of the third policy, extending the table above (same rv32 profile and pinned GCC 15.2, measured on the real `core/src/graph.cpp` rather than inferred):

| arm | flash | static RAM | per-thread TLS |
| --- | ---: | ---: | ---: |
| `reclaim_strict` | −254 B | +0 B | **24 B** |
| `reclaim_local` (default) | baseline | baseline | 176 B |
| `reclaim_qsbr` | +884 B | **+2,560 B `.bss`** | **72 B** |

`reclaim_qsbr` is the only policy that costs static RAM, and all of it is the participant table plus the shared retired table — `kQsbrParticipants × max(kCacheLineBytes, 8)` and `kDeferredReleaseSlots` entries. It **buys back** 104 B of per-thread TLS against the default, because its retired pairs live in that shared table instead of in every dispatching thread's park. All of it is `.bss` rather than `.data`, which is deliberate: the global epoch starts at 0 precisely so no non-zero initializer drags the whole registry into flash-backed initialized RAM (it did, at 2,560 B, until that was measured and fixed). **And every byte of it is 0 on a target that does not bind the policy** — which the object-file identity above proves rather than asserts.

Footprint, on rv32 (`-Os -fno-exceptions -fno-rtti`, GCC 15.2, real `core/src/graph.cpp`):

| arm | flash | static RAM | per-thread TLS |
| --- | ---: | ---: | ---: |
| `reclaim_strict` | +68 B | +8 B | **+0 B** |
| `reclaim_local` | +328 B | +8 B | +136 B |

Of `reclaim_local`'s flash, **+24 B lands in `fan_out`** — the dispatch path — and the rest is the `unsubscribe` overload, the release runner and the accessor, all cold. On the same target `reclaim_strict` produces a **byte-identical dispatch path**: +0 instructions in `fan_out`, only label renumbering. That is what justifies keeping it as a distinct policy rather than folding it into the default.

## The theoretical best — shard, don't reclaim

To know "no reader is using X" one must make readers announce themselves (hot-path cost), wait for a grace period (deferred free), or **never share X across threads** so the freeing thread is the only possible user. The third door is the ideal, and it is reached by architecture rather than a cleverer algorithm: **shard subscription ownership per RX thread**. A subscription only ever dispatched *and* unsubscribed by one thread is instantly quiescent on unsubscribe, and an N-core host degenerates into N independent single-threaded problems.

That advice survives `reclaim_qsbr` landing, and is worth restating now that the residual mechanism exists rather than being hypothetical: **binding `reclaim_qsbr` is not a substitute for sharding.** A sharded design pays nothing and needs no grace period at all; this policy is for the genuine residual — a subscription whose one publish fans callbacks across several threads, or a teardown path that unsubscribes from a thread other than the one delivering. Reach for it when the sharding argument does not close, not before, and keep the default everywhere else. That the default provably pays **zero** for this policy's existence is what makes "not before" a free choice rather than a hedge.

## See also

- [02-graph-model.md](02-graph-model.md) — the snapshot-then-dispatch fan-out this seam exists for.
- [08-views-and-ownership.md](08-views-and-ownership.md) — why the *payload* legs of a snapshot need none of this.
- [15-concurrency-and-scaling.md](15-concurrency-and-scaling.md) — the hardware regimes behind "no atomic on the dispatch path".
