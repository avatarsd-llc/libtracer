# Write and delivery path

**Scope:** the dispatch structure of the C++23 reference implementation under
[`../../../core/`](../../../core/) and what it costs on one host. Not the standard, and not
normative — the obligations any implementation must meet are in
[`../../reference/15-concurrency-and-scaling.md`](../../reference/15-concurrency-and-scaling.md).
The read half of the runtime is [`00-scaling-and-serialization.md`](00-scaling-and-serialization.md);
the failure semantics of a dropped allocation are
[`../allocation-and-backpressure.md`](../allocation-and-backpressure.md).

A write is the more expensive half of the runtime. A read touches one vertex; a write stores a
value and then delivers it to every edge on that vertex and on every ancestor that has a
listener, through three legs with different costs. This page describes that machinery: which
lock each stage takes, where the buffers come from, and what the delivery legs copy.

---

## 1. Write path

`write_impl` (`core/src/graph.cpp:974`) is the one body behind every write overload. It takes
**no map lock**. The stages, in order:

| stage | what it does |
| --- | --- |
| ACL gate | `acl_allows(v, caller, WRITE)`; a denial returns `PERMISSION_DENIED` and stores nothing |
| branch fork | a POINT payload with the branch bit set decomposes through `write_branch` (`graph.cpp:906`) |
| store | `store_value` (`graph.cpp:845`) — LKV or history per role, sequence bump, `await` wake |
| deliver | `deliver_vertex` for a leaf value, `deliver_current` for a `STREAM` (`graph.cpp:923-931`) |

The handle overload is the whole of `write(vertex_handle_t, rope_t, caller)`
(`graph.cpp:1185-1187`) — a call straight into `write_impl`. A **path**-addressed write resolves
first, and `find_ptr` takes `map_mutex_` in shared mode (`graph.cpp:546`), so the path overload
(`graph.cpp:2143`) pays one shared map hold that the handle overload does not.

Two allocation-shaped details on the store leg. A `HANDLER`-role write clones the rope before
storing, because the user handler consumes the value and there is no published pointer to
deliver afterwards; the clone is `try_clone_rope`, nothrow, and on failure the handler still
runs and only the subscriber delivery drops (`graph.cpp:907-919`, `try_clone_rope` at `:729`).
Every other role delivers **the exact pointer `store_value` handed back**, so the hot write path
reclones nothing (`graph.cpp:928-930`).

---

## 2. Edge snapshot

Delivery runs outside the vertex lock, because a callback or a re-dispatch may re-enter the
graph. The edge list therefore has to be copied out first. `fan_out` (`graph.cpp:794`) does that
in one call to `snapshot_edges` (`core/include/libtracer/vertex.hpp:1549`), which takes the
vertex stripe mutex, walks `subs_`, and fills one of two buffers.

It reaches that call only when something subscribes here. `fan_out` opens on
`own_subs_ordered() == 0 ⇒ return` (#635), so an unobserved write — and every placeholder
ancestor a `bubble_up` walks past — never touches the stripe at all. That gate is why the
count is read `seq_cst` in this one place and relaxed everywhere else: it is the only read
that decides whether to deliver, so it has to be ordered against a subscribe taking ADR-0049's
latch, or a write racing the subscribe reaches neither leg.

The two-buffer split is the point. `fan_out` chooses which pair of buffers to hand in from the
lock-free `own_subs()` count:

| shape | call | overflow buffer |
| --- | --- | --- |
| wide, `own_subs() > kInlineFanout` | `v->snapshot_edges(inline_buf, tls_buf)` (`graph.cpp:825`) | a persistent `thread_local` vector, cleared but keeping capacity |
| small, or a nested wide fan-out | `v->snapshot_edges(inline_buf, heap_buf)` (`graph.cpp:838`) | an empty local vector that never allocates unless the snapshot spills |

`inline_buf` is an `edge_snapshot_t`, a raw byte array placement-constructed into, so a small
fan-out neither allocates nor pays the zeroing a default-constructed `edge_view_t` array would
(`vertex.hpp:674-712`). Its width is `kInlineFanout` — the no-heap small-fan-out snapshot width,
`edge_snapshot_t::kCapacity`, 8 (`vertex.hpp:910`, `:691`). A warm wide publish reuses the
thread-local vector's capacity and so allocates nothing either.

`own_subs()` is read without the lock, so the width it reports can be stale.
That costs nothing but a re-read: **`snapshot_edges` re-checks the width under the lock**
(`graph.cpp:809`, `vertex.hpp:1486`), so a subscriber added between the count and the lock costs
at most one fallback allocation on the small path and never a wrong answer. Re-entrancy is
handled by a `tls_busy` flag: a subscriber callback that re-publishes takes the local-buffer
path, so the outer fan-out's thread-local buffer is never aliased, and the flag resets on scope
exit (`graph.cpp:815-833`).

Both allocations inside the snapshot are nothrow. An unreservable overflow vector degrades the
snapshot to the first `kInlineFanout` views and drops the rest of that delivery; an edge whose
owning copies cannot be cloned is skipped, dropping that one delivery (`vertex.hpp:1470-1499`).
Neither can abort.

---

## 3. Dispatch tree

Declared in `core/include/libtracer/graph.hpp`, all private:

| function | declaration | role |
| --- | --- | --- |
| `deliver_vertex` | `graph.hpp:820` | the per-vertex delivery unit both `write` and `propagate` build on: `fan_out`, then `bubble_up` if anyone listens above |
| `fan_out` | `graph.hpp:800` | return at once if nothing subscribes here; else snapshot under the stripe lock, then `dispatch_edge` per view, outside it |
| `dispatch_edge` | `graph.hpp:806` | the one dispatch of an edge's three legs, shared by `fan_out` and the admission durability latch so the legs cannot diverge |
| `dispatch_edge_target` | `graph.hpp:812` | the local re-dispatch leg — a delivery into another vertex |
| `dispatch_edge_remote` | `graph.hpp:813` | the remote leg — a `FWD{WRITE}` through the injected sink |
| `bubble_up` | `graph.hpp:816` | vertical fan-out to every registered ancestor's subscribers |

`dispatch_edge` is defined inline (`graph.cpp:784-792`) precisely so its body stays in the
fan-out loop; the target and remote legs are split into out-of-line helpers to keep that body's
inline estimate small, because the in-process callback leg is the hot case. The three legs are
independent and any subset may fire for one edge: a callback pointer, a target key, and a
non-empty link name.

`bubble_up` (`core/src/graph.cpp:941`, entered only when `listeners_above() > 0`, `:1127`) walks parent pointers, which
are immutable once linked, and so takes **no lock at all**; a placeholder ancestor holds no edges
and its `fan_out` is a no-op. An idle write — nobody subscribed above — pays one relaxed load.

> **This paragraph was false until 2026-07-31 ([#635](https://github.com/avatarsd-llc/libtracer/issues/635)), and so were the three reference-doc statements of the same cost model.** A placeholder ancestor's `fan_out` was *not* a no-op and an idle write did *not* pay one relaxed load: `fan_out` reached `snapshot_edges` unconditionally, and `snapshot_edges` takes the vertex **stripe mutex** before it looks at anything. So an unobserved write took a lock shared with `kVertexLockStripes`-many unrelated vertices, and each ancestor `bubble_up` visited took another. `fan_out` now gates on `own_subs_ordered()` first, which is what makes the sentence above true as written — modulo the load being `seq_cst` rather than relaxed, because it is the one read that decides whether to deliver at all (see `vertex_t::own_subs_ordered` for the pairing that requires it).

---

## 4. Delivery termination

A delivery landing on a target applies exactly the target-local effects of a write — store,
`await` wake, and the target's own handler reaction, all inside `store_value` — and **never**
re-dispatches to the target's own `:subscribers[]` and never bubbles (`graph.cpp:754-759`).
Propagation past a target is the target's own logic: a controller re-emits on its execution, a
handler re-emits when it chooses.

The consequence is structural rather than defensive. A dispatch-level subscription cycle cannot
form, so there is **no depth cap, no dedup, and no drain queue**, and nothing to size. The former
`kMaxDispatchDepth` is deleted with nothing replacing it (`graph.hpp:78-83`). An application that
wants pure relay subscribes the consumer directly.

Rejected alternative: a depth counter threaded through dispatch. It would bound a recursion that
the termination rule makes impossible, and its constant would be a synthetic limit with
no resource behind it.

Cited: [ADR-0051 — Delivery terminates at target, no dispatch
limits](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0051-delivery-terminates-at-target-no-dispatch-limits.md),
[RFC-0007 — Delivery terminates at
target](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0007-delivery-terminates-at-target.md).

---

## 5. Remote delivery legs

`fwd_router_t::deliver_remote` (`core/src/fwd_router.cpp:1159`) is the sink the remote leg calls.
It has two legs, and only one of them copies payload bytes.

**The default full-route leg copies nothing.** It emits
`FWD{ op=WRITE, dst=<stored return route>, src=<empty PATH>, payload=<VALUE> }` as a
scatter-gather send: a fresh stack header, the stored route, an empty `src`, and one span per
rope link (`fwd_router.cpp:1159-1118`). The header is a `stack_writer<16>` — the FWD header of at
most 6 bytes plus the 5-byte op TLV — and both constant TLVs are `constexpr` arrays with no
runtime construction (`fwd_router.cpp:988-995`). The route bytes were copied once at subscribe
time, so a delivery re-uses them by reference; a multi-link value crosses as its own segments,
with no flatten. The iov vector is nothrow-reserved and an exhausted reserve drops that delivery
rather than emitting a truncated frame (`fwd_router.cpp:1002-1005`).

**The COMPACT leg is the one that flattens.** `const view_t flat = value.materialize();`
(`fwd_router.cpp:970`) precedes the compact encode, because a COMPACT wraps a contiguous
payload. Single-link — the common case — that materialize is a zero-copy adopt; a multi-link
value pays one flatten per delivery. A failed flatten drops the delivery
(`fwd_router.cpp:971`). Auto-promotion advertises the label once per flow and then streams
compact frames; a dropped fresh ADVERTISE self-heals through the peer's `HANDLE_NACK`
([RFC-0004 — Remote operation
addressing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)
§E.1).

So delivery compaction trades a per-delivery flatten on multi-link values for a smaller frame.
On single-link values it costs nothing; on roped values it is the one place the delivery path
copies payload.

---

## 6. Delivery drops

Three conditions make a delivery impossible, and all three drop that one delivery rather than
failing the write — the write itself succeeded and the other legs still ran. What is counted is
the drop:

```cpp
struct delivery_drops_t {
    std::uint64_t no_target = 0;      // graph.hpp:753
    std::uint64_t denied = 0;         // graph.hpp:755
    std::uint64_t out_of_memory = 0;  // graph.hpp:757
};
```

| counter | condition | site |
| --- | --- | --- |
| `no_target` | the target PATH resolved to no live vertex — retired, or never created | `graph.cpp:742-745` |
| `denied` | the target's `:acl` denied WRITE to the **edge's stored caller**, not the writer's | `graph.cpp:749-752` |
| `out_of_memory` | the nothrow delivery clone could not be allocated | `graph.cpp:760-764` |

`delivery_drops()` (`graph.hpp:768`, `graph.cpp:515`) is the only record that any of this
happened. Without it, a node whose target was retired, or whose fan-in gate denies the edge's
stored caller, drops every delivery for the rest of its life with nothing anywhere to say so.

Two properties of the counters are deliberate. They are **counted, never enforced** — nothing in
the library reads them, so a deployment chooses whether to alarm. And the three loads are
individually relaxed rather than one atomic snapshot, so a reader racing a delivering thread can
see a torn total: making it coherent would put a lock on the drop path to serve a diagnostic,
and the useful reading of a monotonic counter is "is this growing", not an instant.

The `out_of_memory` cause is where this page meets allocation policy. What a nothrow clone
failure means, which resource it drew from, and why exhaustion is a value rather than an abort
are in [`../allocation-and-backpressure.md`](../allocation-and-backpressure.md).

Cited: [ADR-0026 — Consumer-initiated subscription is a client
write](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0026-consumer-initiated-subscription-client-write.md)
for the fan-in gate.

---

## 7. Cost

Every figure below carries its conditions from the record named beside it. Nothing here is
re-derived, and no figure appears without its shape.

**The delivery stripe lock.** Instrument `bench/bench_lkv_slot` in `graph` mode, ablation by
deleting the lock, fan-1 writes, median of 3 runs, 24-thread host:

| T | lock in place | lock removed | |
| ---: | ---: | ---: | ---: |
| 1 | 11.25 M/s | 11.21 M/s | ×1.00 |
| 4 | 7.56 M/s | 22.58 M/s | ×2.99 |
| 8 | 4.58 M/s | 34.26 M/s | ×7.47 |
| 16 | 4.61 M/s | 57.97 M/s | ×12.56 |
| 24 | 4.75 M/s | 78.73 M/s | **×16.59** |

This is the **adversarial** shape: T writers on T distinct vertices deliberately chosen to
collide on one stripe. The two halves belong together, because the second is what stops the
first from justifying work. On the realistic shape — writers that do not collide on one stripe
— removing the same lock is **1.02×**. The ×16.6 arm carries its full T sweep, run count and
host; the 1.02× carries its shape and no run count or spread, so it settles the direction and
not the magnitude. ×1.00 at T=1 is the same measurement's own control: the lock genuinely is
free where it sits, for one thread. Source: [ADR-0064 — LKV publish is waiterless and the slot
becomes
lock-free](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md),
tracked at [#635](https://github.com/avatarsd-llc/libtracer/issues/635). Deleting the lock is not
the fix — `snapshot_edges` copies `subs_`, and a concurrent `add_edge` / `clear_edge` would race.

**The write path's absence of a map lock, as a control arm.** The `has_registered_child`
ablation that removed the map lock from every read is a 1.00× no-op on the write path:
`spread-fan0` writes at T=8 run 35.4 M/s stock and 35.4 M/s ablated, three runs, medians, host
AMD Ryzen AI 9 HX PRO 375 (12 physical cores / 24 SMT threads, Release `-O3`, GCC 14,
libstdc++). A control arm at 1.00× is the evidence that `write_impl` never took the lock; the
read side of the same table is in [`00-scaling-and-serialization.md`](00-scaling-and-serialization.md) §3.

**The per-slot name digest on the forward demux mount scan.** Instrument
`bench/bench_forward_demux`, 8 alternating rounds across two builds. The scan's marginal cost
over a fixed-position hop falls **35 ns to ~0 at 8 links, 86 to 2 at 16, and 333 to 17 at 64
links (95% removed)**, with the fixed-position hop itself unchanged within ±2 ns
(`core/CHANGELOG.md:62-69`). The digest is a filter and never a decision — `live()` and the full
compare still gate every answer — and the two digest functions are pinned against each other
directly by `test_digest_paths_agree`
(`core/tests/registry_teardown_test.cpp:289`), because a disagreement would throw nothing and
break no obvious test; the registry would simply stop resolving the affected name, which reads
as a routing bug arbitrarily far away. Tracked at
[#660](https://github.com/avatarsd-llc/libtracer/pull/660). The record for this figure names its
instrument and round count but not its host.

**A digest's fixed cost is part of its measurement.** The first mount digest removed 91% of the
scan cost **and added ~30 ns — a fifth of a whole forward hop — to every lookup**. Measuring only
the axis being improved hides that completely, and any further filter on this path carries the
same risk.

---

## 8. How measurement goes wrong here

| A plausible claim | What checking shows | The check that decides it |
| --- | --- | --- |
| The `snapshot_edges` stripe lock is free — measured at 0 cycles | True at T=1 and only at T=1. From T=2 it is the dominant term on the write path: ×16.59 at T=24 on distinct vertices sharing one stripe | Re-run the ablation across the whole thread sweep. A single-thread result generalized to all thread counts is not a result about a lock |
| The stripe lock is the residual on the read path | `snapshot_edges` is on the **delivery** path; `graph_t::read` never calls it | Read the call graph before attributing a cost to a lock |
| A digest that removes 91% of a scan is a win | It also added ~30 ns to every lookup — a fifth of a forward hop — which the improved axis does not show | Measure the axis **not** being improved, on the same binary |
| A ×16.6 lock removal describes the workload | ×16.6 is the shape built to collide on one stripe; the shape that does not collide measures 1.02× | Run the adversarial and the realistic shape in the same session, and quote both |
| A zero-subscriber write skips the delivery machinery | It does **since #635** and did not before — `fan_out` reached `snapshot_edges` unconditionally, so an unobserved write took the stripe lock too. Three reference docs asserted the near-free-when-idle model this whole time | Bench the fan-0 topology, not only fan-1 — and when a doc states a cost model, check the code keeps it |
| Removing a lock cannot make a shape slower | On the ONE-shared-vertex fan-0 arm, removing the stripe lock from the idle path made it **slower**: the mutex was accidental admission control in front of libstdc++'s spin-locked `atomic<shared_ptr>`, and unthrottled spinners thrash it. Two independent changes (the #635 gate, and a `shared_mutex` split) reproduced it at the same magnitude | Keep a true-sharing arm alongside the false-sharing one; a change that helps one can hurt the other |
| A wide fan-out allocates per publish | A **warm** wide publish reuses the thread-local buffer's capacity; a cold one, and a nested re-entrant one, allocate | Time the second publish and the first separately; a re-entrant callback puts you on the local-buffer path |
