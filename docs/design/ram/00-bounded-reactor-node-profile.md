# Bounded-Reactor Node Profile — design & program

> **Status:** design (pre-ADR). **Target:** strawberry-fw on ESP32-C6 (single-core RISC-V, lwIP,
> esp_http_server). **Author:** RAM-leanness grill, 2026-07-23. **Companion docs:**
> [`10-lwip-libtracer-seam.md`](10-lwip-libtracer-seam.md) (the lwIP seam) and
> [`20-zero-copy-and-flatten.md`](20-zero-copy-and-flatten.md) (the flatten question).
>
> **⚠ Second-pass errata (2026-07-23):** a 9-dimension adversarial re-audit corrected several figures and
> causal attributions in this doc — full ledger in
> [`30-second-pass-reconciliation.md`](30-second-pass-reconciliation.md). The thesis stands unchanged; the
> load-bearing fixes are applied inline below and marked **[errata]**.

This document is the spine: it states the problem, the design center, the measured current state,
and the committed multi-slice program to make a **unified transport→graph** node **RAM-lean on a
single-core MCU without touching HPC latency**. Companion docs go deep on the mechanisms it depends on:
[`10`](10-lwip-libtracer-seam.md) (the lwIP I/O seam), [`20`](20-zero-copy-and-flatten.md) (whether we
need to flatten), [`30`](30-second-pass-reconciliation.md) (the adversarial re-audit + errata), and
[`40`](40-buffer-chain-whole-analysis.md) (the whole wifi→graph buffer chain, home of the largest lever **B0**).

---

## 1. Problem

Running libtracer's graph/net plane alongside (and eventually replacing) strawberry-fw's legacy stack
costs **+37 KB idle heap over v1.8.0** on the Gorshok ESP32-C6 (measured: boot-free 216 KB → idle-free
63 KB; v1.8.0 idle-free ~101 KB — reconcile the internal 37-vs-38 KB rounding, both HIL-only). The largest
contiguous free block also collapses **84 KB → 44 KB** — a fragmentation symptom, not idle bytes.

> **[errata + PINNED 2026-07-23] This figure was the `can_en=0` bench image (both 12 KB transport threads).
> Maintainer decision: ship `can_en=1`, graph-over-CAN always.** At `can_en=1` the strawberry CAN domain
> bus owns the single TWAI controller and `make_can_link` steps aside (`can_link_esp.cpp:27-33`), so the
> libtracer CAN thread **does not exist** and the marginal re-anchors to **~25 KB, one transport thread**
> (the TCP d2d listener). *Graph-over-CAN therefore means the graph rides the existing domain bus* — a
> **domain-bus → router seam** on the domain bus's own (v1.8.0-shared) task, **not** a second libtracer
> TWAI link — which is the leaner composition (one TWAI user, no new thread). Building that seam is a
> strawberry-side design item. Consequence for the slice ledger: after **S1 (WS-for-btb)** deletes the
> lone remaining d2d listener thread, the production image has **zero dedicated libtracer transport
> threads** (CAN via the domain-bus seam, WS httpd-folded, d2d folded onto `/ws`) → marginal ≈ **13 KB**.

An exhaustive source audit + a live `/system/tasks` census established that **this is not
inefficiency** — libtracer is lean, and the RAM is the legitimate cost of standing up a whole
comms stack v1.8.0 never had. There is **no dormant lever hiding a large idle-heap win.** The
marginal decomposes (census-corrected) as follows. **[errata]** This is a *loose bottom-up estimate* that
totals ~40–47 KB against the ~37 KB *measured* marginal (rows are approximate and partly **shared with
v1.8.0**); read it as attribution, not a reconciled sum.

| Consumer | ~KB | Nature |
|---|---:|---|
| 2× transport `std::thread` stacks (CAN twai + TCP d2d), each `pcfg.stack_size = 12288` | ~24 | correctness-sized for **inline** `/unit` batch-apply (8 KB measured-overflowed); ~22 KB idle-unused. **[errata]** the CAN thread exists only at `can_en=0`; at `can_en=1` this row is ~12 KB |
| graph durable values (LKV control blocks + segments + bytes, on the default heap) | ~7.5 | lean-by-design; drives fragmentation |
| vertex tree + cold blocks (`vertex_t` ~72–88 B × ~50) | ~9 | already diet'd (#361/#380); insert-only |
| node service backends (auth/net/sys/ota/ctrl_batch/catalog) | ~3.5 | required seams |
| httpd task stack share (batch-apply floor) | ~3 | crash floor (4096 core-dumped) |

So the RAM goes to **(a) thread stacks sized for a deep inline transaction, and (b) heap graph
state on the default allocator.** Both are addressable *by composition and profile*, not by making
the core "more efficient."

## 2. The design center

Three commitments constrain every choice below:

1. **Unified transport → graph is a goal.** *Any* transport (WS, board-to-board, CAN) must reach the
   full graph, **including structural creation** (`/unit` batch-apply). We do **not** confine deep
   writes to one plane. This rules out the "defer batch-apply to a shared worker" idea — with a
   unified graph the deep path exists on every transport, and after folding board-to-board onto
   the WS server there are too few deep threads left for a worker to amortize (it goes net-negative).

2. **Latency-first / HPC must not regress.** libtracer is an RDMA-over-arbitrary-transport,
   latency-first design. We change **composition and profile**, and where we touch core we do it
   **profile-gated** so multi-core HPC hosts keep thread-per-connection + heap.

3. **The single-core fact is the lever.** The ESP32-C6 is **single-core**. Thread-per-connection
   therefore buys it **zero parallelism** — every transport thread is pure RAM overhead with no
   throughput return. An async **reactor** (one poll loop for all sockets) is *strictly* leaner on a
   single-core target **with no throughput cost**, because nothing was parallelizing. This is the crux
   that makes the whole program safe: the RAM-lean profile and the HPC-fast profile diverge exactly
   along the core-count axis. **[errata]** "No throughput cost" is exact; there is, however, a **bounded
   tail-latency tradeoff under concurrency** — the reactor services each ready fd *run-to-completion*
   (`transport_ws.cpp:527-528`), so a deep `/unit` apply head-of-line-blocks other ready sockets for its
   duration, which preemptive thread-per-conn does not. Slice 4 (bounded `/unit`) is the fix and should be
   sequenced *with* the reactor, not after it (recon doc §New-Gaps 3). Magnitude is HIL-only.

**Definition — the bounded-reactor node profile:** on a single-core target, run **one poll reactor**
for all sockets, back the graph value store and transport RX from **one bounded slab**, and keep the
deep-transaction **stack floor low** by not flattening (see companion doc 2). HPC targets select the
opposite profile (thread-per-conn, heap, contiguous-flatten) and are untouched.

## 3. Current thread & memory census (ground truth, 10.5.60.177)

From `bench/taskcensus.ts` (reads `/system/tasks`): 17 tasks, 66 KB total stack, **46.5 KB of
census-apparent stack headroom** (min-ever-free − 512 B margin). **[errata]** "census-apparent," not
"provably-unused": a FreeRTOS HWM is cumulative-worst-*since-boot* and only trustworthy for deep paths that
already fired — an unstressed board leaves MQTT-discovery/TLS/LVGL-redraw/httpd-large-request frames
unfired, inflating the figure. **A deep-path stress census is the single most load-bearing unmade
measurement; no stack shrink may be banked before it** (recon doc §New-Gaps 2). The two libtracer threads:

```
main(=libtracer std::thread)  stack 12288  used <600 B   → CAN twai dispatch
main(=libtracer std::thread)  stack 12288  used ~1004 B  → TCP d2d listener poll
```

Both are 12 KB purely so a CAN- or peer-delivered `/unit` batch-apply survives; both sit ~22 KB-empty
at idle. Everything else (wifi/lvgl/httpd/io_dispatch/…) is app-side and shared with v1.8.0.

The graph value store is `graph_t graph{&mr}` with `mr = heap_resource_t` (plain
`heap_caps_malloc/free`) and **the `value_backend_` argument omitted** → it defaults to
`heap_backend()`. Only the transport RX uses a bounded pool (`rx_pool` over a 4608 B static slab,
3×1472). So the "one slab, whole stack" design (ADR-0039/0060) is **half-wired**.

## 4. The bounded-reactor architecture (target state)

```
                 ┌───────────────────────────────────────────────┐
   lwIP sockets  │   ONE poll reactor (single-core)              │   ← WS server (httpd-folded) +
   ── WS ────────┤   poll() over {listen fd, peer fds, dial fds} │     WS-btb peers + WS client dials
   ── btb ───────┤                                               │     all on one loop, no per-conn thread
                 └───────────────────┬───────────────────────────┘
   CAN (not an fd) ── twai thread ───┤  (stays 1 thread; SHALLOW once /unit is bounded)
                                     │
                          ┌──────────▼──────────┐
                          │  graph + fwd_router │  ← rope-native, zero-copy (companion doc 2)
                          └──────────┬──────────┘
                                     │ draws from
                          ┌──────────▼──────────┐
                          │  ONE bounded slab   │  ← rx (WS+CAN) + LKV values + decode arena
                          │  (single-core pool) │     no per-peer heap, no fragmentation
                          └─────────────────────┘
```

Result on the C6: transport threads collapse from *N* to ~1 reactor + 1 shallow CAN thread; graph +
RX memory is bounded in one slab (no unbounded heap growth, no 84→44 fragmentation); the deep-thread
stack floor drops because the flatten (and its 4096-byte on-stack arena) is gone.

## 5. The four slices (committed, ROI order)

### Slice 1 — WS-for-board-to-board (standalone, −12 KB, zero core change)
Fold board-to-board **inbound** onto the httpd `/ws` server every board already runs for its web-UI
(`httpd_ws_link_t` services peers on the httpd task — **no dedicated thread**). Swap the d2d link from
`transport_tcp_server` (its own 12 KB poll thread) to the httpd-folded WS server; **delete the raw-TCP
transport** (flash too). Since a peer's `/unit` batch-apply then rides the httpd task (already sized
8 KB for exactly that), inbound d2d costs **no new thread**.
- **Caveat:** outbound *dials* still spawn a `posix_endpoint` thread in either model — Slice 2 fixes
  that. Arrange the mesh so boards are mostly *dialed-into* (publisher = threadless httpd) and dial out
  only for data they consume; WS is bidirectional, so one dial per edge carries both directions.
- **Governance:** none (composition). **Verify:** HIL — census shows one fewer 12 KB thread.

### Slice 2 — unify client dials onto the poll reactor (core, profile-gated)
`transport_ws_server` already runs **one** poll thread for all server peers (transport_ws.hpp:47);
only `transport_ws_client` dials each spawn their own `posix_endpoint` `std::thread`
(posix_endpoint.cpp:29). Add a **reactor** that owns a single `poll()` over all libtracer socket fds —
listen fds, accepted peer fds, **and** client-dial fds — so N dials collapse to **one** loop. CAN is
**not an fd** (a TWAI driver queue), so it stays one thread, but a **shallow** one after Slice 4.
- **Profile gate:** single-core selects the reactor; multi-core HPC keeps thread-per-conn (parallel).
- **Governance:** ADR (transport threading model). **Verify:** census — dial threads → 0.

### Slice 3 — one bounded slab + owning ingress (core + integration)
The seam audit ([`10-…`](10-lwip-libtracer-seam.md) §5, [`20-…`](20-zero-copy-and-flatten.md) §6)
sharpened this into an ordered sub-sequence — with an important correction: **libtracer is already
zero-copy on the single-link common path** (`materialize` is a refcount bump, not a copy), so the wins
here are *fragmentation* + *stack high-water*, not big idle-heap bytes.

- **3a — build the single-core crit-section bounded pool (ADR-0060 §2) — the unblocker.** `sync_pool_t`
  is a multi-core *spinlock* (a lower-priority slot-holder can't run while a higher-priority task spins →
  unicore deadlock). The needed variant is an interrupt-disable crit-section pool (ISR-safe), selected
  per-target as an ADR-0047 §2 module-set trait (host keeps heap/`sync_pool_t`). **UNBUILT; everything
  below depends on it.**
- **3b — WS owning delivery (highest structural ROI).** Recv the WS payload straight into a pooled
  `segment_t`, adopt it, `deliver` **owning** (the TCP/UDP shape). Kills the per-frame
  `new(nothrow)` (httpd_ws_link.cpp:332) and the O(n²) `asm_buf` reassembly — the ~27 KB/session
  allocation/fragmentation class — and hands the rope tier an owned segment.
  - **[errata]** `httpd_ws_recv_frame` demands **one contiguous buffer ≥ frame.len** (`httpd_ws.c:353-386`),
    so rope-chaining into slot-segments is legal **only at the RFC-6455 fragment boundary**. A large
    *unfragmented* frame (`kMaxFrameBytes=32768` peer-controlled vs 1472 B slots) forces a **big-slot class
    or a heap fallback** — an added per-target policy decision. Killing `new`/`asm_buf` still stands.
- **3c — pool the TX gather buffer.** The ESP WS TX gather-flatten (httpd_ws_link.cpp:488) is
  **fundamental within the threadless httpd seam** (async `httpd_queue_work` + contiguous send API) — do
  **not** chase removing it. Draw it from the slab to contain the OOM/fragmentation surface.
- **3d — rope-native branch/field decode + streaming decode → the 4096 stack arena.** The 4096-byte
  on-stack arena (graph.cpp:929) is the one always-paid removable cost, but it is **structure scratch,
  not a payload copy**, and `rope_cursor` alone does **not** remove it (rope_cursor is a byte *source*;
  the arena is structure *storage*). Removing it needs a **streaming, walk-callback-driven decode**; the
  RAM **relocates stack→pool (net-neutral)** but drops **~4 KB stack high-water** off the deepest task —
  and stack is the binding constraint here (the httpd task was bumped 4 KB→**8 KB** after a measured
  overflow). Pair it with a **node-counting pre-pass** to convert the #477 throwing-overflow `abort()`
  into a BACKPRESSURE soft-fail. Note: the `rope_cursor` migration is **mostly already shipped** (the FWD
  terminus migrated — `resolve_terminus_rope`); the header comment calling it an unfinished "⑤/⑥
  follow-on" now *lags* the code.
- **Governance:** ADR (single-core pool; value_backend/rx wiring; owning WS delivery contract); the
  branch-decode sink type is ratification-gated (ADR-0041 §2). **Verify:** HIL largest-block + idle +
  stack high-water.

### Slice 4 — per-unit-bounded `/unit` (strawberry, RFC)
Relax `/unit` from **whole-batch atomic** (stage-all → bind-all → rollback-all, the reason the whole
transaction is on one stack) to **per-unit atomic + backpressure** (apply one unit, commit, backpressure
the sender, next unit). This bounds the deep transaction's depth and peak memory to *one unit*, letting
the reactor/CAN stack floor drop toward the shallow `STORED_VALUE` path.
- **Trade:** loses cross-unit atomicity (a batch can partially apply). Per-unit atomicity is retained.
- **[errata]** Slice 4 does **not** lower the *stack floor* — the 12 KB is set by the 4096 decode arena
  (graph.cpp:929) + per-unit projection depth (`web_server.c:114`, ADR-0075/0088), **not** batch atomicity
  (apply loops unwind between units, so the floor doesn't scale with batch size). The **stack-floor win
  belongs to Slice 3d** (streaming decode kills the arena); Slice 4 lowers the *heap* staged-list peak and
  rollback scope, and — more importantly — is the **latency co-requisite of the reactor (S2)**: it chops
  the run-to-completion quantum to one unit, fixing the §2 head-of-line tradeoff. Sequence S4 **with** S2.
- **Governance:** RFC (the `/unit` wire/semantics contract changes). **Verify:** HIL stack high-water
  under a real multi-unit apply over CAN + WS.

## 6. Retirement (parallel track — **credible ~11.2–12.2 KB, but 0 B reclaimable *today***)
Independent of the above: shed the legacy `io_layer` + `event_bus` from the current image. v1.8.0 keeps
both, so removing them lowers *our* absolute idle heap while v1.8.0's stays put — a genuine
**delta-vs-v1.8.0** lever (see §7 for how it closes the parity gap). A dedicated file:line source audit
(2026-07-23, strawberry-fw) grounds it:

- **The number is honest and slightly conservative:** **11,229 B strict** (8192 B two task stacks +
  768 B queues + 2269 B `.bss` gen/free-id tables) → **~12,200 B resident** once FreeRTOS TCBs
  (~720 B) + queue control blocks (~152 B) + mutex pools (~800 B) are counted. **~72 % of the win is two
  4096 B dispatch task stacks.**
- **`io_dispatch` ⊂ `io_layer`, `evt_bus` ⊂ `event_bus`** — the two tasks are the *components' own*
  dispatch tasks, **not** separately retirable. `io_dispatch` dies when `io_layer_init()` stops being
  called; do **not** sum it beside `io_layer` (that double-counts ~4.7 KB).
- **0 B is reclaimable now — every byte is BLOCKED behind consumer migration** to the libtracer
  `trc_subscribe`/graph seam. Only `wg_client` is migrated (PR #26). This is a **migration program**, not
  a delete: ~14 `event_bus` pub/sub + ~15–20 `io_layer` producers/consumers must move first.
- **Already banked (0 forward B), don't credit:** `io_snapshot` (retired, no source, PR #69), `proto`
  (orphaned, no CMakeLists, unbuilt). **Keep, don't retire:** `io_outputs` (its 73 B `.bss` is the live
  ADR-0088 D1 egress seam) — only cut its `io_layer` coupling onto `io_egress_graph`.
- **`display_lvgl`'s hard `REQUIRES io_layer` blocks only the `.lvgl` image** (`CONFIG_APP_DISPLAY_LVGL`
  unset in prodnode), **not the prodnode delta** — `zigbee_drv` likewise CONFIG-gated off.

**Ordered sequence** (shallow→deep, so each delete unblocks the next): (1) `rm components/proto` [hygiene,
0 B]; (2) `io_snapshot` — done; (3) cut `io_outputs`→`io_egress_graph` coupling [0 B, unblocks io_layer];
(4) migrate `event_bus`'s 3 subscribers + invert ~11 publishers, then delete it [**~4.6 KB**, shallower →
first]; (5) migrate `io_layer`'s ~15–20 consumers + give the graph bridges native `trc_subscribe` fan-out,
then delete it, taking `io_dispatch` with it [**~6.7 KB**, the biggest chunk]. See
[`project_legacy_retirement_plan`] + [`project_migration_progress`].

## 7. Expected outcome & honesty

| Lever | Idle Δ | Fragmentation / peak | Core? | Bucket |
|---|---:|---|---|---|
| **B0 wifi+lwIP buffer right-size** | **~0 (peak/ceiling: −20…30 KB)** | ++ | no (sdkconfig) | absolute (shared) |
| S1 WS-for-btb | −12 KB (marginal → ~13 KB @ `can_en=1`) | + | no | marginal |
| S2 reactor (dials → 0) | −N×~4–12 KB (**N=0 today**) | + | yes (gated) | marginal |
| S3 bounded slab | small idle + | ++ | yes (gated) | marginal |
| S4 bounded `/unit` | heap peak ↓ + **latency (HOL) fix** | + | strawberry (RFC) | — |
| Retirement | ~−11.2–12.2 KB (**0 reclaimable now**) | — | no | delta (shared) |

> **[errata + whole-chain + config diff]** **B0 is the largest *peak/ceiling* RAM lever** (not an idle-heap
> lever) and it is **not a libtracer change** — it composes the C6's over-provisioned wifi+lwIP pools for its
> single-flow control-plane role (doc 4). **The v1.8.0 config diff (2026-07-23) settled B0's framing:** the
> v1.8.0 and current `sdkconfig.defaults` are **byte-identical** for every buffer knob and the idle-resident
> pools (`STATIC_RX=6`, `TCP_WND=5840`, `MAX_SOCKETS=12`) are **equal** — so the ~25 KB idle marginal is
> **genuinely libtracer, not buffer config**, and B0 does **not** move idle heap vs v1.8.0. B0 trims *dynamic*
> caps (`DYNAMIC_RX/TX`, `RX_BA_WIN`) that are **on-demand, not idle-resident** → it reclaims burst DRAM
> ceiling + largest-block-under-load + OOM-headroom. **Config-hygiene finding:** the *shipped* image drifted
> to `DYNAMIC_RX/TX=8`, `RX_BA_WIN=8` vs the `=6` its own defaults intend (IDF defaults won over a stale full
> sdkconfig) — B0's **zero-risk first step is to reconcile the shipped config back to 6/6**, then HIL-trim to
> 4/4. S4's stack-floor entry was reassigned to S3d; S2's idle Δ is **N=0** on the measured leaf. Retirement
> is **0 B reclaimable today** (migration-blocked), *shared-with-v1.8.0* bucket — it improves the **delta**,
> not the marginal. S1's −12 KB carries the LRU-eviction reliability tradeoff (recon §NG-4).

**Honest ceiling — now with a number.** At the pinned **`can_en=1`** image the marginal is **~25 KB**; **S1**
takes it to **~13 KB with zero dedicated libtracer transport threads**. The idle-heap gap against v1.8.0 then
closes on **one shared-baseline lever: retirement −~12 KB** — we shed legacy that v1.8.0 keeps, so *our*
absolute idle heap drops below v1.8.0's while it stays put. **B0 does *not* help here** (idle buffers are
equal to v1.8.0 — config diff confirmed); its win is peak/ceiling + fragmentation, not idle. So
**`can_en=1` + S1 + retirement ≈ idle-heap parity-to-slightly-below v1.8.0** *while adding the entire
graph/transport stack* — a credible path to "reclaim more RAM than v1.8.0," **contingent on the
migration-gated retirement actually landing** (0 B until consumers move). B0 + S3 then deliver the
**operational** win the program has always centred: **bounded, fragmentation-free memory** (fixing the
84→44 KB largest-block collapse under load) on a **single-core-optimal** node. The copy chain itself is
already near-optimal (doc 4) — no systemic copy-elision win remains.

## 8. Open questions (post-audit)
Several earlier open questions are now **answered** by the companion docs:
- ~~Does rope_cursor remove the flatten *and* the on-stack arena?~~ **Answered (doc 2 §3):** rope_cursor
  removes the multi-link *flatten* (mostly already shipped), but the *arena* is structure storage that
  needs a **separate streaming decode**; the walk has **no latency-vs-RAM tradeoff** (for single-link
  L=1 it ≈ span_cursor; vs `flatten` it wins on both). The arena win is **stack, not heap**.
- ~~Is riding esp_http_server the right seam?~~ **Answered (doc 1 §4):** yes on single-core — threadless
  (rides the SPA httpd task; a raw `transport_ws_server` costs a ~12 KB recv-task stack). The one cost is
  a **fundamental TX gather-flatten + queue hop**; contain it by pooling the gather buffer (3c).

Still open:
- **Reactor + CAN:** can TWAI alerts feed the poll loop (eventfd-style) to fold CAN in, or does it stay a
  separate shallow thread? (CAN is not an fd.)
- **Single-core pool internals:** interrupt-disable crit-section (docs' recommendation, ISR-safe) vs a
  lock-free MPSC free list — which fits {one reactor + one CAN thread + app writers} best?
- **`/unit` atomicity RFC (Slice 4):** is cross-unit atomicity ever relied on by a client, or is per-unit
  atomicity + backpressure a safe contract change? **[errata]** the strawberry httpd task is **8 KB**
  (adopting `httpd_ws_link_t` ctor, `libtracer_node.cpp:383`; `TASK_STACK_HTTPD=8192`) — `kHttpdTaskStack
  =12288` is dead code on-target (owning ctor only). The batch-apply depth (decode arena + projection) is
  what forces it there, and Slice 3d (not S4) is what lowers it.
