# Allocation-store composition defaults to per-plane (MID), injected per target

Status: **proposed** (2026-08-06, for [#873](https://github.com/avatarsd-llc/libtracer/issues/873)).

The failable/placement allocation store (the `block_source_t` seam of [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md), extended to be the one substrate #873 asks for) is composed **per plane by default**: one **placement** store for the graph (L4) and one **failable** store for the net plane, with the payload-segment backend (`mem_backend_t`, need C) left entirely separate. The composition is an **injected, build-time choice**: a single-core MCU folds the two into one store ("one slab, whole stack"); a many-core host fans the net-plane store out **per receive thread**. Neither the throwing `std::pmr` default resource nor the un-injected global heap remains on any hot path.

## Context

#873 found one concern — "give me bytes, fail by value / with controlled placement" — served through **four** channels with four conventions: `std::pmr` (throws — `graph.hpp:217`), `mem_backend_t` (refcounted segment), `block_source_t` (raw block, fails by value — ADR-0065), and the **un-injected global heap** (hazard nodes, child-registry chunks, every `try_reserve`/`try_push_back`). Injecting bounded sources at three seams still does not bound the process, because channel four bypasses all of them.

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
3. **Both stray channels are removed from hot paths.** The throwing `std::pmr` at `graph.hpp:217` and the un-injected global heap (`graph.cpp`, `fwd_router.cpp`, `route_handle.cpp`, `lkv_slot.hpp`, `child_registry.hpp`) draw from the substrate instead. `std::pmr` survives only as a thin adapter for std-container interop on **non-failable** paths.
4. **"Bounded node" is a property the deployer injects,** not one the library fixes — it is delivered by sizing the injected store(s), and it is now actually deliverable because no allocation escapes to the global heap.

## Building each configuration

Composition is chosen entirely by **which `block_source_t` instances the deployer constructs and wires** — into `graph_t`'s `ctl` seam (`core/include/libtracer/graph.hpp:294`, the third ctor argument) and into each transport factory. No core edit selects a configuration; it is deployment wiring, closed at build time. The `std::pmr::memory_resource* mr` ctor argument is demoted to the std-container adapter role on non-failable paths (or dropped); `ctl` is the primary failable seam.

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
- Implementation is **staged and latency-gated**: each site class moved onto the substrate carries its own pinned A/B; a regression on any read/write hot path is a REJECT. The hot-vertex placement (#843) is the first concrete beneficiary; the net-plane failable moves (#603, #848, #930) are the isolation beneficiaries.
- The composition default and the WIDE/NARROW folds should be stated in the configuration-space doc so a deployer knows which knob sizes their node.
