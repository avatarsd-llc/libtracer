# Second-Pass Reconciliation — RAM-leanness plan

> **Status:** reconciliation / errata (pre-ADR). **Date:** 2026-07-23 (same day as the primary set).
> **Method:** a 9-dimension adversarial re-audit of docs [`00`](00-bounded-reactor-node-profile.md) /
> [`10`](10-lwip-libtracer-seam.md) / [`20`](20-zero-copy-and-flatten.md) against the *current* source
> in both repos (libtracer `core/` + `integrations/`, strawberry-fw `components/`), with an independent
> verify pass adjudicating every challenge, then a synthesis. 27 agents, ~1.85 M tokens.
> **This doc is the correction ledger.** Where it disagrees with 00/10/20, this doc wins; the load-bearing
> factual fixes have also been applied inline to doc 0.

---

## Verdict in one paragraph

**The substantive thesis survives the second pass intact.** libtracer is structurally *lean* — SBO vertex
names, lazy `children_`/`ext_`, a single static rx slab, chaining-not-memcpy ropes, refcount-bump
single-link `materialize`, an *iterative* (not recursive) pool-drawn decode — with **no large dormant
idle-heap lever**. The +37 KB C6 marginal is legitimate comms-stack cost dominated by **two
image-conditional 12 KB transport threads**, and the path forward — **compose the C6 for its role +
retire legacy, don't change the core** — is sound. Ten load-bearing CONFIRMs stand on source; two attacks
were defeated (mbedtls-omission is *correct*; the bounded slab is a *legit injected bound*, and the #477
`abort` is a separate decode arena, not slab exhaustion). **What must be rethought is almost entirely
documentation honesty, causal attribution, one composition contradiction, and one sequencing fix — not
the design.**

---

## What STANDS (re-confirmed on current source)

| # | Approach | Anchor |
|---|----------|--------|
| S3/S4 | Exactly **two** transport `std::thread`s at 12288 (CAN twai + TCP d2d); rx_pool over a 4608 B `.bss` slab; `graph{&mr}` defaults `value_backend_` to heap | `libtracer_node.cpp:359,432,526`; `can_link_esp.cpp:35`; `graph.hpp:876`, `graph.cpp:292` |
| L3 | `vertex_t` ~72–88 B **on-target** (RV32) × ~50, already diet'd | `path.hpp:150` (SBO=16), `vertex.hpp:789` (lazy children), `:1753` (lazy ext) |
| Z1–Z4/Z6 | Rope = chaining not memcpy; single-link `materialize` is a refcount bump; removable flattens are multi-link-only; the 4096 arena is **structure** storage (net *stack* win); decode is **iterative** (refutes "callback frame costs equal stack"); `resolve_terminus_rope` wired | `rope.hpp:148-151,184-218`; `transport_ws.cpp:226-239`; `graph.cpp:924-931`; `fwd_router.cpp:365-390` |
| Z5b/Z7 | ESP WS TX gather is fundamental within the httpd seam; per-frame `new(nothrow)` RX exists; `asm_buf` reassembly is O(n²); #477 needs a node-counting pre-pass | `httpd_ws_link.cpp:488-556,332,119-131`; `graph.cpp:924-931` |
| A2/A3/A4 | `transport_ws_server` runs **one** poll thread for all peers; client dials each spawn a thread; the only built pool is a **multi-core spinlock** that priority-inverts a unicore kernel; CAN is a TWAI queue, not an fd | `transport_ws.cpp:201,565`; `posix_endpoint.cpp:29`; `mem_pool.hpp:151,159-164`; `twai_link.cpp:46,187,217` |
| A5/A7 | Riding `esp_http_server` adds **zero tasks**; `httpd_ws_recv_frame` **can** recv into a caller-provided pooled buffer (3b feasible); the TX gather has no scatter form | `httpd_ws_link.cpp:214-237`; `httpd_ws.c:368` |
| S1/S3/S5 | WS-for-btb deletes the `transport_tcp_server` 12 KB thread (**−12 KB, zero core change**); every board runs the httpd `/ws`; `TCP_LISTEN` default-y + `CAN=y` in prodnode | `libtracer_node.cpp:431-440`; prodnode Kconfig |
| C6 | The two 12 KB threads **cannot** simply be shrunk to idle+margin now — idle HWM (<600 B/~1004 B) is **not** deep-path HWM; the 8 KB overflow makes Slice 4 a **hard prerequisite** | `libtracer_node.cpp:349-357` |
| C2 | Omitting an mbedtls/TLS lever is **correct** — the request plane never touches TLS (`/ws` on non-TLS :80, HTTPS compiled out, d2d raw TCP) | `sdkconfig.prodnode:1442` |
| C3 | The one bounded slab is a **legit injected-resource bound**, not a synthetic limit; value-backend exhaustion soft-fails as backpressure; the #477 `abort` is a *separate* decode arena | `graph.cpp:923` (+8 siblings); `kRxRegion` in the strawberry composition root |

---

## Corrections applied (documentation honesty & attribution — **not** design changes)

Ordered by impact. ✎ = also fixed inline in doc 0; ▤ = corrected here only.

1. **✎ [MED] httpd task is 8 KB adopted, not 12 KB** (doc 0 §5-3d, §8; doc 10 §4.3). Strawberry uses the
   *adopting* `httpd_ws_link_t` ctor (`libtracer_node.cpp:383`) which never sets `cfg.stack_size`; the
   adopted :80 task is **8 KB** (`TASK_STACK_HTTPD=8192`) and already carries the `/unit` batch-apply.
   `kHttpdTaskStack=12288` is dead code on this target (only the *owning* ctor uses it,
   `httpd_ws_link.cpp:185`). The S1 −12 KB is unaffected (the deleted thread is the d2d listener, not httpd).

2. **✎ [MED] The +37 KB is a `can_en=0` bench image; state the condition on every §1 figure.** With
   `can_en=1` (the production domain-bus leaf), `make_can_link` returns `nullptr` — the strawberry CAN
   domain bus owns the single TWAI controller (`can_link_esp.cpp:27-33`) — so the libtracer CAN thread
   **vanishes** and the marginal is **~25 KB, not 37**. See the composition contradiction in New-Gaps #1.

3. **✎ [MED] Drop the unqualified "no latency cost."** The single reactor thread services each ready fd
   **run-to-completion** (`transport_ws.cpp:527-528`), so a deep `/unit` apply head-of-line-blocks every
   other ready socket for its full duration; FreeRTOS preemptive slicing gives thread-per-conn a
   tail-latency-under-concurrency property the reactor lacks. Correct framing: **"no *throughput* cost on
   single-core; a bounded *tail-latency* tradeoff under concurrent deep transactions."** Magnitude is
   HIL-only. Reactor direction, RAM win, and throughput claim all stand.

4. **✎ [MED] Retirement decomposition mis-counts** (doc 0 §6). Two errors: (a) `event_bus` runs a
   **second** live 4096 B task `evt_bus` — task-stack yield is **8 KB from two tasks**, not 4 KB from one;
   (b) `io_snapshot` is **already retired** (no component dir) → zero forward savings. Corrected forward
   ~10 KB = io_dispatch + evt_bus tasks (8 KB) + io_layer/event_bus queues & subscriber tables (~2 KB).
   Keep the migrate-consumers-first framing (`io_layer` is a hard `REQUIRES` of `display_lvgl`).

5. **▤ [MED] §1 decomposition table over-sums.** Bottom-up it totals **~40–47 KB** against a 37 KB
   *measured* marginal — every variant overshoots by 3–10 KB because rows are loose and partly **shared
   with v1.8.0**. Read it as "~37 KB measured; bottom-up estimate ~40–47 KB," not a reconciled sum. Do
   **not** raise the vertex row — `sizeof(vertex_t)` is ~72 B on RV32 (the "~96 B" header comment is the
   64-bit host layout), so the plan's ~72–88 B basis is right on-target.

6. **▤ [MED] Ingress-copy anchor conflation** (doc 10 §1 / C16). `rope.cpp:12` is `flatten()` — the
   **multi-link coalesce** path — not the ingress copy; it never fires on single-link ingress. The real
   ingress copies are `transport_tcp.cpp:229` (recv straight into the owned segment — already the
   RDMA copy-once optimum, **zero** user-space copies) and `httpd_ws_link.cpp:332-338` (ESP WS: owned
   `new[]` then recv). Doc 10's C1 already anchors this correctly; the prose lines 11/13 and C16 that call
   `rope.cpp:12` "the one ingress ownership copy" are the ones to re-read as *multi-link durable-store
   flatten (C11)*. The concept — ingress copy is fundamental for BSD sockets — stands.

7. **▤ [MED] §4 reactor diagram is host-shaped.** On the C6, Slice 1 folds inbound btb onto
   `esp_http_server`'s `/ws`, so the listen fd + accepted peer fds live inside httpd's private `select()`
   (`httpd_main.c:296`) with **no** API to export them or inject libtracer's dial fds. The device therefore
   **inherently retains three loops**: httpd select (listen+peer) + a **dials-only** reactor + the CAN
   dispatch thread. The unified `{listen,peer,dial}` loop is **host-only**. Re-scope the device reactor to
   dial fds only (see New-Gaps #8: on the measured non-dialing leaf, S2 collapses **N=0** threads today).

8. **▤ [MED] Slice 3b collides with httpd's contiguous-recv constraint.** `httpd_ws_recv_frame` demands
   **one contiguous buffer ≥ frame.len** and refuses a smaller `max_len` (`httpd_ws.c:353-386`). So
   rope-chaining a large frame into slot-segments is legal **only at the RFC-6455 fragment boundary** (each
   CONTINUE frame is its own recv). With `kMaxFrameBytes=32768` peer-controlled vs 1472 B rx_pool slots,
   3b forces a **big-slot class or a surviving heap fallback** for large *unfragmented* frames — an
   unspecified per-target policy decision that must be added. Killing `new(nothrow):332` + `asm_buf` O(n²)
   still stands.

9. **▤ [MED] Slice 4 does not lower the stack floor — re-attribute it.** The 12 KB is set by (a) the fixed
   4096-byte on-stack decode arena (`graph.cpp:929`) + (b) per-unit projection call depth
   (`web_server.c:114`, ADR-0075/0088 projection backends), **not** whole-batch atomicity — the apply loops
   unwind between units, so the floor doesn't scale with batch size. Consequence: **Slice 4 lowers the
   heap-side staged-list peak and rollback scope; the *stack-floor* win belongs to Slice 3d** (streaming
   decode kills the arena) + trimming projection depth. The descriptive half of S2 (`/unit` is a
   HANDLER-role atomic stage/bind/rollback batch) stands verbatim (`ctrl_batch.cpp:1-12,681`).

10. **▤ [LOW] "provably-unused" / "absolute win" overstate certainty** (doc 0 §3, §7). A FreeRTOS HWM is
    cumulative-worst-*since-boot* — trustworthy only for deep paths that *actually fired*; an unstressed
    board leaves MQTT-discovery/TLS/LVGL-redraw/httpd-large-request frames unfired, **inflating** the 46.5 KB
    figure. Downgrade to "census-apparent headroom, pending a stress census" and stop folding the ~22 KB
    non-reclaimable deep-path reserve into the headline. (Not a program change — the ~20 KB is fenced
    outside the 4-slice program.)

11. **▤ [LOW] Scope "no dormant lever" to "no *large* lever."** Structural leanness is source-confirmed, but
    the strong global "nothing left" is a whole-image census conclusion. The one identified seam
    (`value_backend` heap default) is ~1–1.5 KB idle; its payoff is **fragmentation, not idle bytes**.

12. **▤ [LOW] The bounded pool is an *unbuilt* latency lever.** The ~120 ns-vs-~2 µs figures are design
    prose — the single-core crit-section pool doesn't exist yet (only the wrong multi-core spinlock). Re-word
    doc 20's latency claims as design-target, not measured.

13. **▤ [LOW] Add an lwIP note; mark S2 delta N=0 on the measured board.** Listen-socket state is sub-KB
    idle; window/recvmbox pools are *peer-scaled*, not idle-resident — don't fold them into the idle figure.

14. **▤ [LOW] Reconcile the +37 vs +38 KB internal disagreement** (doc 0 §1 says +37; its own 63→~101 and
    the memory both give 38). Pick one; both are HIL-only.

---

## New gaps to close **before banking any number**

Prioritized. The first two are blockers.

1. **⛔ Composition contradiction — pin the shipped image.** The whole S-lever ROI table is calibrated on
   the `can_en=0` bench image (both 12 KB threads). But graph-over-CAN (the maintainer's "every board does
   the graph over CAN" constraint) **requires** the libtracer CAN attach, which *steps aside* when the
   strawberry domain bus is enabled — **the two planes are mutually exclusive on one TWAI controller.** A
   single unified image cannot run both. **Decide:** ship `can_en=0` (graph-over-CAN, ~37 KB, both threads)
   **or** `can_en=1` (domain bus, ~25 KB, one thread), and re-calibrate every figure against that decision.
   A reader optimizing against 37 KB may be optimizing an image that never ships.

2. **⛔ The single most load-bearing measurement is unmade: a deep-path (stress) stack HWM census.** Every
   stack-sizing decision (Slice 4 floor-drop, the 46.5 KB / ~20 KB headroom) rests on an **idle** census,
   which provably under-reads — the 8 KB batch-apply overflow was *invisible at idle*. A shrink to
   idle+margin reproduces the exact 4096→8192 crash the plan cites. **Run `strawberry-hil-stress`** (drive
   `/unit` over CAN+WS, LVGL redraw, httpd large-request, wifi churn, MQTT discovery) for trustworthy
   per-task HWM, and gate Slice 4 + the headroom claims on it.

3. **Re-sequence: couple S2+S4 (S4 is the *latency* co-requisite of S2, not a RAM afterthought).** Slice 4
   (per-unit atomicity) chops the reactor's run-to-completion quantum from whole-batch to one-unit, yielding
   between units — it is the natural fix for the §3 head-of-line regression. Shipping the reactor (S2)
   **without** bounded `/unit` (S4) is exactly the worst window for cross-plane HOL blocking. Given the
   Zenoh/ROS latency-first positioning, surface S4 alongside S2.

4. **S1 reliability tradeoff — not pure upside.** Folding d2d peers onto the shared 8-socket httpd pool
   (`max_open_sockets=8`, `lru_purge_enable=true`) means a user hard-refreshing the web UI can **LRU-evict a
   live d2d mesh peer** (`WS_MAX_PEERS=4`) — a regression the raw-TCP :47301 listener (own listen socket)
   doesn't have. Weigh it against the −12 KB; consider reserving sockets or a separate handler for d2d.

5. **`kMaxFrameBytes=32768` is a synthetic-limit candidate** (RFC-0006/0007, ADR-0051). Its own comment
   justifies it as a heap-exhaustion guard "because borrowed delivery heap-allocates per receive" — which
   **Slice 3b pooling removes**, dissolving the rationale. 3b is the moment to make the frame ceiling = the
   pool's contiguous-slot capacity (bound = injected slab), or justify the cap in ADR review.

6. **Profile-gate is real; its artifacts are ~90% unbuilt.** The gate mechanism (`security_acl.hpp` CMake
   `#if` + constexpr module-set traits; core-count is per-target-fixed → ADR-0047-legal) exists, but **no
   reactor/poll class exists anywhere in `core/`** and the single-core crit-section pool is only a recorded
   follow-up (`mem_pool.hpp:106-109`). Split the ledger claim into "gate = expressible (confirmed)" vs
   "single-core profile artifacts = UNBUILT," and specify the reactor-vs-thread-per-conn fork as a **clean
   module-set trait** over the shared `posix_endpoint_t`/`transport_ws_server`, not an `#ifdef` split.

7. **Broadcast TX multiplies the "fundamental" gather by peer count.** A large snapshot (~12.7 KB composed
   root) to N subscriber tabs = N simultaneous `new[total]+memcpy` inside the per-fd broadcast loop
   (`httpd_ws_link.cpp:582-597`) — the same co-resident-heap-spike class that caused the OOM abort, on the
   fan-out axis. Add fan-out amplification to the TX cost model; it's a latency **and** heap-peak concern for
   the RDMA-shaped N-subscriber case the design targets.

8. **Reactor device outcome is N=0 today.** The census has **zero** dial threads (the two 12 KB threads are
   CAN twai + TCP d2d *listener*), and on the C6 the reactor can only fold *outbound* dials. So on the
   recommended "mostly dialed-into" leaf, **S2 collapses nothing** — its value is latency/threadless-mesh
   scaling for boards that dial many peers, not idle-heap on prodnode. Mark the §7 S2 delta N=0 / load-
   dependent, and don't schedule S2 ahead of higher-yield levers for a non-dialing node.

9. **ISR-jitter of the (unbuilt) crit-section pool is unexamined.** The reactor RX-allocs in its hot loop
   and subscribers `destroy()` on last-ref drop (once per **segment** reclaimed in a rope chain); both
   `portENTER_CRITICAL`, briefly blocking **all** ISRs (TWAI RX, lwIP) on the single core. For a latency-first
   RDMA competitor an unbounded interrupt-latency window is exactly the axis to pin — add a worst-case bound
   and validate on-device once the pool is built.

10. **The −10 KB retirement lever is verified by no dimension** — it lives in strawberry-fw legacy code none
    of the `ram/` docs audit, yet it's ~27% of the gap and the second-largest single lever. Do a dedicated
    source audit (io_layer/event_bus/io_dispatch, file:line) before summing −10 KB into any parity claim, and
    correct the task attribution (8 KB from two tasks + ~2 KB queues/tables).

11. **#477 pre-pass adds a second full O(structure) TLV walk** on the branch-write hot path (count-walk +
    `decode_into`'s walk), on the latency-first httpd task. Rank-1 **streaming decode subsumes it** (no count
    needed if the node array never materializes) — prefer streaming where feasible; note the double-walk cost.

12. **WS TX fragmentation is an unexplored alternative to the "fundamental gather"** — `httpd_ws_send_frame_async`
    accepts `fragmented=true`, so ⑨ is "fundamental *given one WS message per reply*," a framing choice, not a
    hard API limit. (Likely a net loss — reintroduces receiver-side O(n²) — but stop calling it absolute.)

13. **Z6 scope:** "terminus migrated, no flatten" is true only for **decode/resolve** — `resolve_terminus_rope`
    still performs an ownership **store** copy (`own_tlv`) for referenced writes (`fwd_router.cpp:376-379`),
    because the ADR-0042 §3 referenced store needs a contiguous frame view a scatter-gather rope lacks. This
    is the fundamental ⑤ copy; keep the "multi-link stores are not zero-copy" scoping explicit.

---

## Bottom line

Nothing here invalidates the program. The four slices, the retirement track, and the compose-not-change-core
strategy all stand. The corrections are **wording, attribution, one composition decision (pin `can_en`), one
sequencing change (couple S2+S4 for latency), and two mandatory measurements** (config-pinned HIL heap read
+ a deep-path stress census) that must land before any parity number is banked.
