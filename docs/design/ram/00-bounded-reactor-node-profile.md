# Bounded-Reactor Node Profile — design & program

> **Status:** design (pre-ADR). **Target:** strawberry-fw on ESP32-C6 (single-core RISC-V, lwIP,
> esp_http_server). **Author:** RAM-leanness grill, 2026-07-23. **Companion docs:**
> [`10-lwip-libtracer-seam.md`](10-lwip-libtracer-seam.md) (the lwIP seam) and
> [`20-zero-copy-and-flatten.md`](20-zero-copy-and-flatten.md) (the flatten question).

This document is the spine: it states the problem, the design center, the measured current state,
and the committed multi-slice program to make a **unified transport→graph** node **RAM-lean on a
single-core MCU without touching HPC latency**. The two companion docs go deep on the mechanisms it
depends on (the lwIP I/O seam, and whether we need to flatten at all).

---

## 1. Problem

Running libtracer's graph/net plane alongside (and eventually replacing) strawberry-fw's legacy stack
costs **+37 KB idle heap over v1.8.0** on the Gorshok ESP32-C6 (measured: boot-free 216 KB → idle-free
63 KB; v1.8.0 idle-free ~101 KB). The largest contiguous free block also collapses **84 KB → 44 KB** — a
fragmentation symptom, not idle bytes.

An exhaustive source audit + a live `/system/tasks` census established that **this is not
inefficiency** — libtracer is lean, and the RAM is the legitimate cost of standing up a whole
comms stack v1.8.0 never had. There is **no dormant lever hiding a large idle-heap win.** The
marginal decomposes (census-corrected) as:

| Consumer | ~KB | Nature |
|---|---:|---|
| 2× transport `std::thread` stacks (CAN twai + TCP d2d), each `pcfg.stack_size = 12288` | ~24 | correctness-sized for **inline** `/unit` batch-apply (8 KB measured-overflowed); ~22 KB idle-unused |
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
   single-core target **with no latency cost**, because nothing was parallelizing. This is the crux
   that makes the whole program safe: the RAM-lean profile and the HPC-fast profile diverge exactly
   along the core-count axis.

**Definition — the bounded-reactor node profile:** on a single-core target, run **one poll reactor**
for all sockets, back the graph value store and transport RX from **one bounded slab**, and keep the
deep-transaction **stack floor low** by not flattening (see companion doc 2). HPC targets select the
opposite profile (thread-per-conn, heap, contiguous-flatten) and are untouched.

## 3. Current thread & memory census (ground truth, 10.5.60.177)

From `bench/taskcensus.ts` (reads `/system/tasks`): 17 tasks, 66 KB total stack, **46.5 KB of
provably-unused stack headroom** (min-ever-free − 512 B margin). The two libtracer threads:

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
- **3c — pool the TX gather buffer.** The ESP WS TX gather-flatten (httpd_ws_link.cpp:488) is
  **fundamental within the threadless httpd seam** (async `httpd_queue_work` + contiguous send API) — do
  **not** chase removing it. Draw it from the slab to contain the OOM/fragmentation surface.
- **3d — rope-native branch/field decode + streaming decode → the 4096 stack arena.** The 4096-byte
  on-stack arena (graph.cpp:929) is the one always-paid removable cost, but it is **structure scratch,
  not a payload copy**, and `rope_cursor` alone does **not** remove it (rope_cursor is a byte *source*;
  the arena is structure *storage*). Removing it needs a **streaming, walk-callback-driven decode**; the
  RAM **relocates stack→pool (net-neutral)** but drops **~4 KB stack high-water** off the deepest task —
  and stack is the binding constraint here (the httpd task was bumped 4 KB→12 KB after a measured
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
- **Governance:** RFC (the `/unit` wire/semantics contract changes). **Verify:** HIL stack high-water
  under a real multi-unit apply over CAN + WS.

## 6. Retirement (parallel track, ~10 KB, delta-relevant)
Independent of the above: retire the legacy `io_dispatch` task (4096 B) + `io_layer`/`event_bus`/
`io_snapshot`. v1.8.0 keeps these, so shedding them from the current image improves the *delta*.
See [`project_legacy_retirement_plan`].

## 7. Expected outcome & honesty

| Lever | Idle Δ | Fragmentation | Core? |
|---|---:|---|---|
| S1 WS-for-btb | −12 KB | + | no |
| S2 reactor (dials → 0) | −N×~4–12 KB | + | yes (gated) |
| S3 bounded slab | small idle + | ++ | yes (gated) |
| S4 bounded `/unit` | stack floor ↓ | + | strawberry (RFC) |
| Retirement | ~−10 KB | — | no |

**Honest ceiling:** a strict *beat* of v1.8.0 on idle heap is not guaranteed — libtracer is *adding*
functionality v1.8.0 lacks. But S1 + S2 + retirement plausibly reach **near/at parity**, and the
program's larger payoff is **bounded, fragmentation-free memory** (the operational OOM-risk symptom)
and a **single-core-optimal** node — which is the right target regardless of the parity scoreboard. The
~20 KB of app-stack headroom the census shows is a separate, v1.8.0-neutral absolute win.

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
  atomicity + backpressure a safe contract change? This also lets the **httpd task stack** (12 KB, sized
  for the in-call batch-apply) drop — the batch-apply depth is what forces it there.
