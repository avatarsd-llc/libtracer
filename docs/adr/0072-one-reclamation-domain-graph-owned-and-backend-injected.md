# One reclamation domain: graph-owned, backend-injected, type-erased

Status: **accepted; §§1–3 implemented for [#576](https://github.com/avatarsd-llc/libtracer/issues/576), amended by Erratum 1 below** (tracking: [#684](https://github.com/avatarsd-llc/libtracer/issues/684), [#576](https://github.com/avatarsd-llc/libtracer/issues/576), [#635](https://github.com/avatarsd-llc/libtracer/issues/635)). This is the reuse [ADR-0069](0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md) §4 anticipated: *"the hazard machinery landed here is expected to be reusable there, and that is a reason to land this first."* It landed; this ADR rules on the reuse.

## Context

Three open issues are one question — *what happens to a heap block a control-plane writer replaces while a lock-free reader still holds a raw reference into it* — and the codebase currently answers it three incompatible ways:

| site | block | today's answer | failure |
| --- | --- | --- | --- |
| [#684](https://github.com/avatarsd-llc/libtracer/issues/684) `child_registry_t::add` rebind | `mount_tlv` | free it immediately | use-after-free → a **misroute** at the next hop |
| [#576](https://github.com/avatarsd-llc/libtracer/issues/576) `graph_t::retire` | `value_handlers_t` | park it in `retired_seams_`, never free | unbounded, **peer-driven** growth |
| [#635](https://github.com/avatarsd-llc/libtracer/issues/635) `snapshot_edges` | the `subs_` edge storage | exclude readers with the stripe mutex | ×16.6 throughput collapse at T=24 |

The premise shift that makes all three live now is the same one #551/#576/#635 each record: RFC-0014 made connection create/remove a runtime, wire-driven operation, and the net plane forwards on multiple threads. Retirement is no longer rare, and writers no longer arrive one at a time.

Meanwhile `core/include/libtracer/lkv_slot.hpp`'s `namespace detail_hp` (ADR-0069 §5) is a working, measured (4.2× at 24 readers through `graph_t::read`), CI-gated hazard-pointer domain — with two properties that block direct reuse: it is **typed to `detail_hp::node_t`** (which exists to own a `shared_ptr<const rope_t>`), and it is a **process-global that allocates from the global heap**, which ADR-0069 itself lists among the reasons the MCU target keeps `sp_atomic_slot_t`.

## Decision

### §1 — One mechanism: `detail_hp` generalizes into the reclamation domain

Rejected alternatives: **per-site answers** (an epoch sweep here, immutability there, a deferral elsewhere) scatter the hardest concurrency reasoning in the codebase across three files and leave the next site to re-derive it; **a second open-coded mechanism** duplicates the one part of `detail_hp` that took real effort to get right (the announce/scan protocol and its exhaustion policy) while leaving the original untouched beside it.

The domain becomes a seam with real adapters — the retired-seam store (#576) and the published edge array (#635) immediately, the LKV slot by migration later, and the deletion test passes: removing it would re-scatter reclamation logic into every tenant. This is not unification for its own sake (rank 3): #635's fan-1 candidates *all* reduce to needing deferred reclamation, so the domain sits on the latency/throughput critical path (rank 1).

### §2 — `graph_t` owns the domain instance

Today's `detail_hp` is process-global — static cells, a shared overflow index, a process-exit sweep. That shape cannot take an injected resource (which of several?) and cannot be torn down (half of what #576 is). Instead:

- **One domain instance per `graph_t`**, constructed with the graph, borrowed by reference by the net plane (`fwd_router_t`, `child_registry_t` — both already hang off the graph's composition root).
- **`~graph_t` runs the final sweep.** The destructor does not exist today (`grep ~graph_t` returns nothing); it lands with the domain. This is also the teardown point #551 wanted independently.
- Host tests running several nodes get isolated domains and isolated bounds; a thread touching two graphs announces in two domains (one extra cache line per domain — acceptable, and a semantic change from today's global cells worth this line).
- **The LKV slot's private static domain migrates later, not first.** `hazard_slot_t` is the scariest tenant, CI-pinned in two build variants; the generalization must not begin by moving it.

Rejected: **process-global with an init-time resource** (teardown stays impossible; several nodes share one bound; violates the injected-resources doctrine in spirit) and **a standalone object owned above `graph_t`** (adds a construction-order obligation to every embedder for a thing only insiders understand).

### §3 — Type-erased retire records, allocated from the injected backend

The retire API is `retire(void* block, void (*deleter)(void*, mem_backend_t&))`; the retired list holds `{ptr, deleter}` pairs; announcement cells stay `void*` (they already are). One domain instance serves every tenant type.

This is consistent with the compile-time doctrine (ADR-0047), not an exception to it: the doctrine targets *dispatch on the hot path*, and the hot path here — announce/clear — is untyped and dispatch-free under either spelling. The deleter fires only on a **scan**, a cold control-plane event past a threshold. A `hazard_domain_t<Ts...>` would spend per-type retired lists (or a variant-visit) on a cold branch and ripple a template parameter through `graph.hpp` every time a tenant is added.

Two bounds, per the §Resource-bound rule (no synthetic limits):

- **Records allocate from the injected backend**, so retirement pressure is bounded by a real resource the target sizes. The deleter's second argument returns the block to the resource it came from; a record captures nothing beyond the pair.
- **Exhaustion policy:** when a record cannot be allocated, the writer runs a **blocking scan and then frees inline** — correct, slow, bounded. Never leak, never abort. (Same shape as ADR-0069 §3's guarantee: correctness never depends on the bound being right.)

### §4 — Per-site application

**#684 does not use the domain.** Reading the code dissolved the race instead: `encode_mount_name` (`child_registry.hpp:416`) is a pure static function of the name alone, and the rebind slot is found *by name match*, so a live slot's replacement `mount_tlv` is **byte-identical by construction**. The fix is the immutability invariant — the rebind path stops assigning `mount_tlv` (it keeps the `multi_peer`/`link` atomic stores, the fields a reconnect actually changes), a debug assert pins the byte-equality, and the doc comment states: *`mount_tlv` is a pure function of the slot key and immutable after publish.* Zero hot-path cost, zero RAM, and the reconnect path drops an alloc+free. Routing it through the domain instead was rejected because it would put an announce/clear pair — a `seq_cst` fence — on `route_fwd_forward`, the bounded node's forwarding hot path, to defend bytes that provably never change. If slot recycling under a *different* name ever becomes legal (#407 teardown, #622), that work inherits the burden of the documented, asserted invariant. The stale comment at `fwd_router.hpp:336` (asserting slot addresses are unstable — ADR-0063 made them stable) is corrected in the same pass.

**#576 becomes the first tenant.** `retire()` hands the seam pointer to the domain; `retired_seams_` is deleted. The four lock-free `handlers()` readers (`graph.cpp` — `read`, `store_value`, `read_children`, `read_children_folded`) each announce before dereferencing and clear after the user callback returns; a slow callback merely delays that one block's reclamation. The announce is gated behind the existing has-extension check, so the placeholder/fan-0 path (RFC-0005's near-free-when-idle shape) pays nothing. **Bench-gated:** interleaved A/B on `read`/`write` of a handler-bearing vertex, host and rv32, before merge. If the pair moves the number, the fallback design — one announcement scope per operation covering both the LKV slot and the seam — is recorded here as the next step, not built preemptively.

**#635's fan-1 half becomes the second tenant.** `subs_` becomes an atomic pointer to an **immutable edge array**; `add_edge`/`clear_edge` build-new, swap, retire-old through the domain; `snapshot_edges` becomes announce → load → read → clear. The stripe mutex leaves the publish path entirely (its ceiling is the measured ×16.59 deletion number, against `shared_mutex`'s ×2.85 cap). Cost moves to edge mutation — control-plane by nature. Constraints carried from #708: the swap must preserve the `own_subs_` Dekker pair, and acceptance numbers come from the same-stripe-distinct bench arm, never `hot1` (bistable, #464).

**Slice order: #684 → #576 → #635.** The one-liner first; the domain lands with its first tenant; the third tenant rides.

## Consequences

- The repo answers the replaced-block question **once**. A future site (e.g. #603's label tables) adopts a tenant, not a mechanism.
- `graph_t` grows a destructor and a domain member; embedder surface is unchanged.
- The MCU objection to `detail_hp` (global-heap allocation) is retired by construction: every domain allocation draws from the graph's injected backend.
- The generalization touches `lkv_slot.hpp` only to *extract* the reusable protocol; `hazard_slot_t`'s behavior and its CI matrix legs must be byte-for-byte unaffected until the migration slice, which gets its own measurement.

## Erratum 1 (2026-08-01, [#576](https://github.com/avatarsd-llc/libtracer/issues/576) implementation)

Building the first tenant refuted three specifics of §3 and §4. The §1/§2 rulings — one mechanism, one instance-owned domain per `graph_t`, `lkv_slot.hpp` untouched — stand unchanged.

### E1.1 — Retire records are INTRUSIVE, not backend-allocated; the blocking exhaustion policy is deleted

§3 specified `retire(void*, deleter)` with records drawn from the injected backend and, on exhaustion, *"a blocking scan and then free inline — correct, slow, bounded"*. **That policy is a peer-reachable deadlock**, not a slow path:

`graph_t::retire_subtree` runs under `std::unique_lock(map_mutex_)`. The four `handlers()` readers announce *outside* that lock precisely so their user callbacks may re-enter the graph (RFC-0010 §A.3 states that freedom for the apply seam, and every synthesized-listing handler uses it). A retirer that blocks until an announcing reader clears is therefore waiting for a thread that may be waiting for the map lock the retirer holds. Records draw from the same injected backend a peer can pressure, so the two conditions line up under exactly the connection churn #576 exists for.

The remedy is not a better fallback but the removal of the failure mode: **the retire record is a `tr::mem::retire_link_t` the tenant embeds as the FIRST member of the block it will retire** (`value_handlers_t::hz_link` for this tenant). `retire(link)` allocates nothing, cannot fail, and never waits; `scan()` re-parks an announced block rather than waiting for it. The §3 bound *"retirement pressure is bounded by a real resource the target sizes"* is replaced by a strictly stronger one: **a node cannot retire more blocks than it allocated in the first place.** The backend reference survives — it is still what a reclaimer returns backend-allocated blocks to (the §3 deleter contract is unchanged).

The record is **one word**, and that is a measured requirement rather than an aesthetic one: a three-word record pushed the 96-byte seam block into the next allocator size class and cost 16 B on every handler-bearing vertex — the perf gate's `mem:reg_escape` row failed on it. One word costs nothing, and it takes two moves to get there. The block's address IS the link's address (hence "first member", asserted with `offsetof`), and **the reclaimer moves from the record to the domain's constructor: one domain per kind of block.** §3's type erasure survives intact — it just lives on a construction parameter now. A composition root with two kinds of reclaimable block owns two domains, which is cheap precisely because of E1.2: the expensive part, the announcement table, is shared process-wide, so the second domain is a few words.

### E1.2 — Announcement is PER-THREAD, in a process-global table; the overflow spin lock becomes a stall counter

§2's "instance, not process-global" ruling was about the retired list and the teardown point, and it holds for both. It also, incidentally, put the *announcement cells* on the domain, and the first implementation claimed one **per operation** with a `seq_cst` CAS. Measured, that CAS plus the gate counter was most of a +36% handler-read regression.

A thread now claims one participant (a cache-line-isolated group of announcement words) **once**, and releases it at thread exit. The participant table is process-global, which is sound because **announcements are matched by address**: two live blocks cannot share one, so a foreign domain's announcement can never match a parked block. Per-instance RAM drops (one table, not one per graph); the per-instance bound §2 wanted is a property of the retired list, which stays per instance.

The `detail_hp` overflow policy — a shared cell under a **non-recursive spin lock** — is replaced by a per-domain **stall counter**: a guard that can get no announcement word (more reader threads than participants, or nesting deeper than a participant's words) increments it, and a scan frees nothing while it is non-zero. It is a counter, not a lock: it nests to any depth, it never waits, and it cannot self-deadlock — which matters because the library's own call sites hold a guard across a user callback that may re-enter the graph and announce again. Nesting is therefore SAFE at any depth; past `kAnnouncePerThread` it costs reclamation throughput on that thread and nothing else.

### E1.3 — §4's fold is NOT the remedy; the remedy is an asymmetric barrier

§4 recorded *"one announcement scope per operation covering both the LKV slot and the seam"* as the fallback if the announce pair moved the number. **That fold cannot pay on the legs that moved.** `graph_t::read` forks on role: `role() == HANDLER` reads the seam and returns; every other role reads the LKV slot. The two announcements are on mutually exclusive branches of the same `if`, so there is no operation that performs both and nothing to fold. Measured lever by lever (`bench/run_seam_ab.sh`), the cost was the ordering itself, not the count: a hazard reader needs StoreLoad between "I announce p" and "is p still published?", which on x86-TSO is an `mfence`-class instruction on every protected read.

The remedy is to move that ordering to the side that is cold. Where the OS can serialize every other CPU on demand (Linux `membarrier` `PRIVATE_EXPEDITED`), the reader announces with a **plain store** and the reclaimer pays a barrier once per scan; where it cannot — every other platform, an old kernel, a denied syscall, or a ThreadSanitizer build whose happens-before model cannot represent an IPI — both sides fall back to the classical `seq_cst` protocol and nothing about correctness changes. This is the one place the design depends on a platform capability, and it degrades to correct-and-slower rather than to incorrect.

### E1.4 — The gate counter is a build option, not a shipped member

`seam_announces()` is the §4 gate's structural observable, and a relaxed RMW on a shared word measured at roughly a tenth of a handler read. It is now `std::optional`, maintained only when `kSeamAnnounceCounter` is on (the repo's own builds), and `nullopt` — never a plausible-looking `0` — everywhere else.
