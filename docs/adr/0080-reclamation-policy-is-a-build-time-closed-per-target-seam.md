# Reclamation of a user-code seam is a build-time-closed, per-target policy

Status: **proposed** (2026-08-06, for [#894](https://github.com/avatarsd-llc/libtracer/issues/894) and [#897](https://github.com/avatarsd-llc/libtracer/issues/897)).

When may libtracer free the memory behind a user-code seam — a subscription's `callback_ctx`, or an LKV node parked from a hazard slot — after the user asks to release it? The answer is **not one mechanism**: it is a **build-time-closed policy** selected on the same target axis as [ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md)'s store composition. A single-threaded build reclaims at the local dispatch-stack grace point (or forbids the re-entrant case outright); a many-core build reclaims at a quiescent state (QSBR). The default is the policy that makes the two targets behave **identically**. There is **no runtime prose contract** asking the embedder to track in-flight state.

## Context

`unsubscribe()` returns with **no quiescence guarantee** (#894): the publish path snapshots edges under the stripe lock and dispatches **outside** it (`fan_out`, `graph.cpp`), so a snapshot taken before `clear_edge` swaps in its inert shell will invoke the old `{fn, callback_ctx}` **after `unsubscribe` has returned success** — a use-after-free the API cannot help the caller avoid. The sibling case (#897): a displaced LKV node lands on the **displacing thread's** private retired list (`lkv_slot.hpp`), and `~hazard_slot_t` cannot reach a still-live *other* thread's list, so a parked `shared_ptr<const rope_t>` allocated from the **injected** resource is later freed through a destroyed resource.

Three facts shape the answer:

1. **The bug lives in the thread configuration, not the code.** In a single-threaded build, publish/dispatch and `unsubscribe()` run on the same thread and cannot overlap; #897's "other thread's list" is the empty set. **Both hazards simply do not exist there.** The race is real only when an RX thread dispatches while another thread unsubscribes. Concurrency is injected per target (ADR-0079), so reclamation policy must be too.

2. **Segments are already safe.** `edge_view_t` snapshots hold **refcount clones** of the value/segment/rope handed to the callback, so the payload is pinned across dispatch. The **only** unmanaged leg in a snapshot is the raw `{fn, void*}` pair, and the `void*` is the **subscriber's own object** — this ADR is exclusively about that leg (and #897's parked node), never about payload bytes (need C, [ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md)).

3. **No runtime drain is free on the hot path.** To make `unsubscribe()` a *runtime* quiescence guarantee costs the dispatch path: a refcount bump on every `edge_view_t` snapshot, or a per-edge in-flight counter (an RMW per dispatch), or a hazard-pointer scan — and the hazard-scan shape is the very machinery that collapsed **×16.6 throughput at 24 threads** ([#635](https://github.com/avatarsd-llc/libtracer/issues/635)). Under the standing **latency-regression = REJECT** rule, none of these may sit on a read/write hot path unless measured free. The [ADR-0072](0072-one-reclamation-domain-graph-owned-and-backend-injected.md) supersession already ruled the generalized hazard domain out (+19%/+29% on the value-seam read), and stated the contract **does not generalize** to a reader holding a block across `fan_out`.

## Decision

Reclamation of a user-code seam is a **build-time-closed trait** (per [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md): types close at build time, instances stay runtime), selected on the ADR-0079 target axis. The grace point — the moment a retired `{fn, ctx}` (or parked node) is safe to free — is what each policy names:

| Policy | Grace point | Target | Hot-path cost | Re-entrant `unsubscribe()` |
| --- | --- | --- | --- | --- |
| `reclaim_strict` | `unsubscribe()` returns | WIDE / MCU, non-re-entrant | **zero** | forbidden (debug-assert if dispatch-depth > 0) |
| `reclaim_local` | this thread's dispatch stack unwinds to depth 0 | WIDE / MCU, re-entrant ok | one **non-atomic** inc+dec+branch **per publish** | supported; freed via an `on_released` signal / ctx-deleter |
| `reclaim_qsbr` | every thread has passed a quiescent state | MID / NARROW host | ~zero per dispatch; deferred batched free | supported (subsumed by the grace period) |

1. **`reclaim_local` is the default.** It makes the two targets behave **identically** — because the host build (`reclaim_qsbr`) already tolerates re-entrant unsubscribe via its grace period, `reclaim_strict` would make the *same app code* legal on the host and illegal on the MCU, a portability bug that surfaces only on the constrained target. `reclaim_local` buys parity for a **non-atomic** dispatch-depth counter (single-threaded ⇒ no cache-line bounce), bumped once per `fan_out` regardless of subscriber count.
2. **`reclaim_strict` is the opt-in zero-cost mode** for an MCU deployment that provably never unsubscribes from inside a dispatch and wants the bare notify-one-trivial-subscriber micro-case at literally zero cost. Re-entrant unsubscribe is forbidden and debug-asserted — a guard on a forbidden op, not a prose contract.
3. **`reclaim_qsbr` is the host build.** Quiescent-state reclamation (RCU/QSBR family): the read side is ~free because libtracer already has a natural quiescent point — an RX thread finishing a message and returning to its event loop holds no `edge_view_t` snapshot. A retired pair is reclaimed once every publisher thread has passed such a point.
4. **No universal prose backstop.** Every policy delivers *real* reclamation; the library owns the tracking and signals release (event-driven), so the embedder **never** polls in-flight state. The guarantee (or explicit non-guarantee) is stated on `subscription_t`, per policy.

### #897 maps onto the same seam
- **`reclaim_strict` / `reclaim_local` (single-threaded):** the bug is **absent** — no other thread exists, so a parked node sits on the one thread's list and is freed by that thread through a live resource. Zero cross-thread machinery.
- **`reclaim_qsbr`:** each thread **self-drains its own retired list at its own quiescent point**; the destructor never reaches across a live thread's list (the only thing the current code tried, and failed structurally, to do), and no `store()`-path atomics are added. The parked `shared_ptr<const rope_t>` is freed on its owning thread before that thread's arena is torn down.
- The `lkv_slot.hpp` relaxed-probe check-then-act (an orphan-push that can be missed even in the adoptable case) is a **separate genuine bug** — but **not** an independently fixable one, as this line originally claimed. Ruled with [#1037](https://github.com/avatarsd-llc/libtracer/issues/1037): every in-body candidate was refuted (the adopting `scan` races the *same* push, so re-probing after the ticket moves the window instead of closing it, and an orphan epoch reproduces the shape one level down). The property moved into the lifetime contract — [ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) §Erratum 8 now requires the injected resource to outlive every writer thread **and** a domain quiescence point — and `retire_and_flush`'s own promise weakened to "at slot death **or** at the next domain scan". This is the half of the lifetime story that stays true regardless of which policy above a target selects; `reclaim_qsbr`'s self-draining participants are what would make the quiescence point routine rather than the embedder's obligation.

## The theoretical best — shard, don't reclaim

To know "no reader is using X" one must either make readers announce themselves (hot-path cost), wait for a grace period (deferred free), or **never share X across threads** so the freeing thread is the only possible user. The third door is the ideal, reachable by architecture rather than a cleverer algorithm: **shard subscription (and hazard-slot) ownership per RX thread** — ADR-0079's NARROW taken to its end. If a subscription is only ever dispatched *and* unsubscribed by one thread, its unsubscribe is instantly quiescent (free immediately, `reclaim_strict` semantics), and a many-core host degenerates into N independent single-threaded problems. `reclaim_qsbr` is then needed **only** for the genuine residual — a subscription whose one publish fans callbacks across multiple threads. This is the "delete, don't shrink" reading: refcount/hazard machinery exists to make a cross-thread-shared mutable seam affordable; the better answer is to make that seam **not exist**, and keep QSBR only for what cannot be sharded.

## Considered options

- **Refcount the `{fn, ctx}` pair** (clone into every `edge_view_t` snapshot). Rejected as a default: an atomic RMW on the dispatch hot path — a latency regression unless measured free. It is the fallback a `reclaim_qsbr` build may adopt per-edge (no global scan, so no #635 collapse) only behind a pinned A/B.
- **Per-edge in-flight counter / epoch** that `unsubscribe` waits on. Rejected: two atomic RMW per dispatch plus a blocking `unsubscribe`.
- **Hazard-pointer scan.** Rejected outright — this is the #635 ×16.6 shape.
- **A universal runtime prose contract** ("the embedder must quiesce publishers before freeing"). Rejected: it pushes the tracking burden onto the app and makes the injected-arena lifetime unenforceable prose — the very thing #897 flags.

## Consequences

- The reclamation policy is a **per-target build-time trait** wired by default to `reclaim_local`; a target selects `reclaim_strict` (fold) or `reclaim_qsbr` (fan) without touching core, on the ADR-0047 / ADR-0079 axis.
- `subscription_t` gains a **stated guarantee** per policy (today it promises nothing); `reclaim_local`/`reclaim_qsbr` deliver it as an `on_released` signal or a ctx-deleter, never as a poll.
- Each policy carries its own **pinned latency A/B** on the notify path before it lands; a regression on any read/write hot path is a REJECT. The bare-notify-one-trivial-subscriber micro-case is the arm that justifies keeping `reclaim_strict`, and the [est] numbers in the #894 grill (~0.7–1.5 ns/publish host, ~12–25 ns/publish rv32 for the `reclaim_local` counter) must be replaced by CI numbers.
- A **reference article** (under `docs/reference/`) documents the grace-point model, the mechanism behind each policy, and the measured comparison.
- This ADR is about *when* a replaced/released block is freed; it composes with — and does not reopen — [ADR-0072](0072-one-reclamation-domain-graph-owned-and-backend-injected.md) (`collect()` + the immutability invariant) and [ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md) (*where* bytes come from). The hazard-node allocations are also one of #873's global-heap-bypass channels, moved onto the injected substrate there.
