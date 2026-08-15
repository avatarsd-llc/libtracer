# Allocation-store composition defaults to per-plane (MID), injected per target

Status: **accepted** (2026-08-15; drafted 2026-08-06 and merged 2026-08-06 as [PR #940](https://github.com/avatarsd-llc/libtracer/pull/940), for [#873](https://github.com/avatarsd-llc/libtracer/issues/873)).

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
