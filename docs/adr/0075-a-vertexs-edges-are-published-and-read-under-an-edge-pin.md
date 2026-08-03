# A vertex's edges are published, and the fan-out reads them under an edge pin

Status: **ACCEPTED (2026-08-03).** Implements the fan-1 half of [#635](https://github.com/avatarsd-llc/libtracer/issues/635) — the half [PR #708](https://github.com/avatarsd-llc/libtracer/pull/708) left open when it closed the fan-0 half with the `own_subs_ordered() == 0` gate. Corrects [ADR-0064](0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md)'s "the `snapshot_edges` lock is free where it sits", which was measured at one thread. Answers the reclamation question [ADR-0072](0072-one-reclamation-domain-graph-owned-and-backend-injected.md)'s supersession explicitly left open for this issue, **without** rebuilding the generalized domain that supersession rejected. No wire change; a `core/CHANGELOG.md` note covers `vertex_t::add_edge`'s new failure answer.

## Context

`graph_t::fan_out` reaches `vertex_t::snapshot_edges`, which copied the vertex's slot table out under the **stripe** mutex — a lock a whole SET of vertices share, because [#361](https://github.com/avatarsd-llc/libtracer/issues/361) §2 deleted the per-vertex mutex to get the blocking primitives off the MCU's RAM budget. Two vertices with nothing in common but a hash collision therefore serialized their publishes against each other.

#708 closed the idle case: a vertex nobody subscribes to now returns before touching the lock. It could not close fan-1, where the gate cannot fire, and that is the case the measurement is worst in. On a 24-core host, T threads writing T *distinct* vertices deliberately chosen to collide on one stripe, fan-1:

| T | writes/s | vs. the same write on distinct stripes |
| ---: | ---: | ---: |
| 4 | 8.30 M | |
| 8 | 4.83 M | |
| 24 | 4.59 M | **×16.6 slower** |

Note the direction. This is not "fails to scale" — aggregate throughput *falls* as threads are added past four. The premise that makes it live is the one #551/#576/#635 each record: [RFC-0014](../spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) made connection create/remove a runtime, wire-driven operation, and the net plane forwards on multiple threads. Writers no longer arrive one at a time.

Every candidate direction reduces to the same question: if the fan-out stops holding a lock while it reads the edges, **what frees the edge list a control-plane verb displaces?** ADR-0072 answered that question once, generally, and was refuted on numbers — three reproducer-caught defects and an irreducible `seq_cst` fence on the *value-seam read path* measured at +19 %/+29 %. Its supersession is explicit that #635 keeps the question and needs its own answer.

## Decision

**A per-vertex published, immutable-after-publish edge array, read under a bounded per-participant EDGE PIN whose scope is the copy-out and nothing else, reclaimed by a retire list scanned on the control-plane mutation path.** One implementation for both targets.

- **Storage.** A vertex's `std::vector<subscriber_t> subs_` becomes `std::atomic<edge_block_t*> edges_`, null until the vertex is first subscribed to. The block owns the `:subscribers[]` slot table exactly as before — index-stable per [RFC-0009](../spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §D.2, mutated only under the stripe lock — plus the published array that is the slot table's dispatch-side projection, and the retire list of arrays displaced from it.
- **Read.** After #708's zero-count gate, the publisher claims its thread's pin cell, announces the published array with a `seq_cst` store, re-reads and re-validates, copies the active dispatch views into the existing inline/TLS buffers, and **releases the pin before the caller's first `dispatch_edge`**. The pin is non-nestable by construction (one cell per thread), and `pin_t`'s constructor asserts the cell is empty — so a re-entrant subscriber callback (a nested wide fan-out, a bubbling ancestor delivery) that found the pin still held would abort in every debug build and in both sanitizer legs.
- **Mutate.** Writer-vs-writer stays serialized by the **existing stripe mutex**, which is kept: the mutex leaves the *publish* path, not the control plane. Under the lock the new array is built and swapped in; the displaced one is pushed onto the retire list and scanned **after the lock is released, on the mutator's own thread**. The scan **never waits** — an array some participant still announces stays parked until the next mutation's scan or the block's teardown flush.
- **Liveness is one monotone bit per published entry.** An unsubscribe flips it and is therefore allocation-free and infallible; the compacting republish that actually releases the dropped entry's refcount clones may soft-fail on OOM without ever leaving the publisher delivering to an edge the caller has torn down.
- **Exhaustion.** A thread that cannot claim a cell (`kEdgePinSlots` in `config.hpp`, per-target through the existing template + drift gate) copies the *current* array under the stripe mutex — safe because displacing an array requires that same lock. Correctness never depends on the constant; only scaling does.
- **Allocation failure.** Publishing a new array is the one allocation an append makes. On refusal `add_edge` rolls the slot back and answers `kNoSlot`, and `graph_t::subscribe` gives its speculative listener bump back and reports `BACKPRESSURE` — the injected-resource status, per the #477 nothrow doctrine.

### Why a pin is admissible here when ADR-0072 said fan-out could not hold one

Because of one code fact the supersession's general argument did not price per site: `snapshot_edges` **already copies out** — an `edge_view_t` owns its strings and refcount clones — and `fan_out` already dispatches **outside** the vertex lock. The window a pin has to cover is therefore the copy loop alone: bounded, no I/O, and provably not re-entrant, because the pin is released before the first callback. That is not the "holds a block across an unbounded, re-entrant, I/O-doing scope" the supersession attributes to fan-out.

The three #750 defects are structurally absent, not merely avoided. Readers never wait, so no mutual-wait cycle can form. The free runs on the mutator's thread outside every lock. And a published edge array owns **only library types** — `subscriber_t::callback_ctx` is caller-owned and never destroyed by us — so no embedder destructor can execute inside this machinery, which is the entire round-2/round-3 defect class.

### Measured

All arms per the #807 protocol: ≥11 interleaved A/B pairs, medians and full ranges, a claim accepted only on **disjoint** distributions. 24-core host.

| arm | T | base median | candidate median | | |
| --- | ---: | ---: | ---: | ---: | --- |
| `stripe1-fan1` | 4 | 8.30 M/s | 29.75 M/s | ×3.59 | disjoint |
| `stripe1-fan1` | 8 | 4.83 M/s | 44.44 M/s | ×9.21 | disjoint |
| `stripe1-fan1` | 16 | 4.26 M/s | 68.90 M/s | ×16.17 | disjoint |
| `stripe1-fan1` | 24 | 4.59 M/s | 88.82 M/s | ×19.35 | disjoint |
| `stripe1-fan1` (pinned) | 1 | 10.99 M/s | 12.47 M/s | ×1.13 | disjoint |

Aggregate throughput is monotonically non-decreasing from T=4 to T=24 — the negative-scaling headline is gone — and T=24 reaches the deletion ceiling the issue measured (78.7 M/s) on this host. The single-threaded arm, pinned, is *faster*: ADR-0064's own argument is that this path is serializing-op-count bound, and a `seq_cst` store to the reader's own cache-line-isolated cell plus a release clear is fewer serializing operations than a mutex lock/unlock pair. That is also why a refcounted published array was rejected instead: its refcount RMW lands on a line every reader of the vertex shares.

The distinct-stripe control (`spread-fan1`) stays at unity across every T, which is what attributes the gain to the stripe. The `hot1` arms are excluded from acceptance and rejection alike — they are bistable ([#464](https://github.com/avatarsd-llc/libtracer/issues/464)), and this run demonstrates it directly: `hot1-fan0`, a path this change does not execute at all (the #708 gate returns first), still produced a ×0.58 *disjoint* swing.

**rv32** (`-Os`, real `core/src/graph.cpp`, GCC 15.2, single-core profile): `sizeof(vertex_t)` **80 → 72 B**, so the 32-bit ceiling that had zero headroom now has eight bytes; the TU's `.bss`+`.sbss` grows 304 → 560 B (the 32-cell pin registry, always emitted); `.text` grows 51,230 → 52,960 B. The heap moves the other way for a *subscribed* vertex: the block (20 B) plus one published array (8 B + 84 B per slot) is new resident memory that the old design built on the stack per publish instead.

## Alternatives rejected

- **The generalized reclamation domain (ADR-0072 §1–§3, PR #750).** Rejected by record and honoured here: three rounds, three reproducer-caught defects, and a measured latency regression on a shipped read path — which is a REJECT, never a trade. This design reuses the announce/scan **protocol shape** on one site's publish path, adds nothing to any read path, registers no tenants and erases no types beyond a `const void*` announcement.
- **`graph_t::collect()` as this tenant's reclaimer.** Its contract — call it where no lock-free reader holds one — is unsatisfiable here: at tens of millions of writes per second across 24 threads there is no nameable moment when no thread is mid-copy, and `collect()` does not wait. Extending it with a wait rebuilds the scan inside the seam mechanism the supersession forbids citing as precedent for this tenant.
- **A seqlock / version-validated optimistic read.** The snapshot must produce *owning* copies. A refcount increment performed on a control block freed between copy and validation is a use-after-free validation cannot undo; `memcpy`-then-validate has the same window at the bump. Safe only with deferred reclamation underneath — at which point it is this design plus a retry loop for nothing.
- **A refcounted published array (`atomic<shared_ptr<edge_array>>`).** Two disqualifiers: libstdc++'s `atomic<shared_ptr>` is spin-locked (ADR-0064 §2), and every delivery would pay a refcount RMW on a line all readers of that vertex share — the exact term ADR-0064's consequences show inverting on the true-sharing shape.
- **A per-vertex mutex.** Reintroduces the per-vertex blocking-primitive RAM #361 §2 deleted (a pthread mutex is 40 B against a 120 B vertex budget) and still serializes publish-vs-publish on one vertex.
- **`std::shared_mutex` on the stripe** (ADR-0064's measured deferral). Caps at ×2.85 at T=24 against a ×16.6 ceiling, regresses the true-sharing arm, and drags `await` onto `condition_variable_any`. Its own text anticipated being superseded by exactly this design.
- **Epoch-based reclamation instead of the pin.** Same machinery class with a worse-stated bound — parked memory scales with write rate × read-region length rather than hazard's constant-per-slot. [ADR-0069](0069-lkv-slot-is-a-compile-time-policy-hazard-reclamation.md) §2 already made this choice on a latency tie with RAM deciding; nothing about this tenant flips it.
- **Raising `kVertexLockStripes` on the host.** A probability reduction, not a fix: birthday collisions remain certain at scale, and a constant that looks like a fix invites closing the issue against it.
- **Accept and document it as a contention-only cost.** Contradicts the latency design centre — the many-core host is first-class, and it is the shape Zenoh-class comparisons are judged on.
- **A per-target policy split as the first move** (host binds the array, MCU keeps the mutex). Kept as the **pre-registered contingency** if a target's gate fails, not taken as the first move: it violates delete-don't-shrink (a knob preserving the residual mutex path before measurement forces it) and doubles the tested surface. The wait-free design is MCU-correct by construction — no spins, no priority inversion, so [ADR-0064](0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md)'s single-core FreeRTOS hazard cannot arise — so the split must be earned by a failed gate.

## Consequences

- The mutex leaves the publish path outright. There is no knob keeping the old path alive on either target; the contingency above is a design to fall back to, not code sitting behind a flag.
- The cost moves to subscribe/unsubscribe, which is control-plane by nature and the correct place to pay it. A mutation now rebuilds and publishes an array rather than editing a vector in place.
- `vertex_t::add_edge` can refuse. Callers must not count a listener against `kNoSlot`; `graph_t::subscribe` is the one in-tree caller and reports `BACKPRESSURE`.
- **The graph must outlive the threads that published through it.** This is the same contract ADR-0069 §6 states for the LKV domain, and `edge_block_t`'s teardown flush is where it is stated for edge arrays. A thread still inside `snapshot_edges` when its vertex is destroyed is a use-after-free with or without this mechanism.
- Parked memory is bounded by the arrays displaced while some reader is mid-copy — a control-plane rate times a sub-microsecond window — and is flushed at teardown. It is never leaked and never waited on.
- `kEdgePinSlots` is always emitted, unlike `kHazardReaderSlots`: the publish path is not a policy binding. A single-core target sets `kCacheLineBytes` to 0 and the registry collapses to 8 bytes per cell.
