# Allocation-store composition defaults to per-plane (MID), injected per target

Status: **accepted** (2026-08-15; drafted 2026-08-06 and merged 2026-08-06 as [PR #940](https://github.com/avatarsd-llc/libtracer/pull/940), for [#873](https://github.com/avatarsd-llc/libtracer/issues/873)), **amended 2026-08-20** — the title's "defaults to per-plane (MID)" is **withdrawn**, and the `NARROW` / `MID` / `WIDE` spelling used for composition throughout the body is **retired** in favour of **folded / per-plane / per-thread**. Read the whole document below through §Amendment (2026-08-20), at the end; the filename and the body text stand as the record of what was decided in August 2026.

> **The status line lagged the facts.** This ADR read `proposed` from its merge until 2026-08-15
> even though PR #940 had merged, the composition below was the one being scheduled against, and
> shipped code cited it by number (family 1, [#1287](https://github.com/avatarsd-llc/libtracer/issues/1287),
> named ADR-0079 in `core/CHANGELOG.md`). Ratifying it here is bookkeeping catching up with
> practice, not a new decision.

> **This ADR's `file:line` citations are maintained LIVE — do not freeze them.**
> `tools/check_doc_citations.py` deliberately leaves `docs/adr/` unenrolled, because an ADR is
> normally a *dated record* that cites the tree as it stood and pinning it would demand rewriting
> history on every refactor. This ADR is the standing exception: it is **actively being built
> against** — §Staging below is the live programme, and implementers read these citations to find
> the seam they are about to move. A stale citation in a live design document misleads every one
> of them (the pin for the throwing `std::pmr` seam had drifted to an unrelated comment before
> 2026-08-15). So the citations here are re-pinned by hand whenever the cited lines move, and are
> **hand-checked, not gate-checked** — the gate cannot see them. Re-pin by reading the cited
> line's *text* out of the revision the citation was written against and locating that exact text
> in the current tree; never by arithmetic on the line number. When §Staging is complete and this
> ADR becomes a historical record again, this note and the maintenance obligation retire with it.

The failable/placement allocation store (the `block_source_t` seam of [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md), extended to be the one substrate #873 asks for) is composed **per plane by default**: one **placement** store for the graph (L4) and one **failable** store for the net plane, with the payload-segment backend (`mem_backend_t`, need C) left entirely separate. The composition is an **injected, build-time choice**: a single-core MCU folds the two into one store ("one slab, whole stack"); a many-core host fans the net-plane store out **per receive thread**. Neither the throwing `std::pmr` default resource nor the un-injected global heap remains on any hot path.

## Context

#873 found one concern — "give me bytes, fail by value / with controlled placement" — served through **four** channels with four conventions: `std::pmr` (throws — `graph.hpp:353`, the `mr` argument), `mem_backend_t` (refcounted segment), `block_source_t` (raw block, fails by value — ADR-0065), and the **un-injected global heap** (hazard nodes, child-registry chunks, every `try_reserve`/`try_push_back`). Injecting bounded sources at three seams still does not bound the process, because channel four bypasses all of them.

There is no nothrow `std::pmr` to reach for — the `memory_resource` interface throws by contract in C++23 and C++26 alike, which is why ADR-0065 built `block_source_t` in the first place. So the substrate is a **custom failable store**, not a `memory_resource` wrapper.

The three real needs are distinct and must not be collapsed into one:
- **A — failable** (returns null, never aborts): anything a peer can provoke. `block_source_t`.
- **B — placement/latency** (deterministic, no `malloc`-chunk luck): hot control-plane objects, chiefly `vertex_t` (#843). A pool over the same store.
- **C — payload bytes / DMA** (refcounted, cache-coherent, shared by views/ropes): `mem_backend_t` + `segment_t`. **Out of scope of the unification** — segments carry refcounts and DMA cache hooks; §289/§290 already forbid treating backend and block-source as redundant twins.

### The composition spectrum, and why it is a dual-target tension

How many stores serve A+B is the load-bearing choice, and the two targets pull opposite ways:

| point | shape | host (many-core) | MCU (single-core) |
| --- | --- | --- | --- |
| **WIDE** | one node-wide store | ✗ graph + N RX threads contend one lock | ✓ single-threaded, no lock; tightest RAM |
| **MID** | graph store + net-plane store (+ segments separate) | ✓ planes don't share a lock | ✓ folds to WIDE by injection |
| **NARROW** | per-thread / per-connection | ✓✓ zero contention | ✗ ~14 KB wasted on per-store slack |

The figures behind the ✗/✓ (measured = this repo's own benches; est = allocator physics):
- **Contention.** A store shared by the graph write thread and the transport RX threads is the same shape as the stripe lock that collapsed **×16.6 throughput at 24 threads** [measured, #635 / LKV contention]. That is the case against WIDE on a host.
- **Placement.** A pooled/arena alloc is ~3–8 ns with no variance [est] vs `malloc` ~20–80 ns [est] carrying a **±2.9×** swing from the glibc chunk class [measured, #843]. Placement is the B win.
- **Footprint.** Each store's fixed reserve+headers ≈ low-single-KB [est]; on an ESP32-C6 (tens of KB free at runtime) NARROW's ~8 stores × ~2 KB ≈ **~14 KB** over WIDE's ~2 KB — real budget there, noise on a host.
- **Bounded-node.** WIDE = one cap, bursts borrow idle space → tightest total for a given safety margin; NARROW = N caps that cannot borrow → their sum ≥ the WIDE cap for equal safety.
- **Blast radius.** Every peer-provoked-abort issue (#603, #848, #872, #909–912, #934) is a hostile peer driving allocation. Under WIDE one flood exhausts the graph too (a peer stalls control-plane vertex creation by spamming the net plane); under MID/NARROW the net-plane failable store is **fenced off** — the flood hits its own cap and backpressures while the graph runs on. This is a security property, not only performance.

## Decision

1. **Default composition is MID (per-plane):** a graph **placement** store (need B, plus the graph's own failable growth), a net-plane **failable** store (need A), and `mem_backend_t` segments (need C) separate. MID is the default because it is the one point where **neither target pays the other's cost**, and it draws the isolation boundary exactly where the peer-provoked risk lives.
2. **Composition is injected, build-time.** MCU folds the graph and net-plane stores into one (§289's "one slab") — recovering the ~14 KB and a single cap. Host fans the net-plane store **per receive thread** — recovering the contention win — with the graph never contending with transports.
3. **Both stray channels are removed from hot paths.** The throwing `std::pmr` at `graph.hpp:353` and the un-injected global heap (`graph.cpp`, `fwd_router.cpp`, `route_handle.cpp`, `lkv_slot.hpp`, `child_registry.hpp`) draw from the substrate instead. `std::pmr` survives only as a thin adapter for std-container interop on **non-failable** paths.
4. **"Bounded node" is a property the deployer injects,** not one the library fixes — it is delivered by sizing the injected store(s), and it is now actually deliverable because no allocation escapes to the global heap.

## Building each configuration

Composition is chosen entirely by **which `block_source_t` instances the deployer constructs and wires** — into `graph_t`'s `ctl` seam (`core/include/libtracer/graph.hpp:355`, the third argument of the constructor declared at `:353`) and into each transport factory. No core edit selects a configuration; it is deployment wiring, closed at build time. The `std::pmr::memory_resource* mr` ctor argument is demoted to the std-container adapter role on non-failable paths (or dropped); `ctl` is the primary failable seam.

The block-source flavours are the tuning surface: `heap_source_t` (unbounded, wraps global `new` — hosts that accept an unbounded seam), `pool_source_t` (recycling, the long-lived bounded seam), `bump_source_t` (a hard buffer bound). Sketches (illustrative — exact transport-factory argument lands with the implementation):

```cpp
// WIDE — one store, one cap. MCU default (single-threaded ⇒ contention-free).
static pool_source_t slab{one_arena};            // or bump_source_t over the single slab
graph_t g{/*mr adapter*/, value_backend, &slab};
register_ws_server(g, /*ctl=*/&slab, ...);       // every transport points at the same store
register_can(g,       /*ctl=*/&slab, ...);

// MID — per-plane (DEFAULT). graph and net plane do not share a lock;
// a peer flooding the net plane cannot exhaust the graph's store.
static pool_source_t graph_ctl{graph_arena};
static pool_source_t net_ctl{net_arena};
graph_t g{/*mr adapter*/, value_backend, &graph_ctl};
register_ws_server(g, /*ctl=*/&net_ctl, ...);    // net plane shares one failable store
register_can(g,       /*ctl=*/&net_ctl, ...);

// NARROW — per-RX-thread. Host default (kills the shared-lock contention).
static pool_source_t graph_ctl{graph_arena};
graph_t g{/*mr adapter*/, value_backend, &graph_ctl};
// each transport constructs a thread_local pool_source_t on its own RX thread
// and passes &that as its ctl — no store is touched by two threads.
```

The composition is a property the deployer *sizes* (the arenas above) — "bounded node" is delivered by these caps, not fixed by the library. `value_backend` (need C — segments/DMA) is a separate argument in every configuration and is never folded into `ctl`.

## Staging — how the composition is actually built out

The composition above is the *shape*; this section is the *programme*, and it is normative ADR
text rather than a triage remark. It consolidates the three [#873](https://github.com/avatarsd-llc/libtracer/issues/873)
rulings that fixed it — the staging order (2026-08-12), the scope ruling and family order
(2026-08-14), and the widening to the whole `tr::net` plane (2026-08-14). Until 2026-08-15 the
word "stage 1" appeared nowhere in this ADR while four issues were scheduled against it, which is
why it lives here now.

### 1. The substrate already exists; the work is WIRING

The remaining work is moving existing allocation sites onto an existing substrate, not building
one. `block_source_t` (`core/include/libtracer/mem_source.hpp:69`) already has four production
implementations — `heap_source_t` `:116`, `null_source_t` `:148`, `bump_source_t` `:184` and
`pool_source_t<Sync>` `:316`, the bounded recycling arena already shipping on ESP per
[ADR-0067](0067-bounded-recycling-source-and-per-owner-topology.md) — plus `block_array_t<T>` `:445` and
`tlv_arena_t` (`core/include/libtracer/tlv_arena.hpp:75`). Three seams already take an injected
source with a `heap_source()` default: `graph_t`'s `ctl`
(`core/include/libtracer/graph.hpp:355`), `fwd_router_t`'s `rx`
(`core/include/libtracer/fwd_router.hpp:179`) and `iov_table_t`
(`core/include/libtracer/iov_table.hpp:90`). That is why this can be scheduled as families rather
than as an epic.

### 2. Stage order

1. **Stage 1 — the net-plane failable store.** Lands first: it unblocks #603 defect 1, #934's
   handshake bound and #981's migration destinations. This is the `tr::net` family programme in
   §4 below.
2. **Stage 2 — the graph placement store.** #843's fix (hot-`vertex_t` placement), gated on
   #1285's diagnosis landing first.

   **Condition added 2026-08-15 (#1285, ruled): the store must specify *alignment*, not merely
   placement.** The diagnosis landed, and it binds this stage: the hot-vertex arm is a function
   of the object's address **mod 64** — `vertex_t`'s 16-byte `lkv_` atomic straddles a cache-line
   boundary at one of glibc's four 16-byte-aligned placements — so a placement store that hands
   out `sizeof(vertex_t)`-byte objects at a `sizeof`-stride rotates through the same four offsets
   `malloc` does and buys nothing. Concretely: (a) the straddle itself is already unreachable
   after #1330's member reorder (`offsetof(lkv_) % 16 == 0`, `static_assert`-pinned) *provided
   the store never places at sub-16-byte alignment* — 16-byte alignment is the stage-2 floor,
   not an optimisation; (b) the remaining ~1.5× (#1285 rec 2) requires **64-byte-aligned**
   placement, which additionally lifts `own_subs_`/`listeners_above_` off the contended line —
   that is the RAM-for-latency trade this stage exists to price, and its A/B must report the hot
   vertex's `mod 64` offset so a mechanism change can be told from a placement re-roll.

### 3. Stage-1 scope — `tr::net` only, MECHANISM not POLICY

| Plane | In scope? | Why |
| --- | --- | --- |
| `tr::net` | **Yes** | The target of stage 1 |
| `tr::wire` (`tlv_arena_t`) | Already done | `fwd_router` decodes through the injected `rx_` (#168–#170) |
| `tr::view` (`rope_t::try_reserve`) | **No** | Called from three hot net sites, but migrating it is an L1 API change |
| Graph placement store (#843) | **No — stage 2** | See above |
| LKV / hazard (`lkv_slot.hpp`) | **No — blocked** | A naive fix adds an atomic to `store()` (#897); needs [ADR-0080](0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md)'s single-threaded escape |
| `mem_backend_t` / segments (need C) | **No** | This ADR keeps need C separate deliberately |

Bounding is included as **mechanism** and excluded as **policy**. Once a site draws from an
injected `block_source_t` the cap *is* the store's size — which is §Decision 4's "bounded node is
a property the deployer injects", and satisfies #934's own no-synthetic-limits requirement. What
stays out of stage 1 is **what a site does when the store refuses** (drop / backpressure / close
the peer — a per-site call), and whether the WS handshake is bounded or made nothrow, which #934
keeps.

### 4. Family order

No `tr::net` site is deferred; the plane is taken whole.

1. **The two hardcoded `heap_source()` defaults** — the base `transport_t::send(iov)` gather and
   `iov_table_t`'s overflow (tcp, udp, ws call sites). **LANDED** as
   [#1287](https://github.com/avatarsd-llc/libtracer/issues/1287) — `net::transport_t::egress_source()`
   / `set_egress_source()` (`core/include/libtracer/transport.hpp:468`, `:488`).
2. **`fwd_router`'s nothrow delivery/gather `try_*` sites** — already failable; a mechanical move
   to `block_array_t`.
3. **`route_handle` (#603 defect 1)** — carries the API change the code itself names: `link_tables_t`
   holds `std::pmr::vector`s while `detail::try_*` is typed to plain `std::vector`. Scoped openly
   as an API change rather than pretended small.
4. **`transport_webtransport`'s guarded sites.**
5. **CAN `pending_` / `can_reassembly`** — the remaining `std::pmr` throwers.
6. **`child_registry` chunks** — already `new (std::nothrow) chunk_t`, merely un-injected; the
   cheapest family. Its hot path is allocation-free by design and pinned by `bench_forward_heap`'s
   `allocs=0` gate — that must not be disturbed.
7. **Transport accept / session setup** — `posix_endpoint`'s `slots_` and poll arrays,
   `transport_ws`'s per-session reassembly and handshake buffers. **`ready-for-human`**, because
   these allocations are **connection admission**: once they draw from a bounded store, a refusal
   means refusing a connection, and the refusal policy (close immediately / accept-and-close on
   first use / treat it as the existing `max_peers` bound) is an unmade decision. It must reuse
   #838's shape — count it, then act — rather than inventing a second refusal vocabulary.
8. **Control-plane string/vector churn** — `fwd_router`, `transport_vertex`, `route_handle` encode
   buffers, `transport_can`. Several of these are reachable from a peer CREATE on an RX thread, so
   "control plane" does not mean "local only".
9. **quic + webtransport unguarded sites** — sequenced AFTER #1286's audit lands; that site list is
   one grep's worth and is explicitly not claimed complete.

Per-site `file:line` detail for families 2–9 is deliberately NOT reproduced here: it is a
hundred-odd pins that would rot faster than this document is read, and it lives in #873's
2026-08-14 rulings. The seams and landed surfaces above are pinned because implementers navigate
by them.

### 5. Cadence

**One site family per PR**, each carrying its own pinned A/B plus an A/A null. Any site that
reintroduces a #897-style atomic in `store()` is a **reject**. A regression on any read/write hot
path is a **reject**. Families 1–6 and 8–9 are agent-eligible; **family 7 is `ready-for-human`**
for the admission-policy reason above. #873 itself stays parked as the umbrella parent.

## Verification — per-configuration CI benches

The three configurations have **different, measurable** cost profiles, so each must be benched, and the numbers this ADR rests on must be reproducible in CI rather than asserted once:

- **A three-configuration sweep** (WIDE / MID / NARROW) over the same workload, reporting per config: **alloc hot-path latency**, **fan-out throughput at increasing thread counts** (the arm that exposes WIDE's shared-lock contention — cf. the ×16.6 collapse at T=24), and **peak resident / store high-water-mark** (the arm that exposes NARROW's per-store slack on the constrained target).
- The sweep is a standing bench so a regression in any configuration is visible, and so a future change to the composition default is argued from CI numbers, not memory. It must bracket the thread counts that matter (single-thread MCU shape **and** ≥ T where WIDE diverges — cf. the #844 gap, where nothing between fan-1 and fan-1024 let a wide-fan regression through).
- The per-store-slack claim (~14 KB on the MCU target) must be produced by a real HWM census under NARROW vs WIDE, not left as an estimate.
- Non-vacuity: a deliberately mis-wired WIDE build under a multi-thread fan-out must show the contention the MID/NARROW builds do not — an arm that cannot redden on the known-bad composition is not a guard.

This bench work is tracked separately (blocked on the substrate itself existing) and is a landing gate for the composition, not optional tooling.

### The instrument exists (2026-08-20, [#941](https://github.com/avatarsd-llc/libtracer/issues/941))

`bench/bench_store_sweep.cpp` + `bench/bench_store_escape.cpp`, driven by `bench/run_store_sweep.sh` and collated by `bench/collate_store_sweep.py`. It sweeps **four** arms — an `H-baseline` control (the shipped all-heap default) plus WIDE / MID / NARROW — over one workload that draws all four channels above, and reports latency by leg, fan-out throughput across T = {1, 2, 4, 8, 16, 24}, deterministic store high-water, and what still escapes to the process heap. The banked run is in `bench/README.md`. Three things about it belong here rather than only there:

- **Its graph arm is Stage-1 only.** `vertex_t` placement is still `std::make_unique<vertex_t>` on the global heap — that is Stage 2 ([#843](https://github.com/avatarsd-llc/libtracer/issues/843), gated on [#1285](https://github.com/avatarsd-llc/libtracer/issues/1285)) and it has not landed. What the sweep varies on the graph side is the descent stacks (`ctl`) and the `std::pmr` control-block channel (`mr`, through the [#1401](https://github.com/avatarsd-llc/libtracer/issues/1401) adapter). The consequence is measured: injecting the whole substrate moves ~104 B of node construction off the process heap, and the escape is non-zero in every arm.
- **The non-vacuity clause is half-confirmed and half-contradicted.** WIDE does show the predicted collapse under a multi-thread fan-out (0.01x of its own single-thread rate by T = 24). So does **MID** — its net-plane store is one `pool_source_t<sync_mutex_t>` shared by every receive thread, which is the same shape at a different granularity. Only NARROW tracks the platform heap's scaling curve. On the whole-node leg every injected arm collapses, because the graph plane is one locked store in all three.
- **Half of it is a standing series; the other half never can be.** The DETERMINISTIC columns — store `used()` (and the store count it is spread over) per (arm, T), and the T = 1 latency cell per (arm, leg) — are banked on every `main` push by `perf-local.yml` and warn-ratcheted against measured pins in `bench/store_sweep_pins.json` ([#1428](https://github.com/avatarsd-llc/libtracer/issues/1428)). That is what satisfies "a standing bench so a regression in any configuration is visible". The fan-out T-sweep and the process-heap escape high-water are **excluded by measurement**: the unpinned throughput window's own A/A null read 58.9 % where the same run's pinned window read 0.87 %, every thread-contention bench in the tree already declines the gate on that ground, and `bench/ram_census_pins.json` records a high-water swinging 66 % across runs. Both exclusions are enforced rather than documented — `store_sweep_gate.py` fails on a pin naming either column. The one growth already priced is Stage 2 ([#843](https://github.com/avatarsd-llc/libtracer/issues/843)): it will step every `used_total` pin, and the pin file pre-declares that so the series cannot flag the fix as the regression.

## Considered options

- **WIDE as default.** Rejected: the ×16.6 host contention and the single shared blast radius. Kept as the MCU *fold* of MID, chosen by injection where it is free.
- **NARROW as default.** Rejected: ~14 KB of per-store slack on the RAM-critical target and the largest bounded-node sum. Kept as the host *fan* of MID, chosen by injection where RAM is free.
- **Keep the four channels, only add `[[nodiscard]]`/close the worst aborts.** Rejected: treats the symptoms (#603/#930) without making "bounded node" a real property — the global-heap bypass remains, so a bounded deployment still is not bounded.
- **Fold need C (segments) in too.** Rejected: segments carry refcounts and DMA cache hooks and are shared across views; they have a different contract, and §289/§290 already rule them not-redundant with the block source.

## Consequences

- The substrate is designed as a **per-store injectable** with MID wired by default; a target selects WIDE (fold) or NARROW (fan) without touching core — the [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) module-set discipline (types close at build time, instances stay runtime).
- Every peer-provoked allocation is fenced in the net-plane store; a flood backpressures locally instead of aborting or starving the graph.
- Reclamation is unchanged: this ADR is about *where bytes come from*, not *when a replaced block is freed* — the per-tenant reclamation of [ADR-0072](0072-one-reclamation-domain-graph-owned-and-backend-injected.md) §Supersession and the open edge-array question (#894/#635) stand untouched.
- Implementation is **staged and latency-gated** — the programme is §Staging above: each site family moved onto the substrate carries its own pinned A/B; a regression on any read/write hot path is a REJECT. The hot-vertex placement (#843) is the first concrete beneficiary; the net-plane failable moves (#603, #848, #930) are the isolation beneficiaries.
- The composition default and the WIDE/NARROW folds should be stated in the configuration-space doc so a deployer knows which knob sizes their node.

## Amendment (2026-08-20): the composition triad is renamed, and no composition is the default

Ruled by the maintainer on 2026-08-20 against the banked
[#941](https://github.com/avatarsd-llc/libtracer/issues/941) sweep
([PR #1427](https://github.com/avatarsd-llc/libtracer/pull/1427), pins in
`bench/store_sweep_pins.json`), closing
[#1429](https://github.com/avatarsd-llc/libtracer/issues/1429). Two things change: **what the
three points are called**, and **the claim that one of them is the default**. Nothing about the
substrate, the injection seams, the §Staging programme, or the separation of need C changes.

### 1. The universal-default claim is withdrawn

§Decision 1 ("Default composition is MID") and the title's "defaults to per-plane" are
**withdrawn**. No composition is *the* default, because there is no one target to default for.
What ships is neither of the three: every allocation seam in the tree — a dozen-odd of them —
takes an injected `mem::block_source_t` **defaulting to `&mem::heap_source()`**, which is the
sweep's `H-baseline` arm and the behaviour of every un-wired build. Composition is therefore
**multiple knobs, varied always per target**, not a single dial with a factory setting; the ADR
picks no point on the reader's behalf. §Decision 2 ("composition is injected, build-time") and
§Decision 4 ("bounded node is a property the deployer injects") are unaffected and carry the
whole weight now.

### 2. The triad is renamed to descriptive terms

`NARROW` / `MID` / `WIDE` are retired **as composition names**, everywhere in this ADR's body,
in §Considered options, in the sketches under §Building each configuration, and in §Verification:

| this ADR's old name | canonical name | what it is |
| --- | --- | --- |
| **WIDE** (one node-wide store) | **folded** | all stores share one source — "one slab, whole stack" |
| **MID** (graph store + net-plane store) | **per-plane** | one source per plane, segments still separate |
| **NARROW** (per-thread / per-connection) | **per-thread** | one source per RX thread (the sweep realises it per lane — per child, per link) |

The rename is the flagship of this amendment, not cosmetics: the old spelling **collided with
the canonical NARROW/WIDE target spectrum** — constrained MCU … big host — and collided with the
mapping *inverted*. Composition-`WIDE` was the **MCU** recipe and composition-`NARROW` was the
**many-core host** recipe, so a sentence like "NARROW ignores this accessor and injects its own
per-thread source" read, to anyone holding the target spectrum, as advice for the 16 KB part
when it was advice for the 24-core host. NARROW/WIDE from here on describe **targets only**
(CONTEXT.md §Store composition); compositions are folded / per-plane / per-thread.

Where the old spelling survives it is a **dated or banked record and stays as written**:
`bench/README.md`'s banked #941 tables, `bench/store_sweep_pins.json`'s `arm` keys and
`bench/bench_store_sweep.cpp`'s arm labels (the instrument's own identifiers — renaming them
would break every banked pin), and [ADR-0080](0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md)
§3's "ADR-0079's NARROW taken to its end", which means **per-thread**. Read them through the
table above.

### 3. per-plane is demoted to "the blast-radius point"

Per-plane was made the default on a contention argument that discriminated it from folded. The
measured sweep says it does not discriminate. On the `net-fwd` leg, T=1 → T=24 (ops/s per
thread, banked in `bench/README.md`):

| composition | T=1 → T=24 | T=1 `net-fwd` latency |
| --- | ---: | ---: |
| `H-baseline` (shipped, all-heap) | 0.45x | 242.73 ns |
| **folded** | 0.01x | 240.47 ns (within the null) |
| **per-plane** | **0.01x** | 233.44 ns |
| **per-thread** | **0.46x** | **220.39 ns (−9.2 %)** |

Per-plane collapses to **0.01x of its own single-thread rate by T = 24 — identical to the widest
(folded) arm** — because its net-plane store is one `pool_source_t<sync_mutex_t>` shared by every
receive thread: the same shape at a different granularity, not a different shape. **Only
per-thread scales** (0.46x, tracking the platform heap's own 0.45x curve) and it is the only arm
measurably faster than the shipped baseline (−9.2 % on the net leg, outside the 0.87 % A/A null).

So per-plane keeps exactly one claim, and it is a security claim, not a performance one: it is
**the blast-radius point** — the composition that fences a peer-provoked flood in the net-plane
store off from the graph plane (§Context, "Blast radius"). Choose it when that isolation is what
you are buying, and know that you are buying it *instead of* fan-out scaling, not alongside it.

### 4. Per-target recipes, in place of a default

- **Multi-RX host → per-thread.** The only composition that survives a fan-out, and the fastest
  per hop. Its cost is per-store slack: **3,870 B** measured over folded on the sweep's workload
  (~161 B/lane over 49 stores). On a host that is nothing.
- **Single-threaded MCU → folded.** One cap, tightest RAM, and contention-free *by construction*
  because there is one thread — the contention argument against folded is vacuous on that target.
  This is §289's "one slab, whole stack".
- **Anything wanting the net-plane fence → per-plane**, on the blast-radius claim alone.
- **Un-wired → all-heap**, which is what ships and what `H-baseline` measures.

The ~14 KB per-store-slack estimate in §Context remains an **MCU-target estimate over a different
channel set** and is not comparable to the 3,870 B above; §Verification's call for a real HWM
census on the constrained target still stands.

### 5. What this amendment does not touch

The whole-node (`full`) leg collapses in **every** injected composition, per-thread included,
because the graph plane is one locked store in all of them — that is Stage 2
([#843](https://github.com/avatarsd-llc/libtracer/issues/843), gated on
[#1285](https://github.com/avatarsd-llc/libtracer/issues/1285)), not a verdict on composition.
The §Staging programme, its family order, the gated/never-gated column split
([#1428](https://github.com/avatarsd-llc/libtracer/issues/1428)), and the separation of need C
(segments) all stand unchanged.

## Amendment (2026-08-27) — `graph_t` takes ONE source; the wiring sketches above are historical

The 2026-08-26 grill on [#873](https://github.com/avatarsd-llc/libtracer/issues/873) ruled the
shape this ADR left open, and phase 1 has landed it. **`graph_t`'s constructor collapses to one
injected `block_source_t`.** The `std::pmr::memory_resource*`, the `mem_backend_t*` and the
second (ring) source are gone from the signature; the graph builds a `source_resource_t` and a
`source_backend_t` over the single source internally. This ADR already sanctioned the outcome —
§"Building each configuration" said the `mr` argument is *"demoted to the std-container adapter
role on non-failable paths (or dropped)"* — so what changes is only that the parenthesis won.

Consequences for the text above:

- **The three `graph_t g{/*mr adapter*/, value_backend, &store};` sketches are historical.** The
  current spelling of each is `graph_t g{store};` — WIDE points that one store at the transports
  too, MID gives the graph its own, NARROW gives the graph its own and fans the net plane per RX
  thread. The compositions themselves are unchanged; only the graph's argument list is.
- **Decision 3's "`std::pmr` at `graph.hpp:353`" is discharged for the graph.** It survives only
  as the graph's internal adapter over the injected source, which is exactly the "thin adapter
  for std-container interop on non-failable paths" the decision allows.
- **Need C is still not folded into the substrate — it is re-expressed on top of it.**
  `mem_backend_t` remains the type that carries the refcount and the DMA hooks; it is no longer
  an injection seam. `source_backend_t` is that wrapper. Deployers who inject their own backend
  at a *transport* seam are unaffected.
- **Ring and control are one default, not one pool by stealth.** The graph-level default ring
  source now resolves to the same injected source, and per-vertex isolation stays where the
  amendment's own measurement put it — at `set_ring_source`, which is untouched.

Two channels the decision names are still open and are explicitly phased, not forgotten:
`lkv_slot.hpp`'s hazard-slot nodes are **phase 2** (gated on a hazard-slot acquisition A/B,
because the naive fix adds an atomic to `store()`), and re-layering the backend/pmr adapters is
**phase 3**.

### Final state (2026-08-27, phases 2 and 3 closed)

Both phases named in the paragraph above are now terminal, so this amendment is complete rather
than partial.

- **Phase 2 is a CARVE-OUT, by measurement.** The hazard-slot migration was built, measured and
  reverted on the gate the ruling set for it: +22.7 % on hazard-node acquisition and +3.5 % on
  the free-list-hit *steady* arm that never touches the substrate, ranges disjoint against a
  −0.12 % A/A null. `lkv_slot.hpp`'s nodes stay on the global heap, with the figures recorded at
  the seam and in reference 09. The ruling named this an acceptable terminal outcome.
- **Phase 3 is landed, and it is smaller than this ADR imagined.** Two of its three parts were
  already true. The pmr adapter needed no promotion: `source_resource_t` has been a public,
  reusable header since before phase 1, and the graph builds its channel out of that very type.
  `source_backend_t` was already the `mem_backend_t`-over-`block_source_t` wrapper. What phase 3
  actually changed is three things: `source_backend_t` now draws **one block per segment**
  instead of two (the packing phase 1 deferred, and the shape a size-classed `pool_source_t`
  needs); `heap_backend_t`'s acquisition is expressed on the substrate's own
  `heap_source_t::acquire`/`reclaim` arm, as a `static` non-virtual entry point rather than an
  injected `block_source_t&` — the layering claim without the indirection phase 2 priced; and
  the graph's **read-back encoders** stopped reaching the global heap through
  `view::over_bytes`'s one-argument overload and now mint their segments from the graph's own
  backend, so a peer's READ is charged to the deployer's slab like every other channel.
- **`source_backend_t` is NOT the only backend left, and the module set gains no `SOURCE`
  enumerator.** `heap_backend_t` (the process default), `mem_pool` (a caller-owned slab whose
  slab *is* the bound, with nothing to re-layer) and the two borrowed backends (which acquire
  nothing) all stay in the fast set for stated reasons. That question, left open by phase 1, is
  answered here rather than deferred again.
- **What one injection now bounds, and the two things it does not**, is tabulated in
  [reference 09 §"The channel ledger, at #873's close"](../reference/09-memory-substrate.md).
  The carve-outs are the hazard-slot nodes above and the plain `std::vector<std::byte>` sites
  whose container type is fixed by the signatures they cross. The router's and transports'
  own seams are outside the ledger on purpose — receiver-pays, per ADR-0060 erratum 1's
  shared-pool measurement — not by omission.
