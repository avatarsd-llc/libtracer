# Reclamation of a user-code seam is a build-time-closed, per-target policy

<!-- status: accepted -->

Status: **accepted** (2026-08-15; proposed 2026-08-06, for [#894](https://github.com/avatarsd-llc/libtracer/issues/894) and [#897](https://github.com/avatarsd-llc/libtracer/issues/897)). **Amended 2026-10-12** ([Amendment 1](#amendment-1-2026-10-12-one-grace-mechanism)): `reclaim_local` is withdrawn, Decision 1's parity is redefined, Decision 3's fence becomes a build-time flavour, and the cost table below is replaced by measured numbers. The body stays as the record.

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

## Amendment 1 (2026-10-12): one grace mechanism

*(Ruled 2026-10-12, on the reclaim study of 2026-10-11/12.)* This amendment contradicts §Decision 1 (the default and its parity argument), §Decision 2's "debug-asserted", §Decision 3's "host build" framing and the cost table. The build-time-closed trait, §Decision 4 (no runtime prose contract, no poll) and the #897 self-drain stand. It records rationale only. No wire surface moves, and no page under `docs/spec/` or a §3 annex is touched. The rules land with the code changes named below and in the reference pages those changes edit.

The study prototyped six candidates on local branches against main (with [#2087](https://github.com/avatarsd-llc/libtracer/pull/2087) (frame bracket: a removed link is never freed while a frame is inside it)) and against [#2100](https://github.com/avatarsd-llc/libtracer/pull/2100) (asymmetric fence: no-fence bracket entry, `membarrier` on the reclaim side). It measured each one on three targets:

- **WIDE**: an x86-64 EPYC VM, bench cores 2–6, best of rounds, with an A/A guard of the same binary run twice.
- **MID**: an ESP32-C6 (rv32, one hart) built with ESP-IDF v6.0 at `-Os`. Its counts are static, read from objdump; there is no board.
- **NARROW**: the Cortex-M0 footprint build, plus the bracket compiled alone.

ARM64 was compiled to objects (clang 18, `armv8.3-a+rcpc`) and not run. Multi-thread rows whose own A/A guard failed are used only where the effect is many times the A/A spread, and they are marked so.

### 1. `reclaim_local` is withdrawn; `reclaim_qsbr` is the default on every target

`reclaim_local`'s grace point is the *unsubscribing* thread's dispatch depth. An app thread that unsubscribes is at depth 0, so it runs the release hook at once, while another thread that snapshotted the edges before the unsubscribe still holds `{fn, ctx}` and calls it afterwards. This is #894's hole in the default build. The study showed it with a deterministic reproducer (a latch, not a race): ASan reports a heap-use-after-free in the default build on main and on #2100, for an in-process publisher thread and for a router RX thread inside its frame bracket alike. It does so on 4 CPUs **and on 1 CPU**, so the hole needs no second core. On the ESP32-C6 the same window opens through FreeRTOS preemption (`configUSE_PREEMPTION`, time slicing at 100 Hz) and through a callback that blocks inside the fan-out (a mutex, or a remote edge's socket send). `reclaim_qsbr` was safe in every cell. The default policy now closes the hole. It is a correctness fix, and nothing else is claimed for it.

- **Tags.** `reclaim_qsbr` is the default on WIDE, MID and NARROW. `reclaim_strict` is the only opt-out. The tag set closes at two, and the traits that were equal to `kDefersToDispatchExit` in every tag (`kReentrantUnsubscribe`, `kGraceSpansThreads`) go with `reclaim_local_t`. That is a public API break, noted in the CHANGELOG of the change that removes the tag.
- **Parity, redefined.** §Decision 1 argued parity as "the hook runs on the caller's thread, the same on every target". That parity held on one CPU too, and so did the use-after-free. Parity now means: *the hook runs once, after every snapshot that could name the pair is gone, on whichever participant completes the grace period; the same on every target.*
- **What that changes for a release hook.** It may run on a receiving thread, at that thread's priority and on its stack, and after `unsubscribe()` has returned. It must therefore be short, thread-safe, non-blocking and `noexcept`. Posting the context to an app-owned queue is the ideal shape. A thread-affine context (one that must be released on its own thread) uses `reclaim_strict` and promises that no dispatch is live when it unsubscribes. The hook running on a receiving thread bends the receive-pays rule, within those limits.
- **§Decision 2's guard is not there.** "Forbidden, debug-asserted" was false at every sha studied: the assert reads a dispatch depth that `reclaim_strict` does not keep, so it is compiled to `false`. Re-entrant or cross-thread unsubscribe under `reclaim_strict` is forbidden and **not detected**. This amendment records that fact and does not rule on a debug-only depth.
- **Spelling.** "MID / NARROW host" in §Decision is the composition spelling that [ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md)'s amendment retired. This page now uses the target axis: NARROW is the Cortex-M0 class, MID is an RTOS MCU with one or a few cores, and WIDE is the host.

### 2. The entry fence is chosen at compile time

The policy (who may run a hook, and when) must not follow the core count, because the 1-CPU reproducer shows the race on one CPU. The *fence* may follow it, because the app never sees the fence. There are three flavours:

| flavour | selected when | reader entry / exit | reclaim side, before a concluding scan | proof |
| --- | --- | --- | --- | --- |
| `COMPILER` | `kSingleCore` | store + compiler barrier / compiler barrier + store | compiler barrier | single hart (below) |
| `FENCED` | **the default on Linux**, and every multi-core target without the opt-in | release store + `seq_cst` fence / release store | `seq_cst` fence | `qsbr.hpp` §qsbr_ordering |
| `MEMBARRIER` | compile-time opt-in on Linux (`kQsbrAsymmetricFence`), registration accepted | acquire epoch load + store / release store | `seq_cst` fence, `membarrier(PRIVATE_EXPEDITED)`, `seq_cst` fence | `qsbr.hpp` §qsbr_asym |

- **`kSingleCore` is a correctness fact, not a tuning knob.** It defaults to false. Each integration derives it from a build fact: ESP-IDF from `CONFIG_FREERTOS_UNICORE`, Zephyr from `!CONFIG_SMP`, and a bare-metal board preset sets it itself. It is never inferred from `kCacheLineBytes` (a padding knob an integrator may zero to save RAM) or from the target triple (an RP2040 is a dual M0+, an ESP32-P4 a dual rv32). Setting it on a multi-core build removes the fences that the proof needs, so it is a use-after-free. That is why it fails safe to false.
- **The single-hart argument and its limit.** One hart executes one instruction stream, and a context switch is a precise trap, so every access a descheduled task made is performed before the next task's first instruction. The compiler barrier keeps the announce above the reads and the reads above the offline store. The Dekker case split then holds in hart order. This is a hardware and compiler argument, not one inside the C++ abstract machine, the same argument as Linux's `CONFIG_SMP=n` `smp_mb()`. It holds only while nothing but that hart writes these words. The ESP32-C6's LP core does not run libtracer.
- **FENCED is the Linux default and MEMBARRIER is opt-in** because of what each one costs (§9.2). MEMBARRIER saves about 2–4 ns per outermost bracket. On the reclaim side it pays an IPI to every CPU running a thread of the process, 12.6–41 µs with 1–4 other threads running. The kernel's refusal of registration falls back to FENCED. That refusal is the one runtime bit, because no build can know it.
- **The fenced announce must be a release store.** GenMC (RC11 and IMM) finds a use-after-free with a relaxed announce and two brackets, or with a cell reused by a successor thread. It finds none with the release store, which #2087 carries. TSan cannot see this class (it ignores fences) and x86 cannot show it (TSO), which is the case for the model-check leg in §3.

### 3. Every refusal is counted; one park per graph; the grace state is a domain object

- **Refusals.** Every refusal to park a retired context is named and counted in one place ([#2117](https://github.com/avatarsd-llc/libtracer/issues/2117) (name and count every refusal; fix the silent `park_release` refusal at link teardown)). The study found two summed drop counters and a refused link park that was counted nowhere.
- **One park.** Every retired-context kind parks in one park per graph ([#2118](https://github.com/avatarsd-llc/libtracer/issues/2118) (one park per graph for every retired-context kind)). Links and seams share one park, one ripe prefix and one drain. It measured byte-identical on the `fan_out` path, at −8 CCN.
- **Domain object.** The grace state becomes a domain object with a `constinit` default instance ([#2119](https://github.com/avatarsd-llc/libtracer/issues/2119) (grace state as a domain object with a `constinit` default)). Library call sites keep the default, and a test can own a domain and drive participants by index. Its numbers are in §9.6: it is cheaper than the global static on every target measured. It also makes the shipped header model-checkable, because GenMC cannot model the current `thread_local` participant that has a destructor.
- **CI.** GenMC runs on the shipped header in CI ([#2120](https://github.com/avatarsd-llc/libtracer/issues/2120) (model-check the shipped qsbr header with GenMC)). A leg runs the suite under a seccomp filter that refuses `membarrier` ([#2121](https://github.com/avatarsd-llc/libtracer/issues/2121) (a seccomp leg that refuses membarrier so the fenced path runs on Linux)), so the refusal fallback runs on a multi-core host under real threads.
- **A per-graph domain is rejected.** Embedding the domain in `graph_t` costs +2,808 B per graph on x86-64 and +336 B on the ESP32-C6. It decouples two graphs only for the fenced entry, not for MEMBARRIER. It also reopens [ADR-0072](0072-one-reclamation-domain-graph-owned-and-backend-injected.md) §2 ("one domain instance per `graph_t`"), which that ADR's supersession retired. A graph-pointer variant buys nothing without it. One process domain stays.

### 4. Draining stays as it is: self-drain plus `collect()`

Each participant self-drains at its own outermost exit, and `collect()` drains the rest, which is §#897's shape. Under one policy that now holds on every target, not only on the opt-in. Two alternatives were measured and **deferred**, not adopted:

- **One park drained only by `collect()`, triggered at the outermost dispatch exit** (the study's C3 option O5). It deletes the per-thread park and 136 B of TLS per FreeRTOS task. On the old bracket it costs −39 % on a quiet hooked unsubscribe, and it frees on a library-chosen thread.
- **No drain on the data path** (C5's options O4 and O5). It gives an RX p99.9 of 0.84 µs under `reclaim_qsbr` unsubscribe churn, but it conflicts with §#897's self-drain.

The tail they would fix is the MEMBARRIER path's. Under unsubscribe churn paced at 10k/s on 4 cores, the RX p99.9 is **1.38 µs with FENCED against 111 µs with MEMBARRIER**. With FENCED as the Linux default (§2) the default no longer has that tail, so there is no case yet for moving the drain.

### 5. #2100 is reworked, not merged as opened

#2100 (asymmetric fence) lands as follows:

- **The default stays FENCED.** It keeps the fenced entry as the default on every target, with the release-store announce of §2.
- **MEMBARRIER is the opt-in.** It ships as the compile-time opt-in of §2.
- **One horizon per reclaim pass.** The fence is taken once per pass and outside `map_mutex_` (option E). This closes `drain_ripe`'s check-then-claim race as [#2104](https://github.com/avatarsd-llc/libtracer/pull/2104) (drain_ripe claims a slot before checking its epoch) does. #2104 must survive the rebase.
- **Single-flight on the MEMBARRIER path only.** A drain pass that is about to fence skips if another pass is already in flight. `retire()` and `collect()` stay ungated. Skipping only defers a free and never frees early.

As opened, #2100 put one `membarrier` inside `map_mutex_` on every `collect()` batch under the default policy. That made `collect()` ×21 slower (6.9 → 142 µs at 4 cores), held the lock ×34 longer at p50, and raised the RX p99.9 ×129 under unsubscribe churn (§9.3). **Break-even.** MEMBARRIER saves about 2 ns per frame bracket. One event that pays a `membarrier` (a hooked unsubscribe, or a `collect()` that frees) costs the churning thread 30–60 µs at 4 cores, plus the RX stalls. The opt-in pays back only above roughly **15,000–30,000 frames per reclaim event**. A read-mostly node is above that, and a gateway that churns subscriptions or links can be below it.

### 6. The edge pin: single-hart on MID, kept on host and ARM64; ADR-0075 holds

- **On MID under `kSingleCore`**, the edge pin's announce becomes a plain store with a compiler barrier ([#2122](https://github.com/avatarsd-llc/libtracer/issues/2122) (edge pin: single-hart announce under `kSingleCore`)). On the ESP32-C6 that is 8 → 4 fences per `fan_out` beyond the common three, for +54 B, with no syscall and no change on the host.
- **On host and ARM64 the edge pin is kept.** The study measured the alternative: delete the pin and retire displaced edge arrays under the frame or dispatch bracket. With other threads publishing, subscribe and unsubscribe ran 20–100× slower (−75 … −99.6 %), because every scan that might free pays a `membarrier`. Parked memory also lost its bound: up to 622 arrays (11.6 MB) at peak, and up to 121 still parked after the publishers stopped, against the pin's 2–5 arrays (≤ 287 KB). Without the syscall the reader saves nothing on x86 (`mfence` 4.97 ns against the pin's `xchg` 4.98 ns). On ARM64 the pin is already `dmb`-free.
- **[ADR-0075](0075-a-vertexs-edges-are-published-and-read-under-an-edge-pin.md) holds.** Its rejection of epoch reclamation for edge arrays rests on parked memory scaling with write rate × read-region length rather than staying a constant per slot. On today's numbers that rejection is stronger than written.

### 7. One trivially destructible per-thread record, and the `fan_out` bracket hoisted

The QSBR participant, the dispatch depth and the edge pin's index become **one trivially destructible `constinit` per-thread record**, `{depth, cell, pin}`. Under the default policy `fan_out` opens one bracket at its first remote edge, and every later edge nests in it ([#2124](https://github.com/avatarsd-llc/libtracer/issues/2124) (one trivially destructible per-thread record and the `fan_out` bracket hoisted)).

- **Why the record matters.** A `thread_local` with a destructor is guarded. The current bracket resolves it four times through an out-of-line guarded `self()`, which is where #2087's +9–12 % on `fan32` comes from. The fence does not cause it: forcing the fenced entry changes nothing. Nesting does not cause it either: with every edge nested the cost is still +11 %.
- **What the hoist adds.** The record recovers most of that cost and the hoist the rest. Hoisting alone recovers less than a fifth.
- **How the cell is released (R-cxa).** A C++ `thread_local` companion releases the cell at thread exit. It is constructed only on the cold claim path, so the hot path never touches a guarded object. Three alternatives were rejected:
  - A `pthread_key` destructor is a per-target option at most, and bare metal has none.
  - A registry sweep for dead owners has no portable liveness test: thread ids and FreeRTOS task handles are reused.
  - Never releasing cells makes every thread past `kQsbrParticipants` lifetimes overflow permanently, which is acceptable only for fixed task sets.
- **Why it is safe.** The record's storage stays valid through every other `thread_local`'s destructor. The companion releases the cell with a release store, and sets the record's cell to "none", so a later bracket takes the existing overflow path, which blocks every free. The next owner's acq_rel claim reads that release. That is the existing reuse argument, without the "last store" assumption.

### 8. The exit-order hazard

Today a `thread_local` that is destroyed after the participant, and that dispatches from its destructor, calls into the destroyed participant. It sees a stale index and announces on a cell that another thread may already have claimed. A scan can then read the other owner's quiescent state and permit a free while the exiting thread still reads. The edge pin has the same shape. This is [#2123](https://github.com/avatarsd-llc/libtracer/issues/2123) (a `thread_local` destructed after the participant can announce on a cell another thread owns). The app provokes it, not a peer, and its window is thread exit. §7's record closes it by construction, and #2124 carries a test that opens a bracket from a `thread_local` destructor after the cell is released.

### 9. Measured costs (replaces the cost table)

The table under §Decision is replaced by one table per decision, with one column per option. WIDE rows are x86-64, single thread on one bench core unless marked, p50 in ns, 10-ns timer quantum. MID rows are ESP32-C6 static counts. NARROW rows are Cortex-M0.

**No payload term.** Every row measured at 1 KiB, 4 KiB and 16 KiB shows the same picture as 64 B, in every arm of every decision. For example, `forward` is 120–130 ns flat from 64 B to 16 KiB in every arm. The bracket costs per frame or per edge, never per byte. Payload columns are therefore omitted below.

**Peak RSS** is equal within ±2 % in every arm of every decision on WIDE. Library RAM differences are bytes of TLS and `.bss`, given per row.

#### 9.1 Default policy (§1)

| row | `reclaim_local` (withdrawn) | `reclaim_qsbr`, FENCED (default) | `reclaim_strict` (opt-out) |
| --- | --- | --- | --- |
| cross-thread hooked unsubscribe during a delivery | **use-after-free**, on 4 CPUs and on 1 | safe | not protected (allowed only with no live dispatch) |
| WIDE `forward` / `publish1` / `publish8` p50 | 120 / 90 / 110 ns | 130 / 100 / 110 ns | 120 / 90 / 100 ns |
| WIDE hooked unsubscribe, idle | 200 ns | 220 ns | 200 ns |
| WIDE hooked unsubscribe/s, 4–5 busy publishers (A/A-fail set, effect beyond its spread) | 1.2–1.8 M | 0.94–1.5 M | 1.8 M |
| WIDE TLS per thread | 272 B | 16 B | 16 B |
| MID ESP32-C6 instructions per in-process publish bracket | ≈ 22 | ≈ 136 before #2124, ≈ 50 with the §7 record | 0 |
| MID TLS area per FreeRTOS task (on every task's stack) | 192 B | 64 B (**−128 B per task**) | 64 B [est] |
| MID ELF text | — | +144 B against #2100 | — |
| NARROW M0 footprint build (no graph links) | text 22,612 / data 268 / bss 7,416 | identical | identical |
| `graph.cpp` CCN (lizard, file total) | 771 | 755 | 755 |

#### 9.2 Entry fence (§2)

| row | `COMPILER` (`kSingleCore`) | `FENCED` (Linux default) | `MEMBARRIER` (opt-in) |
| --- | --- | --- | --- |
| WIDE outermost bracket, isolated | — (unsafe on more than one CPU) | 6.62 ns | 3.68 ns |
| WIDE `forward` / `publish1` / `publish8` p50 | 120 / 90 / 100 ns (a 1-CPU floor, not a configuration) | 130 / 100 / 110 ns | 120 / 90 / 100 ns |
| WIDE hooked unsubscribe, idle | — | 220–230 ns | 280 ns |
| WIDE hooked unsubscribe/s, 4–5 busy publishers | — | 0.94–1.5 M | **31–165 k** |
| WIDE unsubscribe churn 10k/s, 4 cores, RX p99.9 | — | **1.38 µs** | **111 µs** |
| WIDE link churn, 64-entry `collect()`, 4 cores, p50 / `map_mutex_` hold p50 | — | 4.9 µs / 840 ns | 142 µs / 33.2 µs (as opened; §9.3) |
| WIDE `membarrier` per call, with 1 / 3 / 4 other threads spinning | — | — | 12.6 / 31.2 / 40.9 µs |
| MID ESP32-C6 fences per outermost bracket | **0** (`sw` / `sw`) | 3 (`fence rw,w` + `sw` + `fence rw,rw` / `fence rw,w` + `sw`) | n/a (no `membarrier`) |
| NARROW M0 bracket | no `dmb` | `dmb` on entry and on exit | n/a |
| ARM64 entry / exit (codegen only) | `ldr` + `str` / `str` | `ldr` + `stlr` + `dmb ish` / `stlr` | `ldapr` + `stlr` / `stlr` |

#### 9.3 #2100's reclaim side (§5)

| row | #2100 as opened (per-slot `membarrier`, under the lock) | E: one horizon per pass, outside the lock | E + single-flight (MEMBARRIER only) | FENCED fallback, same binary |
| --- | --- | --- | --- | --- |
| WIDE unsubscribe churn 10k/s, 4 cores: RX p99.9 | 111 µs | 130 µs | 7.3 µs | 1.38 µs |
| … RX throughput | 5.20 M/s | 4.53 M/s | 10.94 M/s | 8.71 M/s |
| … unsubscribe p99 | 166 µs | 169 µs | 54 µs | 2.59 µs |
| … `membarrier` per unsubscribe | 2 | 2 | 1 | 0 |
| WIDE same, 5 cores, 1 / 4 / 16 KiB: RX p99.9 | 155 / 151 / 174 µs | 32 / 224 / 209 µs | 15.7 / 6.3 / 7.8 µs | 1.18 / 1.59 / 1.29 µs |
| WIDE link churn, 64-entry `collect()`, 4 cores: p50 | 142 µs | 34.6 µs | = E | 4.9 µs |
| … `map_mutex_` hold p50 / max | 33.2 µs / 983 µs | 30 ns / 17 µs | = E | 840 ns / 42 µs |
| `drain_ripe` check-then-claim race | present (needs #2104) | closed | closed | per its base |
| MID ESP32-C6 `collect()` fences | 67 per entry, under the lock | 35 per call, outside the lock (+276 B flash) | n/a (no `membarrier`) | 67 per entry before E |
| NARROW M0 footprint | identical | identical | identical | identical |

#### 9.4 Edge pin (§6)

| row | pin kept | pin with single-hart announce (`kSingleCore`, #2122) | pin deleted, arrays retired under the bracket |
| --- | --- | --- | --- |
| WIDE barrier per `fan_out` | 1 `xchg` (4.98 ns) | n/a on a multi-core host | 0 with `membarrier`; 1 `lock or` fenced (4.97 ns) |
| WIDE subscribe+unsubscribe pairs/s, 1 publisher | 988 k | n/a | 37–41 k |
| WIDE same, 32 subscribers, 4 publishers, 100 µs callbacks | 1.93 M | n/a | 11–87 k |
| WIDE parked arrays, peak / after publishers stop | 2–5 / 0 | as kept | up to 622 (11.6 MB) / up to 121 |
| MID ESP32-C6 fences per `fan_out` beyond the common three | 8 | **4** | 4 outermost, 1 nested |
| MID image against #2100 | 0 | +54 B | −386 … −1,062 B |
| NARROW M0 reader sequence | 7 `dmb` | 3 `dmb` | 3 `dmb` |
| ARM64 reader | `ldar` / `stlr`, 0 `dmb` | n/a | 0 `dmb` asymmetric, 1 `dmb ish` fenced |

The churn rows failed their own A/A guard (scheduling-bound). Their effects (75–99.6 %) lie beyond its maximum in every row with a publisher running, and two runs agree.

#### 9.5 Per-thread record (§7)

| row | today's guarded participant | record (R-cxa) | record + hoisted `fan_out` bracket (ruled) |
| --- | --- | --- | --- |
| WIDE x86 instructions, outermost / nested bracket | 44 / 20 | 40 / 16 | 40 / 16 |
| WIDE `fan32` instructions per write, against main before #2087 | +9.1 % | +3.6 % | +1.5 % |
| WIDE `fan32` p50, default policy, against the same | +11.8 % | +4.9 % | +4.6 % |
| WIDE `-fPIC` `.so` outermost bracket (`__tls_get_addr` sites) | 11.2 ns (10) | 4.0 ns (2) | 4.0 ns (2) |
| MID ESP32-C6 outermost bracket: instructions / tp accesses / `self()` calls | 103 / 8 / 4 | 51 / 2 / 0 | 51 / 2 / 0 |
| MID nested bracket; each remote edge after the first | 48 instructions; 103 instructions + 3 fences | 18; 51 + 3 fences | 18; **18, 0 fences** |
| MID flash against #2100 / TLS per task | 0 / 192 B | −56 B / 185 B | +64 B / 192 B |
| NARROW M0 (graph-bearing harness) outermost: instructions / `__aeabi_read_tp` | 115 / 8 | 58 / 2 | 58 / 2 |
| ARM64 outermost / nested instructions | 52 / 25 | 41 / 17 | 41 / 17 |

#### 9.6 Grace-state domain (§3)

| row | process-global static (today) | domain object, `constinit` default (#2119) | per-graph embedded domain (rejected) |
| --- | --- | --- | --- |
| WIDE outermost bracket, MEMBARRIER / FENCED | 3.68 / 6.62 ns | 2.71 / 6.63 ns | 2.71 / 6.40 ns |
| WIDE `fan32` kfps, default policy | 558 | 588 (+5.4 %) | 580 |
| WIDE `receive` 64 B kfps | 2,892 | 2,815 (within A/A) | 2,568 (−8 … −13 %, unexplained) |
| WIDE `sizeof(graph_t)` | 904 B | 904 B | 3,712 B |
| MID ESP32-C6 bracket hot path: instructions / guarded TLS reads | 96 / 4 | 54 / 2 | ≈ 54 / 2 [est] |
| MID RAM | — | +32 B `.bss` | +336 B per graph |
| NARROW M0 isolated bracket, static instructions | 126 | 114 | 113 |
| graphs coupled through one domain | yes | yes | no for FENCED, yes for MEMBARRIER |
| GenMC on the shipped header | cannot run | runs | runs |

### Knock-on, carried by the changes above (not by this page)

[ADR-0072](0072-one-reclamation-domain-graph-owned-and-backend-injected.md)'s supersession wording ("no `membarrier`, no announce/scan") stopped being true when #2087 merged. `docs/reference/` states `reclaim_local` as the default and the router as untouched by the bracket. CONTEXT.md's "Reclamation domain" entry, `reclaim.hpp`'s trait table and the CI legs that duplicate the default (`reclaim_qsbr` build-test and TSan) all change. Each of these is edited by the change that makes it false, so that no page describes code that has not landed.
