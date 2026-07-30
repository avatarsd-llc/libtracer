# Scaling and serialization in the reference implementation

> **Status:** design record, 2026-07-30. **Host for every number below:** AMD Ryzen AI 9
> HX PRO 375 — 12 physical cores / 24 SMT threads, 1 socket, 1 NUMA node, 24 MiB L3 in **two**
> instances. Release `-O3`, GCC 14, libstdc++. **Perfect scaling on this host is ~10× at
> T=24, not 24×.** The hardware model these numbers sit in is
> [`../../reference/15-concurrency-and-scaling.md`](../../reference/15-concurrency-and-scaling.md) §3.
>
> **⚠ This document's subject has been mis-attributed three times.** §7 is the ledger. The
> short version: the residual was blamed on the delivery-path stripe lock, then on "nothing
> process-wide", before an ablation found a process-wide reader-writer lock on every read. Read
> §7 before trusting any causal claim in this area from an older document.

---

## 1. What was wrong, and how it hid

A `graph_t::read` on distinct vertices ran at ~19.7 M ops/s aggregate at 24 readers — and at
~18.7 M/s at *one* reader. Flat.

Flat looks like health. It is the opposite: 24 threads producing the same total as one means
each thread is 24× slower, which is the signature of a **perfect serializer**. Nothing in the
scaling curve distinguishes "no contention" from "one global lock", because a *blocking* lock
plateaus rather than collapsing (reference §3 regime (c)). Two documents concluded from the flat
curve that nothing process-wide was serializing. Both were wrong, and the second one said so in
as many words — *"nothing process-wide is serializing — not the map lock"* — about the exact
lock that was.

The lock was `map_mutex_`, taken **shared on every read** by `has_registered_child` to decide
the leaf/branch fork. It was removed in #654.

---

## 2. The serializer inventory

Every synchronization point in the graph runtime, what it protects, and where it sits.
Line numbers are `core/src/graph.cpp` and `core/include/libtracer/vertex.hpp` as of #654.

### 2.1 `map_mutex_` — one `std::shared_mutex` per graph

Process-wide by construction: one graph, one mutex. Guards the ADR-0057 Composite child links
(`children_->sorted`, appended under the unique lock, so an unsynchronized iteration can
observe a reallocation) and the `registered_` placeholder flag.

| taken | where | frequency |
| --- | --- | --- |
| **unique** | `register_vertex_key` (`graph.cpp:333`), `retire` (`:398`, `:421`), `retire_subtree` (`:379`) | control plane |
| shared | `find_ptr` (`:546`) — **so every `path_t` overload pays it once** | per op, path-addressed only |
| shared | `field_write` (`:1478`) | per `:field` write |
| shared | `read_children` (`:1801`), `read_children_folded` (`:1848`), `read_subtree_folded` (`:1917`) | per composed read — these walk, so they need it |
| shared | `note_subscriber_added` / `_removed` (`:534`, `:540`), `evict_link_edges` (`:443`, `:448`), `has_first_level_child` (`:610`) | control plane |
| ~~shared~~ | ~~`has_registered_child`~~ — **removed by #654**, was on every read | — |

So after #654 a **handle**-addressed scalar read or write takes no map lock at all. A
**path**-addressed one still resolves through `find_ptr` and pays it once. Every measurement in
this document uses the handle overloads, which `bench_lkv_slot` benches exclusively — the
path-addressed numbers are worse than anything here and have not been measured.

### 2.2 The vertex lock stripes — `kVertexLockStripes` mutexes, process-wide

`vertex.hpp:701`, sized by the ADR-0068 config knob (default 16), selected by
`vertex_stripe_of` hashing the vertex address (`:719`). Guards the fan-out edge list, the STREAM
ring, the write-sequence bump and the ACL state. Taken by `snapshot_edges` (`:1406`) on **every
delivery**, and by `add_edge` / `clear_edge` / `set_acl`.

Two vertices that hash to the same stripe contend even though they share nothing else — which
is what the `stripe1` bench topology exists to measure.

### 2.3 The LKV slot — per vertex, policy-selected

`lkv_slot_t` in `config.hpp` (ADR-0069). Two bindings ship:

| binding | mechanism | regime (reference §3) |
| --- | --- | --- |
| `sp_atomic_slot_t` (default) | `std::atomic<std::shared_ptr<const rope_t>>` | **(d)** — libstdc++ spins its `_Sp_locker` pointer-lock bit |
| `hazard_slot_t` | lock-free `atomic<node*>` + a hazard-pointer domain | **(b)** — one contended RMW for the promotion |

### 2.4 What has no lock

The write-sequence read, the subtree-listener counters (`own_subs_`, `listeners_above_`), the
ACL `OWN_ACES` bit, the `#654` fork bit, the cold-extension pointer `ext_`, and — since #555 —
the publish itself when no waiter is parked.

---

## 3. Two independent limits, and which binds where

The ablation settles it. `has_registered_child` short-circuited to `return false`, tree otherwise
unchanged, 3 runs, medians, default slot:

| shape | T | stock | ablated | |
| --- | ---: | ---: | ---: | ---: |
| `stripe1-read` — distinct vertices | 8 | 21.3 M/s | 64.3 | 3.0× |
| `stripe1-read` | 24 | 19.7 | **165.3** | **8.4×** |
| `spread-read` — distinct vertices | 8 | 18.4 | 64.3 | 3.5× |
| `hot1-read` — one shared vertex | 24 | 1.74 | 1.65 | **0.94×** |
| `spread-fan0` — write | 8 | 35.4 | 35.4 | 1.00× (control) |

Two limits, and the second hid the first:

1. **The map lock capped every read** at ~20 M/s per process, whatever the topology. That
   `mutex`/`rwlock` calibration lands at 18.3–22.9 M/s aggregate at T=24 is not a coincidence —
   one blocking lock *is* the whole distinct-vertex read rate.
2. **On one shared vertex the map lock was never what bound.** Removing it changes nothing
   (0.94×). There the limit is the value's own reference count, and the `sp-load` calibration
   arm — 1.4 M/s at T=24 — accounts for nearly all of the 1.74 M/s stock rate.

The write path takes no map lock (`write_impl`, `graph.cpp:890`), which is the entire "writes
scale 5×, reads do not" asymmetry that looked mysterious for weeks.

**A caution on the calibration arms.** `sp-load` measures 710 ns/op at T=24 against a whole real
read of 574 ns — the arm is 1.24× the thing it explains. It is a tight loop with no work between
attempts, so it over-states: real work between lock attempts thins the retry storm. The same
applies to `rwlock` (54.7 ns) against the ablation's measured delta (44.6 ns). Treat both arms as
**upper bounds on a term**, never as the term.

---

## 4. What the two changes bought

`bench_lkv_slot graph`, four build combinations, medians, aggregate M ops/s.
n = 6 / 6 / 6 / 4 rounds respectively.

### One shared vertex (`lkvgraph_hot1-fan0-read`)

| T | stock | hazard slot | #654 | both | both/stock |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 21.13 | 18.70 | 31.59 | 23.99 | 1.14× |
| 8 | 2.19 | 5.20 | 2.36 | 7.59 | 3.48× |
| 16 | 1.86 | 5.69 | 1.86 | 9.84 | 5.29× |
| 24 | **1.74** | 7.35 | 1.78 | **15.15** | **8.69×** |

`#654` alone does nothing here, as §3 predicts. The hazard slot alone is 4.2×. Together, 8.69× —
because removing the lock exposes the slot win that the lock was masking.

### Distinct vertices (`lkvgraph_stripe1-fan0-read`)

| T | stock | hazard slot | #654 | both | best |
| ---: | ---: | ---: | ---: | ---: | --- |
| 8 | 21.26 | 16.13 | **67.92** | 65.32 | #654 |
| 16 | 22.10 | 15.32 | **122.72** | 103.21 | #654 |
| 24 | 19.74 | 17.08 | **163.48** | 161.14 | #654 |

163.5 against the ablation's 165.3 — **99% of the ceiling**, so at most 1% remains in any
reformulation of the fork check itself.

**And the hazard slot is no longer worth binding for this shape.** With the map lock gone its
pin overhead is exposed with no lock left to remove: `#654` alone beats `both` at every T from 2
up (35.0/34.1, 35.5/32.1, 67.9/65.3, 122.7/103.2, 163.5/161.1). The direction is consistent five
times out of five so the sign is real, but only the T=16 gap (19%) exceeds the run-to-run spread
of 1.16–1.29×; at T=24 it is 1.4%. **Bind `hazard_slot_t` only for genuine many-readers-on-one-
vertex**, which is narrower guidance than ADR-0069 shipped with.

---

## 5. The cost budget

One shared-vertex read at T=24 with the hazard slot, before #654 (136 ns system-wide measured).
Each term from the calibration, not fitted:

| term | ns | source |
| --- | ---: | --- |
| map lock — `shared_lock` + child walk | 54.7 | `rwlock` arm (upper bound; the ablation delta was 44.6) |
| promotion — `shared_ptr` copy in and out | 32.5 | `sp-copy` arm |
| rope copy — one segment refcount pair | 27.9 | `rmw2` arm |
| everything else | 6.0 | the ablated distinct-vertex read |
| **modelled** | **121** | vs **136 measured — 89% explained** |

The 11% unexplained is not the hazard pin's fence. That was measured separately and costs
**0.30 ns of 136** (0.2%): a `seq_cst` announce on a private line is per-thread work that 24
cores amortize, while every term above is serialized. A folly-style asymmetric fence
(`membarrier` on the reclaimer) would be Linux-specific, add a syscall per scan, and buy
nothing. **Dropped on measurement, not on taste.** This is the one number here from a scratch
file rather than a committed bench.

---

## 6. What is left, ranked

| lever | shape | now | ceiling | left | tracked |
| --- | --- | ---: | ---: | ---: | --- |
| — | distinct-vertex read | 163.5 M/s | 165.3 | **~0, exhausted** | — |
| scoped non-owning read | shared-vertex read | 15.15 M/s | ~70 | **~4–5×** | [#649](https://github.com/avatarsd-llc/libtracer/issues/649) |
| stripe lock on delivery | write / fan-out | — | — | ×16.6 at T=24 claimed | [#635](https://github.com/avatarsd-llc/libtracer/issues/635) |
| `find_ptr`'s map lock | path-addressed ops | unmeasured | — | unknown | not filed |

The slot itself is finished: the promotion is two contended RMWs (`sp-copy` 32.5 ns against
`rmw2` 27.9 — at the hardware floor), and the pin fence is 0.2%. Its remaining cost is entirely
its *contract*, which is what #649 changes rather than optimizes.

**Path-addressed operations are the unmeasured gap.** `find_ptr` takes the same lock this
document is about, and every number here avoids it by using handles. Nothing has quantified it.

---

## 7. Ledger of corrections

This area's causal claims have been wrong three times. Recorded because the *pattern* is the
lesson: each error was an inference from a curve, and each was settled by an ablation.

| # | Claimed | Actually | Settled by |
| --- | --- | --- | --- |
| 1 | The residual is `snapshot_edges`' stripe lock ([#635](https://github.com/avatarsd-llc/libtracer/issues/635)) | `snapshot_edges` is on the **delivery** path; `read` never calls it | reading the call graph |
| 2 | "Nothing process-wide is serializing — not the map lock" | Every read took `map_mutex_` shared | the §3 ablation |
| 3 | Distinct-vertex reads "retain 94%/91% of their T=1 rate", read as healthy | The arithmetic used the wrong shape's denominator (real figures 106%/96%), and retention of a T=1 *aggregate* is a serializer signature, not a health signature | recomputing it |

Two further habits this cost, both now standing rules: **quote a bench arm only after checking
its return type matches the real API's** (a non-owning model read was quoted as a 1806× win
against an achievable 20.8×), and **report the run-to-run spread beside every ratio** — on these
shapes it is 1.0–2.8× and it silently swallows anything under ~1.5×.

---

## 8. Diagnostic recipe

To decide which regime a workload is in, on your own machine:

1. `cmake -S bench -B bench/build -DCMAKE_BUILD_TYPE=Release && cmake --build bench/build -j`
2. `bench/build/bench_contention` — calibrate the host. Note `local`'s T24/T1: that is your
   real scaling ceiling, and it is **not** your logical core count.
3. `bench/build/bench_lkv_slot graph` — the real shapes. Compare `hot1` (one vertex) against
   `stripe1` and `spread` (distinct) at the same T.
4. Run each **at least three times, alternating** between the builds you are comparing, and take
   medians. Never rebuild between rounds. Report the spread.
5. Read the result against reference §3: aggregate rising ≈ regime (a); flat ≈ a serializer;
   falling ≈ a spin lock.
6. If a term is suspected, **ablate it** — short-circuit it in a scratch worktree and re-measure.
   Every correction in §7 came from an ablation and none came from reasoning about a curve.
