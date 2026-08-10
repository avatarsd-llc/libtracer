# Scaling and serialization

> **Host for every number below:** AMD Ryzen AI 9 HX PRO 375 — 12 physical cores / 24 SMT
> threads, 1 socket, 1 NUMA node, 24 MiB L3 in **two** instances. Release `-O3`, GCC 14,
> libstdc++. **Perfect scaling on this host is ~10× at T=24, not 24×.**

Scope: what synchronization costs in this C++ implementation, on that one host. Not the
standard. The implementation-independent obligations and the four hardware regimes are
[`../../reference/15-concurrency-and-scaling.md`](../../reference/15-concurrency-and-scaling.md)
§3, which every regime reference below points at.

Causal claims in this area have repeatedly survived plausible reasoning and failed ablation. §7
lists the checked cases and the rules they produce; read it before quoting any ratio from this
page.

---

## 1. Reading a scaling curve

A flat aggregate across thread counts is the signature of a **perfect serializer**, not of an
absent one. T threads producing one thread's total means each thread is T× slower. A *blocking*
lock plateaus rather than collapsing (reference §3 regime (c)), so nothing in the curve
distinguishes "no contention" from "one global lock" — both are flat, and the flat one that
looks healthy is the one to suspect. Only ablation separates them: short-circuit the suspected
term, re-measure, and read the delta.

The instance on this codebase. With the branch/leaf fork check taking `map_mutex_` shared,
`graph_t::read` on distinct vertices runs at ~19.7 M ops/s aggregate at 24 readers and ~18.7 M/s
at *one* reader. Flat at both ends. Short-circuiting that single check lifts the 24-reader figure
to 165.3 M ops/s (§3) — an 8.4× ceiling that the curve gives no hint of. The fork check answers
from a per-vertex bit and takes no lock (§2.1,
[#654](https://github.com/avatarsd-llc/libtracer/pull/654)).

---

## 2. The serializer inventory

Every synchronization point in the graph runtime, what it protects, and where it sits. Line
numbers are `core/src/graph.cpp` and `core/include/libtracer/vertex.hpp`.

### 2.1 `map_mutex_` — one `std::shared_mutex` per graph

Process-wide by construction: one graph, one mutex. Guards the Composite child links
(`children_->sorted`, appended under the unique lock, so an unsynchronized iteration can observe
a reallocation) and the `registered_` placeholder flag
([ADR-0057 — graph composite vertex tree](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0057-graph-composite-vertex-tree.md)).

The list below is every acquisition in `graph.cpp` — **18 sites in 17 functions**, which is what
`grep -n map_mutex_ core/src/graph.cpp` returns once its two comment hits (`:382`, `:535`) are
discounted. `evict_link_edges` is the one function that takes it twice.

| taken | where | frequency |
| --- | --- | --- |
| **unique** | `register_vertex_key` (`:309`), `retire` (`:488`), `collect` (`:520`) | control plane |
| shared | `find_ptr` (`:663-664`) — **so every `path_t` overload pays it once**; ≥3× and non-scaling (§6) | per op, path-addressed only |
| shared | `vertex_slot` (`:407`), `vertex_slot_at` (`:422`), `deref_vertex_slot` (`:448`), `vertex_slot_count` (`:396`) | per op, bound-path addressed only — see below |
| shared | `field_write` (`:1690`) — the `:acl` branch only, not every `:field` write | per `:acl` write |
| shared | `read_children` (`:1970`), `read_children_folded` (`:2073`), `read_subtree_folded` (`:2136`) | per composed read — these walk, so they need it |
| shared | `note_subscriber_added` / `_removed` (`:652`, `:658`), `evict_link_edges` (`:562`, `:567`), `has_first_level_child` (`:728`), `parked_seam_count` (`:529`) | control plane |

`retire_subtree` (`:352`) takes nothing of its own: it is called from inside `retire`'s unique
hold and recurses under it. The doc comment at `:535` states the same contract for the
`evict_link_edges` snapshot helper — it documents a required hold, it is not an acquisition.

**The RFC-0024 bound-path slot API is on this list, and it is not control plane.** Minting an
element takes the lock (`op_resolve_walk.hpp:803` → `vertex_slot`) and honouring one takes it
again (`op_resolve_walk.hpp:845` and `fwd_router.cpp:900` → `deref_vertex_slot`), so a bound-path hop pays
`map_mutex_` on both ends of the round trip that bound paths exist to make cheap. The two are not
the same cost: `vertex_slot` **scans `vertex_slots_` linearly** inside the hold, while
`deref_vertex_slot` and `vertex_slot_at` are a bounds check and one compare — the asymmetry
`graph.hpp:495-502` states in the header. The hold is not incidental in either: one shared
acquisition is what stops the slot index and the retire generation straddling a concurrent
`retire`, which is how an element gets stamped with the successor tenant's number.

The leaf/branch fork reads a per-vertex bit (`vertex_t::has_registered_child`,
`core/include/libtracer/vertex.hpp:1411`), called from `core/src/graph.cpp:959`, and takes no
lock. The symbol exists on the vertex rather than on the graph, so a reader grepping for it finds
a flag test rather than a lock acquisition.

A **handle**-addressed scalar read or write takes no map lock. A **path**-addressed one resolves
through `find_ptr` and pays it once, and a **bound-path**-addressed one pays it in
`deref_vertex_slot` instead. Every measurement on this page uses the handle overloads,
which `bench_lkv_slot` benches exclusively; the path-addressed figures are in §6 and are worse in
kind, not only in degree.

### 2.2 The vertex lock stripes — `kVertexLockStripes` mutexes, process-wide

The stripe count is an ordinary config constant shared through one header
([ADR-0068 — build configuration is plain C++](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0068-build-configuration-is-plain-cpp-config-header.md);
default 16, the sharing rationale at `vertex.hpp:997-1001`). The stripe is selected by
`vertex_stripe_of` (`:1079`) from the vertex address, hashed `(h >> 6) % kVertexLockStripes`
(`:1075`). The stripes guard the fan-out edge list, the STREAM ring, the write-sequence bump and
the ACL state. `add_edge`, `clear_edge` and `set_acl` take one; **`snapshot_edges` (`:2038`) no
longer does.** Delivery reads a published, immutable edge array under a bounded edge pin
instead — the stripe mutex left the publish path and kept the control plane
([ADR-0075](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0075-a-vertexs-edges-are-published-and-read-under-an-edge-pin.md)),
which is worth ×18.58 on the same-stripe fan-1 write at twenty-four threads. The zero-subscriber
gate stands in front of it either way: `fan_out` returns on a zero `own_subs_ordered()` first,
so an unobserved write — and every placeholder ancestor a bubble walks past — touches neither
the stripe nor the pin ([#635](https://github.com/avatarsd-llc/libtracer/issues/635)). Without
that gate the same-stripe write arm scaled negatively, because every placeholder ancestor took
a stripe.

The table has two realizations, chosen by whether the platform's `std::mutex` has a constexpr
constructor. Duplicated here from the configuration notes because a reader reasoning about
stripe-lock cost looks in this section:

| platform | table | cost |
| --- | --- | --- |
| host libstdc++ / libc++ | `inline constinit std::array<vertex_stripe_t, kVertexLockStripes> vertex_stripes` (`vertex.hpp:1057`) | none — constant-initialized |
| a target without a constexpr `std::mutex` | guarded function-local `static` (`:1065`) | one predicted branch per control-plane verb |

Two vertices that hash to the same stripe contend even though they share nothing else — which is
what the `stripe1` bench topology exists to measure.

### 2.3 The LKV slot — per vertex, policy-selected

`lkv_slot_t` is a compile-time policy (`core/include/libtracer/config.hpp.in:252`,
[ADR-0069 — LKV slot is a compile-time policy](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md)).
Two bindings ship:

| binding | mechanism | regime (reference §3) |
| --- | --- | --- |
| `sp_atomic_slot_t` (default) | `std::atomic<std::shared_ptr<const rope_t>>` | **(d)** — libstdc++ spins its `_Sp_locker` pointer-lock bit |
| `hazard_slot_t` | lock-free `atomic<node*>` + a hazard-pointer domain | **(b)** — one contended RMW for the promotion |

### 2.4 What has no lock

The write-sequence read, the subtree-listener counters (`own_subs_`, `listeners_above_`), the ACL
`OWN_ACES` bit, the branch/leaf fork bit, the cold-extension pointer `ext_`, and the publish
itself when no waiter is parked
([#555](https://github.com/avatarsd-llc/libtracer/pull/555)).

The **whole delivery decision for an unobserved write** is on this list too
([#635](https://github.com/avatarsd-llc/libtracer/issues/635)): two counter loads and no lock. One of them,
`own_subs_ordered()`, is read `seq_cst` rather than relaxed — it is the only read that decides
whether to deliver at all, so it pairs against a subscribe taking ADR-0049's latch. Every other
reader of the same counter stays relaxed, because deciding *how much* work to do can safely be
one publish behind; deciding *whether* cannot.

---

## 3. Two independent limits, and which binds where

Ablating the fork check isolates the map lock's share. `has_registered_child` short-circuited to
`return false`, tree otherwise unchanged, 3 runs, medians, default slot:

| shape | T | stock | ablated | |
| --- | ---: | ---: | ---: | ---: |
| `stripe1-read` — distinct vertices | 8 | 21.3 M/s | 64.3 | 3.0× |
| `stripe1-read` | 24 | 19.7 | **165.3** | **8.4×** |
| `spread-read` — distinct vertices | 8 | 18.4 | 64.3 | 3.5× |
| `hot1-read` — one shared vertex | 24 | 1.74 | 1.65 | **0.94×** |
| `spread-fan0` — write | 8 | 35.4 | 35.4 | 1.00× (control) |

The 0.94× row and the 1.00× control are what make the 8.4× a lock rather than an artifact: a
change that lifts distinct-vertex reads eightfold while moving the shared-vertex read and the
write not at all is acting on exactly one term.

Two limits, and the second hides the first:

1. **The map lock caps every read** at ~20 M/s per process, whatever the topology. That the
   `mutex`/`rwlock` calibration lands at 18.3–22.9 M/s aggregate at T=24 is not a coincidence —
   one blocking lock *is* the whole distinct-vertex read rate.
2. **On one shared vertex the map lock is not what binds.** Removing it changes nothing (0.94×).
   There the limit is the value's own reference count, and the `sp-load` calibration arm —
   1.4 M/s at T=24 — accounts for nearly all of the 1.74 M/s stock rate.

The write path takes no map lock (`write_impl`, `graph.cpp:1179`), which is the entire "writes
scale 5×, reads do not" asymmetry.

**A caution on the calibration arms.** `sp-load` measures 710 ns/op at T=24 against a whole real
read of 574 ns — the arm is 1.24× the thing it explains. It is a tight loop with no work between
attempts, so it over-states: real work between lock attempts thins the retry storm. The same
applies to `rwlock` (54.7 ns) against the ablation's measured delta (44.6 ns). Treat both arms as
**upper bounds on a term**, never as the term.

---

## 4. Measured cost of the map lock and of the slot binding

`bench_lkv_slot graph`, four build combinations, medians, aggregate M ops/s.
n = 6 / 6 / 6 / 4 rounds respectively.

### One shared vertex (`lkvgraph_hot1-fan0-read`)

| T | stock | hazard slot | map lock removed | hazard slot + map lock removed | ratio to stock |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 21.13 | 18.70 | 31.59 | 23.99 | 1.14× |
| 8 | 2.19 | 5.20 | 2.36 | 7.59 | 3.48× |
| 16 | 1.86 | 5.69 | 1.86 | 9.84 | 5.29× |
| 24 | **1.74** | 7.35 | 1.78 | **15.15** | **8.69×** |

Removing the map lock alone does nothing on this shape, as §3 predicts. The hazard slot alone is
**4.2× at T=24** (7.35 against 1.74). Together, 8.69× — removing the lock exposes the slot win the
lock masks.

### Distinct vertices (`lkvgraph_stripe1-fan0-read`)

| T | stock | hazard slot | map lock removed | hazard slot + map lock removed | best |
| ---: | ---: | ---: | ---: | ---: | --- |
| 8 | 21.26 | 16.13 | **67.92** | 65.32 | map lock removed |
| 16 | 22.10 | 15.32 | **122.72** | 103.21 | map lock removed |
| 24 | 19.74 | 17.08 | **163.48** | 161.14 | map lock removed |

Both tables are the map-lock removal ([#654](https://github.com/avatarsd-llc/libtracer/pull/654))
crossed with the `hazard_slot_t` binding.

163.5 against the ablation's 165.3 — **99% of the ceiling**, so at most 1% remains in any
reformulation of the fork check itself.

**The hazard slot is not worth binding for this shape.** With the map lock off the path its pin
overhead is exposed and there is no lock left for it to remove: map-lock-removed beats the pair at
every T from 2 up (35.0/34.1, 35.5/32.1, 67.9/65.3, 122.7/103.2, 163.5/161.1). The direction is
consistent five times out of five so the sign is real, but only the T=16 gap (19%) exceeds the
run-to-run spread of 1.16–1.29×; at T=24 it is 1.4%. **Bind `hazard_slot_t` only for genuine
many-readers-on-one-vertex.** As guidance that is narrower than "bind it for read-heavy work":
read-heaviness is not the axis — sharing of a single vertex is.

**Reference-returning reads.** A read of a published value returns a reference to it rather than a
copy: 1.48× median across shapes, 95 of 102 paired samples favouring it, and 3.07× on distinct
vertices at T=8 ([#661](https://github.com/avatarsd-llc/libtracer/pull/661)).

**Forward demux.** The router's mount scan is not on the graph read path but shares the same
measurement discipline: a per-slot name digest cuts the marginal scan cost from 333 ns to 17 ns at
64 links ([#660](https://github.com/avatarsd-llc/libtracer/pull/660)). Its *fixed* cost is the
more instructive half — §7, rule 4. The delivery side of the runtime is described in
[`01-write-and-delivery-path.md`](01-write-and-delivery-path.md).

---

## 5. The cost budget

One shared-vertex read at T=24 with the hazard slot and the map lock on the path, 136 ns
system-wide measured. Each term comes from the calibration, not from a fit:

| term | ns | source |
| --- | ---: | --- |
| map lock — `shared_lock` + child walk | 54.7 | `rwlock` arm (upper bound; the ablation delta was 44.6) |
| promotion — `shared_ptr` copy in and out | 32.5 | `sp-copy` arm |
| rope copy — one segment refcount pair | 27.9 | `rmw2` arm |
| everything else | 6.0 | the ablated distinct-vertex read |
| **modelled** | **121** | vs **136 measured — 89% explained** |

The 11% unexplained is not the hazard pin's fence. That was measured separately and costs
**0.30 ns of 136** (0.2%): a `seq_cst` announce on a private line is per-thread work that 24 cores
amortize, while every term above is serialized. The alternative — a folly-style asymmetric fence,
`membarrier` on the reclaimer — would be Linux-specific, add a syscall per scan, and buy nothing.
Rejected on the measurement above.

Provenance: a scratch harness, not a committed bench — the only such number on this page.

---

## 6. Remaining serializers on the read path

### The branch/leaf fork

Exhausted. The distinct-vertex read reaches 163.5 M/s against the ablation's 165.3 M/s ceiling —
within 1% of the ablation ceiling; no reformulation of the fork check can recover more.

### `find_ptr`'s map lock — path-addressed operations

**A path-addressed read is a serializer, not a slower read.** It is 8.23× slower than a
handle-addressed read at T=24 and does not scale with cores at all: 15.4 M/s at one thread falling
to 12.8 at twenty-four. Ablation attributes roughly three quarters of that to `find_ptr`'s
`map_mutex_`; the rest is the walk itself. The lock's own magnitude is **≥3× always** — ~13–16× on
a quiet host, ~4–5× oversubscribed — and it costs +8–17 ns/op even at T=1
([#635](https://github.com/avatarsd-llc/libtracer/issues/635)).

Removing it is not a local edit: the shared hold is what excludes concurrent vertex creation
during the walk. A count of 11–12 ThreadSanitizer-reported races with the lock removed circulates
without a named build, shape set or test list, and is **not verified here**. The check that
settles it: the CI ThreadSanitizer configuration — `-fsanitize=thread -g -O1`,
`CMAKE_BUILD_TYPE=Debug`, both `LIBTRACER_LKV_SLOT` bindings, `ctest` over `core/`
(`.github/workflows/core-ci.yml:113-124`) — rebuilt with `find_ptr`'s `shared_lock` removed,
recording each reported race site rather than a count.

### Two approaches that do not work

Depth is nearly free under contention: `find` is 17.3 M/s at three segments and 17.7 at eight, so
caching a path prefix buys ~1.5× single-threaded and ~0 at T=24.

Porting the append-only connection-table container to `children_` — the obvious move, since the
precedent looks exact — **regresses**: 0.30× at 1,024 siblings and 0.078× at 4,096, because that
container replaced a binary search with a linear scan, which is right for tens of links and wrong
for a wide composite. Crossover is around 256 siblings at T=24. Any real fix needs a chunked
structure that keeps an index, not a port
([ADR-0063 — connection table, lock-free reads](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0063-connection-table-lock-free-reads-trait-serialized-writes.md)).

### The stripe lock on delivery

×16.6 at T=24 on the adversarial shape (many vertices hashing to one stripe) and 1.02× on the
realistic one. Both halves belong together or neither does: quoting the 16.6× without the 1.02×
describes a topology nobody deploys, and quoting the 1.02× alone hides a real cliff
([#635](https://github.com/avatarsd-llc/libtracer/issues/635)).

### The scoped non-owning read

20–60× on the two INTERNAL legs whose value never escapes the call. The magnitude depends on the
value never outliving the scope, which is why it is confined to those two legs
([#649](https://github.com/avatarsd-llc/libtracer/issues/649)).

### The slot promotion floor

The promotion is two contended read-modify-writes: `sp-copy` 32.5 ns against `rmw2` 27.9 ns — at
the hardware floor for the shape. The pin fence is 0.2% (§5). There is nothing left in the slot
itself.

---

## 7. How measurement goes wrong here

Reasoned magnitudes in this area fail in a consistent direction: they overstate the value of the
work being proposed. Every row below was settled by an ablation, a recomputation or a paired
re-measurement — never by further reasoning about a curve.

| A plausible claim | What checking shows | The check that decides it |
| --- | --- | --- |
| The read-path residual is `snapshot_edges`' stripe lock | `snapshot_edges` (`vertex.hpp:2038`) is on the **delivery** path; `read` never calls it — and since ADR-0075 it takes no stripe lock at all | reading the call graph |
| "Nothing process-wide is serializing — not the map lock" | Every read acquired `map_mutex_` shared through the fork check — the one lock the claim named | the §3 ablation |
| Distinct-vertex reads "retain 94%/91% of their T=1 rate", read as healthy | The arithmetic used the wrong shape's denominator — real figures 106%/96% — and retention of a T=1 *aggregate* is a serializer signature, not a health signature | recomputing it |
| Only a config traits template can recover the stripe table's 896 B, "because the alignment is part of the type" | The *count* cannot reach the alignment; the **alignment itself is a config constant**. One `constexpr` and one token recover the identical 896 B, zero templates | building it both ways on rv32 |
| Unlocking `find_ptr` is worth **15.9×** | **≥3× always**, but 10.2× / 16.4× / 7.5× / 5.4× across four sessions — it tracks idle CPU, because the base is lock-bound and load-insensitive while the ablation is CPU-bound | four re-runs on a varying host |
| Porting the append-only connection-table container to `children_` buys 7.33× on all 13 `find_ptr` sites | **Regresses**: 0.30× at 1,024 siblings, 0.078× at 4,096. The precedent replaced a binary search with a linear scan — right for tens of links, wrong for a wide composite | a sibling-width sweep |
| Returning the published value by reference is worth **2.1×** at T=24 | **1.27×** there; 1.48× median across all shapes. Real, and smaller than claimed | both arms alternating in ONE binary |
| `hazard_slot_t` is the general read-path win | 4.2× on one shared vertex at T=24; a loss on distinct vertices with the map lock off the path (163.5 against 161.1 at T=24) | running both shapes, not only the shape the policy targets |

Four rules follow, and they apply to any measurement on this page:

1. **Quote a bench arm only after checking its return type matches the real API's.** A non-owning
   model read was quoted as a 1806× win against an achievable 20.8×.
2. **Report the run-to-run spread beside every ratio.** On these shapes it is 1.0–2.8×, and it
   silently swallows anything under ~1.5×.
3. **Prefer both arms in ONE binary over two builds.** Alternating two *builds* leaves layout,
   allocator state and thermal drift in the comparison, and those are the same size as the effect
   being measured.
4. **An optimization's fixed cost is part of the measurement.** The first mount digest
   ([#660](https://github.com/avatarsd-llc/libtracer/pull/660)) removed 91% of the scan cost and
   added ~30 ns — a fifth of a whole forward hop — to *every* lookup. Measuring only the axis
   being improved hid it completely.

---

## 8. Diagnostic recipe

To decide which regime a workload is in, on your own machine:

1. `cmake -S bench -B bench/build -DCMAKE_BUILD_TYPE=Release && cmake --build bench/build -j`
2. `bench/build/bench_contention` — calibrate the host. Note `local`'s T24/T1: that is your real
   scaling ceiling, and it is **not** your logical core count.
3. `bench/build/bench_lkv_slot graph` — the real shapes. Compare `hot1` (one vertex) against
   `stripe1` and `spread` (distinct) at the same T.
4. Run each **at least three times, alternating** between the builds you are comparing, and take
   medians. Never rebuild between rounds. Report the spread.
5. Read the result against reference §3: aggregate rising ≈ regime (a); flat ≈ a serializer;
   falling ≈ a spin lock.
6. If a term is suspected, **ablate it** — short-circuit it in a scratch worktree and re-measure.
   An ablation decides; a curve does not.
