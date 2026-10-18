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

The first three pay on the dispatch path for what a grace period pays on the reclaim path, which is why libtracer takes a grace period. One shape that looks cheaper is not offered: a grace point stated over **one thread** ("this thread's dispatch stack unwound"). The context's lifetime has to outlast every snapshot that names it, on every thread, and on one CPU as on many, so the grace point is stated over every dispatching thread ([ADR-0080 Amendment 1](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md#amendment-1-2026-10-12-one-grace-mechanism) §1). `reclaim_local`, the per-thread policy that was the default until #2127, is withdrawn in favour of `reclaim_qsbr`.

In the reference implementation each tenant has its own answer: the value seam is freed by the explicit collector `graph_t::collect()`, the `mount_tlv` rebind is immutable by construction (the replaced bytes are a pure function of the slot key, so nothing is ever replaced), and the last-known-value slot keeps its own private hazard policy (`lkv_slot.hpp`), whose per-thread retired list each thread drains at its own quiescent point. The one shared mechanism is the grace period below, which frees the subscriber `{fn, ctx}` pair. No generalized hazard domain exists.

## The model: a grace point, closed at build time

A **grace point** is the moment at which a retired `{fn, ctx}` pair is provably named by no live snapshot, and is therefore safe to free. Each policy is a different grace point, selected the same way every other per-target knob is — a `using` in the build's configuration ([ADR-0068](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0068-build-configuration-is-plain-cxx.md)), never a runtime flag:

```cpp
// libtracer/config_override.hpp
#pragma once
namespace tr::graph {
struct my_node_config_t : default_config_t {
    using reclaim_policy_t = reclaim_strict_t;   // or leave it: the default is reclaim_qsbr_t
};
using config_t = my_node_config_t;
}  // namespace tr::graph
```

| policy | grace point | target | dispatch cost | unsubscribe while a dispatch is live |
| --- | --- | --- | --- | --- |
| **`reclaim_qsbr`** (default) | every dispatching thread has passed a quiescent state | WIDE, MID and NARROW | one relaxed load, one release store and one fence **per outermost bracket** (a compiler barrier under `kSingleCore`) | supported, on any thread |
| `reclaim_strict` | `unsubscribe()` returns | a deployment that can show no dispatch is live when it unsubscribes | **zero** | forbidden, and not detected |

The tag set is closed at these two. A build that still names `reclaim_local_t` is refused at compile time, with the migration note.

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

### `reclaim_qsbr` — the default

It is quiescent-state reclamation in the URCU sense. The dispatch bracket keeps a per-thread depth whose transition to 0 is the proof that this thread holds no snapshot, and the *outermost* bracket adds:

- **entry**: one relaxed load of a global epoch — a read-mostly line, bumped only on the control plane — and one release store into this thread's own cache-line-isolated cell, marking it online at that epoch, then the entry fence. **No atomic read-modify-write.** The fence is chosen at compile time (ADR-0080 Amendment 1 §2): a `seq_cst` fence by default, a compiler barrier under `kSingleCore`, or none with the `membarrier` opt-in on Linux.
- **exit**: one release store of `0` to the same cell. Zero means "holds nothing", which is what makes a thread idling in `epoll` non-blocking — the classic QSBR liveness hole, closed structurally rather than by a `thread_offline()` verb the embedder must remember to call.

`unsubscribe()` then bumps the epoch once and scans the participant table. A pair retired at epoch `E` is free once every participant is either offline or online at an epoch greater than `E`. **The scan is on the reclaim path only** — that is the precise difference from the hazard-pointer shape in the table above, which put its scan on the *read* path and is why that one collapsed throughput.

- **No participant mid-dispatch** (the ordinary case): the scan concludes at once, the hook runs **inline**, and `unsubscribe()` returns already quiescent.
- **Some participant mid-dispatch** — including this thread, when it unsubscribes from inside a delivery: the pair waits in one shared, bounded table (`kDeferredReleaseSlots`, default 16) and the participant whose quiescent state completes the grace period runs the hook. On a single-threaded node that is this thread's own outermost dispatch exit, before the `write()` that started it returns.

Two consequences the API states rather than hides:

- **The retired pairs live in one shared table, not per-thread.** They have to. The case this policy exists for is B retiring while A dispatches; if B parked the pair in B's own storage nothing would ever drain it, because B is not dispatching and "unsubscribe once, then never touch the graph again" is the dominant teardown shape.
- **So the hook may run on a thread other than the `unsubscribe()` caller**, at that thread's priority and on its stack, after `unsubscribe()` has returned. It must be short, thread-safe, non-blocking and `noexcept`; posting the context to an app-owned queue is the ideal shape. A grace period that spans threads cannot promise the caller's thread, because the only alternatives are to block the caller (which the rule above forbids) or to leak. Parity across targets now means *the hook runs once, after every snapshot that could name the pair is gone, on whichever participant completes the grace period — the same on every target.*

On overflow the pair is **dropped and its hook never runs** — a deliberate leak, because the alternative is releasing a context a live fan-out still names. That `unsubscribe` answers `BACKPRESSURE` (the edge is gone all the same), and `graph_t::reclaim_refusals()` counts every drop as `release_refused`, so an undersized bound is visible rather than silent.

When the same build also binds `hazard_slot_t`, each thread's quiescent point additionally self-drains its **own** private retired LKV list. That is the half of [#897](https://github.com/avatarsd-llc/libtracer/issues/897) ADR-0080 assigns to this policy — `~hazard_slot_t` never has to reach across a live thread's private list — and it costs `lkv_slot.hpp` no code at all: the quiescent point calls the already-shipped `retire_and_flush`, whose own early-out makes it free on a thread that parked nothing.

`graph_t::thread_quiescent()` exists for the thread the automatic path cannot reach — one that displaces LKV nodes without ever dispatching, and the stated teardown precondition before an injected `std::pmr` resource dies. **No guarantee depends on an embedder calling it**; every dispatching thread announces and drains automatically, and under `reclaim_strict` it is an empty inline function.

### `reclaim_strict` — the opt-out

For a deployment that can show no dispatch is live — on any thread — when it unsubscribes, or whose context is thread-affine and must be released on its own thread. Nothing is tracked, nothing is stored, `unsubscribe()` always releases inline. Unsubscribing while a dispatch is live is **forbidden and not detected**: the policy keeps no dispatch state, so no build can see the violation. That is the trade it is for.

## What it costs — measured

The study behind the amendment measured the choice on three targets ([ADR-0080 Amendment 1](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md#amendment-1-2026-10-12-one-grace-mechanism) §9.1). With the single trivially destructible per-thread record of #2124, `reclaim_qsbr` against the withdrawn default:

| row | `reclaim_local` (withdrawn) | `reclaim_qsbr` (default) | `reclaim_strict` |
| --- | --- | --- | --- |
| cross-thread unsubscribe during a delivery | release did not wait for other threads' snapshots | release after every snapshot ends | forbidden |
| WIDE `forward` / `publish1` / `publish8` p50 | 120 / 90 / 110 ns | 130 / 100 / 110 ns | 120 / 90 / 100 ns |
| WIDE TLS per thread | 272 B | 16 B | 16 B |
| MID TLS per FreeRTOS task | 192 B | 64 B | 64 B |

The bracket costs per frame or per edge, never per byte: every row measured at 1, 4 and 16 KiB shows the same picture as 64 B. The measurement for the change that made it the default is in that change's pull request.

## The theoretical best — shard, don't reclaim

To know "no reader is using X" one must make readers announce themselves (hot-path cost), wait for a grace period (deferred free), or **never share X across threads** so the freeing thread is the only possible user. The third door is the ideal, and it is reached by architecture rather than a cleverer algorithm: **shard subscription ownership per RX thread**. A subscription only ever dispatched *and* unsubscribed by one thread is instantly quiescent on unsubscribe, and an N-core host degenerates into N independent single-threaded problems.

That advice survives `reclaim_qsbr` becoming the default: **the grace period is not a substitute for sharding.** A sharded design never makes the grace period wait, because no other thread is ever inside a snapshot that names the pair, so every unsubscribe concludes inline. The grace period is what keeps the residual safe — a subscription whose one publish fans callbacks across several threads, or a teardown path that unsubscribes from a thread other than the one delivering.

## See also

- [02-graph-model.md](02-graph-model.md) — the snapshot-then-dispatch fan-out this seam exists for.
- [08-views-and-ownership.md](08-views-and-ownership.md) — why the *payload* legs of a snapshot need none of this.
- [15-concurrency-and-scaling.md](15-concurrency-and-scaling.md) — the hardware regimes behind "no atomic on the dispatch path".
