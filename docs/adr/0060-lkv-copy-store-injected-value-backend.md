# The last write-path heap allocation becomes poolable: the LKV copy-store draws its owned `segment` from a graph-injected `value_backend_` (`mem_backend_t`)

Status: accepted (maintainer-ratified 2026-07-21 in a grill-with-docs session). **Refines [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) §2–§3** (the owning-delivery subview store and its injected receive backend) — it pools the copy leg [ADR-0041](0041-terminus-arena-decode-span-contract.md) §2 left on the default heap. Upholds [ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) §2/§4 (one slab, exhaustion-is-backpressure), [ADR-0012](0012-modular-memory-binding-transparent-router.md)/[ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) (`mem_backend_t` is the L0 byte-buffer seam), and [ADR-0051](0051-delivery-terminates-at-target-no-dispatch-limits.md)/[RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md) (bounds are injected resources, never magic constants).

## Context

[ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) gave an **owning** (view-delivered) frame a zero-copy store: a large WRITE stores a **subview** of the refcounted frame (`store_ref_min_bytes`). But it deliberately left three cases **copying** the value into a fresh owned `segment`: a **borrowed** (span-delivered) frame — a borrowed span cannot be pinned — a **small** payload (a copy beats pinning a whole frame), and a **trailered** oversized payload. That copy is where a `STORED_VALUE` vertex's last-known-value (LKV) gets its durable bytes.

Today that copy allocates from the **default `heap_backend()`** — the write path calls `value.materialize()` with no backend argument (`graph.cpp:1030`, `:804`). So on any node whose transport delivers borrowed — which includes the ESP-IDF `httpd_ws_link` the reference firmware runs — **every stored write allocates on the general heap**. It is the one remaining unpoolable allocation on the write hot path, and a source of the heap fragmentation / low `min_free` pressure observed on-device (the `svc_gate` sheds WS work near the service floor).

## Decision

**`graph_t` gains one injected byte-buffer seam, `mem_backend_t* value_backend_` (constructor parameter, defaulted to `mem::heap_backend()`).** The write-path `materialize()` / copy-store sites draw the LKV's owned `segment` from it instead of the default heap. A bounded host passes a `pool_t` over its slab; the default keeps behavior byte-identical. This is the mem_backend leg of "one slab, whole stack" ([ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) §2) applied to the value store — a distinct injection *point* from ADR-0042's transport-receive backend, which a host typically wires to the **same** slab.

Four sub-decisions, each with its rejected alternative:

1. **Seam = `mem_backend_t`, not `std::pmr`.** The value is a durable byte `segment`, and `mem_backend_t` is the doctrinal L0 byte-buffer seam: cache hooks, `owns_bytes` ("durably storable"), ISR-safety, and `alloc`-or-`nullptr` backpressure that `std::pmr` lacks; `pool_t` already *is* the bounded arena. The graph's existing `std::pmr mr_` stays — it allocates the `shared_ptr` + `rope_t` **wrapper object**, a different allocation *kind* from a cache-managed byte buffer, so it is not redundant. (The lean end-state — one allocation per write packing wrapper + value into a single `segment`, retiring `mr_` — is recorded as a follow-up, not required for the win.)

2. **One thread-safe pool, not per-stripe; sync is an arch-selected trait, never an OS mutex.** A `segment` self-routes its own reclaim: `segment_ptr_t::release` fires `destroy_dispatch(seg)` → the segment's own backend `destroy`, **on whatever thread drops the last ref** (`segment.hpp:80`, `:137`) — typically a *reader/subscriber* thread, concurrent with a writer's `alloc`. Because reclamation is cross-thread, sharding the pool per lock-stripe removes no race (each shard still sees reader-free vs writer-alloc) while adding partition imbalance (false exhaustion when one shard fills and another is empty). So `value_backend_` MUST be thread-safe — but the sync mechanism is chosen by the target's concurrency model (an [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §2 module-set trait, alongside `is_isr_safe`), **not** a heavyweight OS mutex, which at ~2 µs (a FreeRTOS semaphore round-trip) would dominate the ~120 ns O(1) free-list op and negate the pool. The spectrum: an **interrupt-disable critical section** on single-core targets (the ESP32-C6, Cortex-M — fastest, est. ~150–250 ns alloc+free); a **spinlock** on multi-core (the ESP32-S3, host — negligible contention for an O(1) section); and a **lock-free index+tag CAS** held as a *measured* upgrade for a many-core host under contention. Lock-free is cheap to reach here: `pool_t`'s free-list is already index-based (`free_head_` is a slot index, `mem_pool.cpp`), so `[index | ABA-tag]` fits one word — **no double-width CAS** — and libtracer already assumes atomic CAS (the segment refcount, `segment.hpp:54`), so this adds **no new arch floor**. The default `heap_backend()` is already thread-safe, so a non-pool host pays none of this.

**Acceptance gate:** a bench (`bench/bench_libtracer.cpp` → the `lkv-alloc-*` / `lkv-store-*` series, gated by `bench/perf_gate.py`) must show the pooled path decisively cheaper than the default heap with **zero fragmentation growth** (fixed slots, every allocation reclaimed) before the pool backend is recommended. Measured (host, glibc): the pooled alloc/free clears **~2.5×** the default heap and the end-to-end flatten **~2.4×** — *not* the ≳10× first estimated, because glibc's tcache serves a hot same-size `malloc`/`free` in ~15 ns, against which the pool's ~6 ns O(1) free-list is only ~2.5×. The **≳10× margin is the constrained-target figure** — the ESP-IDF `multi_heap` alloc is hundreds of ns and fragments under churn, where the pool's flat cost and zero fragmentation dominate — and is validated **on-device** (with the FreeRTOS-mutex-vs-crit-section numbers) as the recorded follow-up. The host gate therefore asserts the robust invariants (routing works — no heap fallback — and the pool is deterministically faster, ≥2×), not the host-allocator-dependent multiple.

3. **Graph stays size-agnostic — `alloc`-or-BACKPRESSURE.** On `nullptr` (pool exhausted *or* value > slot) the write returns a resource-reject, not a silent `heap_backend` fallback (which would breach the bounded-memory guarantee the host bought by injecting a pool). Value-size distribution is the **host's** to compose — a uniform-telemetry pool, or a size-class / pool-plus-heap-fallback composite `mem_backend_t` — never graph logic. This holds the "no synthetic limits" line: exhaustion is an injected-resource signal, not a hardcoded constant.

4. **Eliminating the copy is a separate, later optimization.** The copy is cheap (a small `memcpy`); the *allocation* is the cost, and pooling it is this ADR's whole win. Making a borrowed-delivery transport (e.g. `httpd_ws_link`) deliver **owning** so the store subviews instead of copies (ADR-0042 §1/§3, `set_view_receiver`) is per-transport surgery that pins frame-sized buffers — it gets its own decision, layered on top of this one.


## Erratum 1 — "negligible contention" is refuted, and the proposed CAS upgrade would not fix it

§2 says a spinlock is fine on multi-core because there is "negligible contention for an O(1)
section", and holds a **lock-free index+tag CAS** as the measured upgrade for a many-core host.
Measured on a 12-core host with the (now honestly named) `poolalloc-mt*` / `heapalloc-mt*` rows —
every thread instrumented, so latency and throughput describe the same workload:

| threads | pool ops/s | pool p50 | heap ops/s | heap p50 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 8.3 M | 60 ns | 15.8 M | 30 ns |
| 4 | 3.6 M | 802 ns | 25.5 M | 70 ns |
| 8 | **1.36 M** | **3587 ns** | 31.0 M | 70 ns |

Two things follow, and the second is the important one.

**Contention is not negligible — it is catastrophic.** The pool does not merely serialize; it drops
to roughly a *fifteenth of its own single-thread rate*. Pure serialization would hold flat at the
T=1 figure. Collapsing far below it is the signature of a cacheline storm: `sync_pool_t` takes one
global `atomic_flag` twice per operation (once in `alloc`, once in `destroy`) and `lock()` is a bare
`test_and_set` spin with no test-load and no pause, so every waiter's RMW steals the line. The heap,
by contrast, *scales* — glibc's per-thread tcache has no shared line to contend. Independently
reproduced on the 4-core CI runner.

**The proposed fix would not have worked.** A lock-free `[index | ABA-tag]` CAS on `free_head_`
replaces one contended word with… the same one contended word. It removes the spin but keeps every
thread hammering a single cacheline, which is what actually costs the 15×. The shape that fixes this
is per-thread free-lists or magazines — i.e. what the heap already does. Recording that here matters
because §2 presents the CAS as the known answer, and someone would have built it and measured no
improvement.

**What is NOT retracted.** The single-threaded case is unaffected (the pool's O(1) op is genuinely
cheap), and the MCU rationale stands: on a single-core target with an interrupt-disable critical
section there is no concurrent RMW to storm, and a deterministic bounded ceiling is the point rather
than throughput. This erratum is about the *multi-core* branch of §2's spectrum only.

Related: the same "the pool is a latency lever" reading is refuted single-threaded in
[ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md)'s errata (295 vs 309 ns on the terminus,
~85 vs ~104 ns on the in-process write). This is the third independent measurement.

No redesign is proposed here. The finding is recorded; the fix is an ADR-0060 decision with its own
gate.

## Erratum 2 — the arch-selected sync mechanism is built, as a compile-time policy (#770)

Decision 2 described the mechanism spectrum in prose and left `sync_pool_t` — a hardcoded
`std::atomic_flag` spinlock — as its only realization, which
[ADR-0063](0063-connection-table-lock-free-reads-trait-serialized-writes.md) Erratum 1 correctly
called out as "the trait does not exist to be reused". It exists now, for the **data-path pool**
only: `mem::synchronized_pool_t<Sync>` (`core/include/libtracer/mem_pool.hpp:170`) takes the
critical section as a template parameter constrained by `mem::pool_sync_policy`, with
`mem::spin_sync_t` (the multi-core host) in `core/` and `tr::esp::portmux_sync_t` (the single-core
interrupt-disable section) in the ESP-IDF component, where the FreeRTOS headers are.
`sync_pool_t` is retained as the alias for the host pairing, so nothing that used it changed.

Two limits this does **not** lift. It is a *compile-time* choice per ADR-0068 — the target knows
its concurrency model at build time — not a runtime-selectable trait, so it says nothing about
call sites that need a *blocking* lock: ADR-0063 Erratum 1's ruling (the connection table's
milliseconds-long control-plane section takes a mutex, not an interrupt-disable section) stands
untouched, and this policy must only ever wrap the O(1) free-list op it was measured on. And it
remains **opt-in construction**: no seam defaults to a pool, `heap_backend()` is still the default
at `value_backend`, `flat` and the transport receive backend.

## Consequences

- New surface: one defaulted `mem_backend_t*` constructor parameter on `graph_t`. Additive; no existing caller changes; behavior byte-identical until a host injects a pool.
- The last default-heap allocation on the copy-store write path becomes poolable → deterministic, fragmentation-free value memory on bounded targets; directly targets the on-device `min_free` / `svc_gate`-shedding pressure.
- ADR-0042's subview store remains the *zero-copy* path for large owning-frame writes; this ADR only changes **where the copy leg allocates**, for the borrowed / small / trailered cases that leg always covered.
- A host achieves "one slab, whole stack" by pointing `value_backend_` and the transport-receive `mem_backend_t` at the same `pool_t`.
- Scope note (#831): the seam covers **payload bytes**, on the read path as well as the write path — the branch/field-write flattens it was introduced for, and **both** folded READs' POINT headers — `read_subtree_folded`'s per-subtree-node header and `read_children_folded`'s per-child plus outer listing header (the site the wire `":children"` READ routes to) — whose length field wraps the stored TLV and the name records below it. Route-byte-sized reply-egress construction is explicitly *not* in scope — that is [ADR-0074](0074-terminus-reply-egress-is-its-own-injected-backend.md)'s own seam. All share this seam's contract exactly: allocate-or-`nullptr` degrading to `BACKPRESSURE` by value, and cross-thread self-routed reclaim (§2), which the folded-READ headers need because they escape inside the reply rope and are freed wherever the last reference drops. An injector sizing a bounded slab budgets for these too; their count is peer-influenced (a peer picks the composed root, and thus how many subtree nodes fold; or whose `":children"` to list, and thus how many members frame). The graph stays size-agnostic (§3) — the size classes remain the host's composition problem, so an ADR-0067 one-slab node re-sizes nothing new.
- Follow-ups recorded: (i) `httpd_ws_link` owning delivery, which unlocks the subview store on the board; (ii) collapsing the wrapper + value into a single per-write `segment` to retire `mr_`.
