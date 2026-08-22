# Write and delivery path

**Scope:** the dispatch structure of the C++23 reference implementation under
[`core/`](https://github.com/avatarsd-llc/libtracer/tree/main/core/) and what it costs on one host. Not the standard, and not
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

`write_impl` (`core/src/graph.cpp:2163`) is the one body behind every write overload. It takes
**no map lock**. The stages, in order:

| stage | what it does |
| --- | --- |
| ACL gate | `acl_allows(v, caller, WRITE)`; a denial returns `PERMISSION_DENIED` and stores nothing |
| branch fork | a POINT payload with the branch bit set decomposes through `write_branch` (`graph.cpp:2287`) |
| store | `store_value` (`graph.cpp:2067`) — LKV or history per role, sequence bump, `await` wake |
| deliver | `deliver_vertex` for a leaf value, `deliver_current` for a `STREAM` (`graph.cpp:2214-2236`) |

The handle overload is the whole of `write(vertex_handle_t, rope_t, caller)`
(`graph.cpp:2644-2646`) — a call straight into `write_impl`. A **path**-addressed write resolves
first, and `find_ptr` takes `map_mutex_` in shared mode (`graph.cpp:1585-1586`), so the path overload
(`graph.cpp:4457`) pays one shared map hold that the handle overload does not.

Two allocation-shaped details on the store leg. A `HANDLER`-role write clones the rope before
storing, because the user handler consumes the value and there is no published pointer to
deliver afterwards; the clone is `try_clone_rope`, nothrow, and on failure the handler still
runs and only the subscriber delivery drops (`graph.cpp:2182-2212`, `try_clone_rope` at `:1853`).
Every other role delivers **the exact pointer `store_value` handed back**, so the hot write path
reclones nothing (`graph.cpp:2223-2236`).

---

## 2. Edge snapshot

Delivery runs outside the vertex lock, because a callback or a re-dispatch may re-enter the
graph. The edge list therefore has to be copied out first. `fan_out` (`graph.cpp:1984`) does that
in one call to `snapshot_edges` (`core/include/libtracer/vertex.hpp:1806`), which takes **no
lock at all** (#635): it copies the vertex's PUBLISHED, immutable-after-publish edge array
(`subscriber.hpp:749`) into one of two buffers under a bounded per-participant **edge pin**
(`core/include/libtracer/edge_pin.hpp:153`), and releases the pin before the caller's first
`dispatch_edge`. The stripe mutex used to be taken here, which serialized the publishes of
every vertex that merely hashed to the same stripe — ×16.6 at twenty-four threads
([ADR-0075](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0075-a-vertexs-edges-are-published-and-read-under-an-edge-pin.md)). It
still serializes writer-vs-writer on the control plane, where a mutation builds the new array
and swaps it in (`vertex.hpp:2536`); the displaced array is scanned and freed on the mutator's
own thread, outside the lock, once no participant announces it (`subscriber.hpp:852`).

It reaches that call only when something subscribes here. `fan_out` opens on
`own_subs_ordered() == 0 ⇒ return` (#635), so an unobserved write — and every placeholder
ancestor a `bubble_up` walks past — never touches the stripe at all. That gate is why the
count is read `seq_cst` where a *delivery* is skipped and relaxed everywhere else: it decides
whether the vertex's own fan-out happens, so it has to be ordered against a subscribe taking
ADR-0049's latch, or a write racing the subscribe reaches neither leg. `mark_pending` is the
**deferred** half of the same decision and reads the same ordered count
([#1140](https://github.com/avatarsd-llc/libtracer/issues/1140)): its skip omits the sweep
mark, so the vertex enters no sweep set and only a later write can re-mark it — omitted work,
not deferred work, and the same lost delivery a skipped fan-out would be. Both are the near
axis, the vertex's OWN count, where the publisher writes the very LKV the racing subscriber's
latch reads.

The **ancestor** gate is the other axis and stays relaxed: `deliver_vertex` and
`mark_pending` both read `listeners_above()` relaxed, and it needs no ordered twin
([#854](https://github.com/avatarsd-llc/libtracer/issues/854), measured and REFUTED): the latch a
subtree subscribe takes snapshots the subscribed *ancestor's* own LKV, never a descendant's, so a
stale zero at that gate is indistinguishable from the write linearizing before the subscribe.

Neither near-axis pairing can be exercised by any x86-64 test: a `seq_cst` store there lowers
to a locked `xchg`, so even a *relaxed* later load of zero is ordered in hardware and the
anomaly cannot appear. The coverage is the weakly-ordered `ubuntu-24.04-arm` CI leg
(core-ci `build-test-arm64`), which is the same memory model the shipped rv32 targets have;
the host guards in `graph_test` pin the *program-order* half only, and say so.

The two-buffer split is the point. `fan_out` chooses which pair of buffers to hand in from the
lock-free `own_subs()` count:

| shape | call | overflow buffer |
| --- | --- | --- |
| wide, `own_subs() > kInlineFanout` | `v->snapshot_edges(inline_buf, tls_buf, drops)` (`graph.cpp:2041`) | a persistent `thread_local` vector, cleared but keeping capacity |
| small, or a nested wide fan-out | `v->snapshot_edges(inline_buf, heap_buf, drops)` (`graph.cpp:2059`) | an empty local vector that never allocates unless the snapshot spills |

`inline_buf` is an `edge_snapshot_t`, a raw byte array placement-constructed into, so a small
fan-out neither allocates nor pays the zeroing a default-constructed `edge_view_t` array would
(`subscriber.hpp:660-698`). Its width is `kInlineFanout` — the no-heap small-fan-out snapshot width,
`edge_snapshot_t::kCapacity`, 8 (`vertex.hpp:693`, `subscriber.hpp:663`). A warm wide publish reuses the
thread-local vector's capacity and so allocates nothing either.

`own_subs()` is read without the lock, so the width it reports can be stale.
That costs nothing but a re-read: **`snapshot_edges` re-checks the width against the published
array** (`graph.cpp:2024-2025`, `vertex.hpp:2618`), so a subscriber added between the count and
the copy costs at most one fallback allocation on the small path and never a wrong answer. Re-entrancy is
handled by a `tls_busy` flag: a subscriber callback that re-publishes takes the local-buffer
path, so the outer fan-out's thread-local buffer is never aliased, and the flag resets on scope
exit (`graph.cpp:2031-2055`).

Both allocations inside the snapshot are nothrow. An unreservable overflow vector degrades the
snapshot to the first `kInlineFanout` views and drops the rest of that delivery; an edge whose
owning copies cannot be cloned is skipped, dropping that one delivery (`vertex.hpp:2625-2640`).
Neither can abort. A thread that cannot claim a pin cell — more concurrent publishers than
`kEdgePinSlots` — copies the current array under the stripe mutex instead, which is the
pre-#635 path for those threads and nobody else; correctness never depends on the constant.

---

## 3. Dispatch tree

Declared in `core/include/libtracer/graph.hpp`, all private:

| function | declaration | role |
| --- | --- | --- |
| `deliver_vertex` | `graph.hpp:2371` | the per-vertex delivery unit both `write` and `propagate` build on: `fan_out`, then `bubble_up` if anyone listens above |
| `fan_out` | `graph.hpp:2330` | return at once if nothing subscribes here; else snapshot under the stripe lock, then `dispatch_edge` per view, outside it |
| `dispatch_edge` | `graph.hpp:2336` | the one dispatch of an edge's three legs, shared by `fan_out` and the admission durability latch so the legs cannot diverge |
| `dispatch_edge_target` | `graph.hpp:2342` | the local re-dispatch leg — a delivery into another vertex |
| `dispatch_edge_remote` | `graph.hpp:2343` | the remote leg — a `FWD{WRITE}` through the injected sink |
| `bubble_up` | `graph.hpp:2367` | vertical fan-out to every registered ancestor's subscribers |

`dispatch_edge` is `always_inline` (`graph.cpp:1973-1982`) precisely so its body stays in the
fan-out loop; the target and remote legs are split into `noinline` helpers to keep that body's
inline estimate small, because the in-process callback leg is the hot case. The three legs are
independent and any subset may fire for one edge: a callback pointer, a target key, and a
non-empty link name.

`bubble_up` (`graph.cpp:2130`, entered only when `listeners_above() > 0`, `:2432`) walks parent pointers, which
are immutable once linked, and so takes **no lock at all**; a placeholder ancestor holds no edges
and its `fan_out` is a no-op. An idle write — nobody subscribed above — pays one relaxed load.

> **The subscriber gate is what makes the paragraph above true, and nothing else does.**
> `snapshot_edges` used to take the vertex **stripe mutex** before it looked at anything, and a
> stripe is shared by `kVertexLockStripes`-many unrelated vertices. Reach it unconditionally and
> an unobserved write took a lock it shares with vertices it has nothing to do with, and every
> ancestor a `bubble_up` visits took another — which is why `fan_out` opens on
> `own_subs_ordered() == 0 ⇒ return` and why the idle write costs one load and no lock. That
> load is `seq_cst` rather than relaxed because its skip has to be ordered against a subscribe
> taking ADR-0049's latch; `vertex_t::own_subs_ordered` carries the pairing that requires it
> (the ancestor gate `listeners_above()` stays relaxed by ruling — #854). The
> rationale and the measurements behind the gate are
> [ADR-0064](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md)'s.
>
> The gate is still what makes an *idle* write free, but it is no longer the only thing standing
> between a fan-1 write and the stripe: `snapshot_edges` now reads a published, immutable edge
> array under an edge pin and takes no lock at all
> ([ADR-0075](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0075-a-vertexs-edges-are-published-and-read-under-an-edge-pin.md)).

---

## 4. Delivery termination

A delivery landing on a target applies exactly the target-local effects of a write — store,
`await` wake, and the target's own handler reaction, all inside `store_value` — and **never**
re-dispatches to the target's own `:subscribers[]` and never bubbles (`graph.cpp:1899-1907`).
Propagation past a target is the target's own logic: a controller re-emits on its execution, a
handler re-emits when it chooses.

The consequence is structural rather than defensive. A dispatch-level subscription cycle cannot
form, so there is **no depth cap, no dedup, and no drain queue**, and nothing to size. The former
`kMaxDispatchDepth` is deleted with nothing replacing it (`graph.hpp:87-92`). An application that
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

`fwd_router_t::deliver_remote` (`core/src/fwd_router.cpp:3116`) is the sink the remote leg calls.
It has two legs, and only one of them copies payload bytes.

**The default full-route leg copies nothing.** It emits
`FWD{ op=WRITE, dst=<stored return route>, src=<empty PATH>, payload=<VALUE> }` as a
scatter-gather send: a fresh stack header, the stored route, an empty `src`, and one span per
rope link (`fwd_router.cpp:3226-3233`). The header is a `stack_writer<16>` — the FWD header of at
most 6 bytes plus the 5-byte op TLV — and both constant TLVs are `constexpr` arrays with no
runtime construction (`fwd_router.cpp:3223-3227`). The route bytes were copied once at subscribe
time, so a delivery re-uses them by reference; a multi-link value crosses as its own segments,
with no flatten. The iov table is a `mem::block_array_t` over the graph's injected `ctl`
`block_source_t`, sized once up front to its exact final entry count, and a refused reservation
drops that delivery rather than emitting a truncated frame (`fwd_router.cpp:3247-3249`). Its entry
count is the *sending* side's choice — `3 + link_count` — so a bounded node bounds it by sizing
that source.

That container is the point, not an implementation detail. `std::vector` +
`tr::detail::try_reserve` sat here until
[#981](https://github.com/avatarsd-llc/libtracer/issues/981), and that helper answers by value only
where the growth **throws** (`core/include/libtracer/mem_heap.hpp:157-171`): under
`-fno-exceptions` it can only probe the global heap, free the probe block and then run the throwing
`reserve`, so a writer-thread context switch in that window makes the `reserve` `abort()` the node
([#850](https://github.com/avatarsd-llc/libtracer/issues/850), the residual
[#923](https://github.com/avatarsd-llc/libtracer/issues/923) could not reach). A `block_array_t`
growth is one refusable `try_alloc` with no second step — no window to lose on any profile — and
`std::span` is trivially copyable, so its `memcpy` relocation is exact. The `try_*` sites whose
element types cannot ride that relocation keep the residual, tabulated in
[`../allocation-and-backpressure.md`](../allocation-and-backpressure.md) and stated at each site.

**The COMPACT leg is the one that flattens.** `value.try_materialize(*flat_)`
(`fwd_router.cpp:3154`) precedes the compact encode, because a COMPACT wraps a contiguous
payload. Single-link — the common case — that materialize is a zero-copy adopt; a multi-link
value pays one flatten per delivery, out of the router's INJECTED byte backend rather than the
global heap. A REFUSED flatten drops the delivery (`fwd_router.cpp:3155`) — since #917 that is a
test on the named refusal, so a legitimately empty value is delivered rather than swept up with
the OOM by an `empty()` guess.
Auto-promotion advertises the label once per flow and then streams
compact frames; a dropped fresh ADVERTISE self-heals through the peer's `HANDLE_NACK`
([RFC-0004 — Remote operation
addressing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md)
§E.1).

So delivery compaction trades a per-delivery flatten on multi-link values for a smaller frame.
On single-link values it costs nothing; on roped values it is the one place the delivery path
copies payload.

---

## 6. Delivery drops

Three conditions make a target-edge delivery impossible, and all three drop that one delivery
rather than failing the write — the write itself succeeded and the other legs still ran. Two
more sheds happen further up, before an edge is ever dispatched. Two more again come from the
**net plane**, which performs deliveries the graph never sees. What is counted is the drop:

```cpp
struct delivery_drops_t {
    std::uint64_t no_target = 0;         // graph.hpp:2209
    std::uint64_t denied = 0;            // graph.hpp:2219
    std::uint64_t out_of_memory = 0;     // graph.hpp:2222
    std::uint64_t fan_out_truncated = 0; // graph.hpp:2226
};
```

| counter | condition | site |
| --- | --- | --- |
| `no_target` | the target PATH resolved to no live vertex — retired, or never created | `graph.cpp:1878-1881` |
| `denied` | a subscription edge's delivery was refused by the target's `:acl`, gated on the **edge's stored caller**, not the writer's | `graph.cpp:1895-1898` |
| `denied` | a WRITE was refused at the graph's own gate — the API write, the `FWD{WRITE}` terminus and both `COMPACT` terminus arms enter through it | `graph.cpp:2163-2201` |
| `no_target` | a net-plane route resolved to no vertex (`fwd_router.cpp:3016`), or its binding vanished under a concurrent unbind (`:2911`) | `fwd_router.cpp:2911`, `:3016` |
| `out_of_memory` | a `COMPACT` terminus could not take the payload view or reserve its rope | `fwd_router.cpp:2862-2864`, `:2873`, `:3023` |
| `out_of_memory` | the nothrow delivery clone could not be allocated | `graph.cpp:1907-1910` |
| `out_of_memory` | a HANDLER write's notify clone failed — the WHOLE fan-out is shed, one count per subscriber | `graph.cpp:2210` |
| `fan_out_truncated` | the wide-fan-out overflow buffer could not be reserved, so every edge past the inline prefix was abandoned | `vertex.hpp:2630` |

A fifth `out_of_memory` site used to sit in this table — an edge whose owning link/caller
copies could not be allocated was skipped by the snapshot. [#1448](https://github.com/avatarsd-llc/libtracer/issues/1448)
removed the *failure*, not the report: the dispatch snapshot now holds a refcount share of the
immutable cold half and copies no bytes, so the per-edge snapshot has no allocation left to
fail and `snapshot_drops_t` carries only `truncated`.

The handler shed and the snapshot sheds were **uncounted until #896**, and the handler one is
why that mattered: it sheds
every subscriber of the vertex and still returns success, so `delivery_drops()` — the only thing
that could say so — read zero while a whole fan-out evaporated. The unit of every counter is
therefore a **delivery**, not an event: a shed fan-out of N counts N, which is why the
remaining snapshot shed is tallied inside `vertex_t::snapshot_edges` (`vertex.hpp:1760`, its
`snapshot_drops_t`) and folded by `fan_out` (`graph.cpp:2044-2045`, `:2059-2060`) through
`count_snapshot_drops` (`:1530`) rather than counted as one at the caller. Every site **on
this plane** goes through one door, `count_drop` (`:1495`), so a path here that abandons an
admitted delivery without counting it is a visible omission.

The net plane reaches that door through exactly one public method, `count_external_drop`
(`graph.hpp:2264`, `graph.cpp:1515`), which maps its two causes onto `count_drop` and adds no
second counting mechanism (#1068). It is a method rather than a friendship because the
counters are a published surface while the internal drop sites are not: a deliverer needs to
add to the numbers, not to reach into the machinery that maintains them. `denied` is absent
from it on purpose — a refusal is counted at the WRITE gate, on every plane, so a router that
also counted the `PERMISSION_DENIED` it discards would report one refusal twice.

The two sheds that happen *before* the fan-out went through this door in #1003, and the plane
boundary that used to qualify this section is gone. A STREAM ring-append refused under
allocation pressure abandons the write's whole fan-out without ever reaching a dispatch site —
for a STREAM the ring drain **is** the fan-out, so the skipped entry is a delivery every
subscriber loses, and the eager delivery then drains zero and returns before one edge is
snapshotted. A `mark_pending` OOM leg is the same loss deferred: an unmarked vertex rides no
sweep, so with no later write to re-mark it the assigned value is never delivered at all.

Both now count at the same one-per-subscriber width as the handler leg. The write still
answers `SUCCESS` in both cases — the value publish landed, and a stream's history is
bounded-lossy by contract (RFC-0008 §E) — which is precisely why the counter has to carry the
loss instead. `vertex_t::store` reports its shed by reference (`store_drops_t`, the shape
`snapshot_drops_t` uses) because the storage layer owns no counters; `store_value` passes the
tally on as a **required** out-parameter, so a graph write path cannot abandon a delivery by
forgetting to ask. Whether a shed cost a delivery stays the caller's call: a branch *notify*
fans its slice out eagerly and flushes the drain cursor, so its shed costs history rather than
a delivery and is deliberately not counted.

Read `delivery_drops()` as *what the vertex shed*, with one documented exclusion: the ancestor
legs a bubble would also have served are not counted, because #854's close ruling dropped
ancestor-leg drop instrumentation outright.

`delivery_drops()` (`graph.hpp:2237`, `graph.cpp:1488`) is the only record that any of this
happened. Without it, a node whose target was retired, or whose fan-in gate denies the edge's
stored caller, drops every delivery for the rest of its life with nothing anywhere to say so.

Two properties of the counters are deliberate. They are **counted, never enforced** — nothing in
the library reads them, so a deployment chooses whether to alarm. And the loads are
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
tracked at [#635](https://github.com/avatarsd-llc/libtracer/issues/635). Deleting the lock was
never the fix — `snapshot_edges` copies the edge list, and a concurrent `add_edge` /
`clear_edge` would race.

> **Landed (2026-08-03,
> [ADR-0075](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0075-a-vertexs-edges-are-published-and-read-under-an-edge-pin.md)).**
> What replaced the lock is a published immutable edge array read under a bounded edge pin, so
> the copy is race-free without excluding anybody. Re-measured against the same arm on the same
> host under the #807 protocol (15 interleaved pairs, medians, disjoint ranges): **×3.59 at
> T=4, ×9.21 at T=8, ×16.17 at T=16, ×19.35 at T=24**, with aggregate throughput monotonically
> non-decreasing across that sweep — the negative scaling is gone. The distinct-stripe control
> stays at unity, and the single-threaded arm, pinned, is ×1.13 *faster*.

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
(`core/CHANGELOG.md:1509-1513`). The digest is a filter and never a decision — `live()` and the full
compare still gate every answer — and the two digest functions are pinned against each other
directly by `test_digest_paths_agree`
(`core/tests/registry_teardown_test.cpp:275`), because a disagreement would throw nothing and
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
