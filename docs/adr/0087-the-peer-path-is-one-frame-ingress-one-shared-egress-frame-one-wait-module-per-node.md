# The peer path is one frame-ingress module, one shared egress frame, one wait module per node and one peer record per peer

<!-- status: accepted -->

Status: **accepted** (2026-10-14, maintainer rulings Q1–Q23 of the peer-path design grill, rounds 1–4). Amends [ADR-0053](0053-lazy-rope-backed-decode-view-partial-path-routing.md) §5 (how a transport hands frames upward), [ADR-0071](0071-host-transport-is-a-separate-tu-with-shared-nothing-epoll.md) (the unit that waits on sockets) and [ADR-0044](0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md) (the per-peer stand-in and the insert-only CAN peer table). The copy-or-share half is a spec-process change and is recorded as [RFC-0022 Amendment 3](../spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md#amendment-3--2026-10-14-copy-or-share-is-a-backend-trait-plus-a-per-vertex-hint). Keeps the standing design rules of [`CLAUDE.md`](../../CLAUDE.md) §Design rules: no timers or clock reads, complexity lowered by deletion, compile-time by default, receiver pays. No wire surface moves.

## Context

A peer-cost study (2026-10-12/14) counted what one more peer and one more frame cost on the shipped tree, at `origin/main` `7817870a`. The host was loaded during the study, so **every figure is a count** (bytes, blocks, calls, syscalls, switches, `sizeof`), not a timing. Figures marked [est] are read from code, not measured. WIDE is the x86-64 host; MID is the ESP32-C6 (IDF v6.0, no board in the loop, so every MID figure is [est] or a `sizeof`). NARROW links no transports in its sentinel build and was not measured.

**Copies per frame (WIDE, measured).** User-space byte copies per inbound frame, counted by interposing `memcpy`/`memmove`:

| Ingress path | 64 B | 1 KiB | 4 KiB | 16 KiB |
| --- | ---: | ---: | ---: | ---: |
| TCP server, relayed out a server link | 2.6× | 2.04× | 1.01× | 1.00× |
| WS server, relayed out a server link | **4.2×** | **3.2×** | **2.8×** | **2.2×** |
| WS server, relayed out a WS client link | **7.1×** | **5.2×** | **4.8×** | **4.2×** |

The WebSocket excess has three avoidable sites: the stream-buffer append, an `erase_front` tail move of about 3.2–3.4 KiB per frame at 4 KiB and 16 KiB, and an assembler copy that runs even for an unfragmented frame, because the router installs a rope receiver and so skips the borrowed fast path. A WS client egress adds a base-class flatten and a separate masking copy. Below the 4 KiB share threshold the LKV store copies every inbound value a second time; the C6 build copies always. Fan-out TX on host server links is already zero-copy (`writev` from the LKV segment).

**Per peer.**

| Item | WIDE x86 | MID C6 | Source |
| --- | ---: | ---: | --- |
| heap per extra peer, tcp / ws / CAN server link (K = 8) | 273 / 385 / 246 B | — | measured, heap-rooted build |
| ws stream buffer held by an idle peer after one 16 KiB frame | 16 815 B | — | measured |
| `peer_endpoint_t`, a whole `transport_t` per tcp/ws session and per CAN peer | 120 B | 44 B | `sizeof` |
| `httpd_ws_link_t` session, of which strings (`subject`, `endpoint_str`, `name`) | — | 288 B, 203 B (70 %) | `sizeof` |
| esp_http_server `sock_db`, drawn eagerly × (max_peers + 3) | — | 200 B each, 1 400 B at 4 peers | `sizeof` |
| receive thread or task stack per client link; per self-heal connection | 8 MiB virtual, a few pages resident | 12 288 B; 24 576 B | [est], Kconfig defaults |
| UDP receive segment per datagram, default backend | 64 KiB | refused by the 32 KiB arena unless `max_frame` is set | measured (WIDE) |
| CAN peer entries | never erased; one per node id ever heard | | code |

**Scheduling (WIDE measured, MID [est]).** The host runs every frame to completion on its receive thread: 0 futex and 0 condvar calls per frame, one voluntary switch per paced frame. But each receive thread wakes every 100 ms only to re-check a stop flag, which is a timer in effect: 351 idle wakeups/s at 32 TCP dialers and 748/s at 32 UDP peers, plus 744 idle mutex locks/s on UDP. On the C6, with IDF's default `LWIP_TCPIP_CORE_LOCKING=n`, every lwIP socket call is a round trip to the tcpip task: about **9 task switches per inbound WS frame** and **7–8 per queued outbound frame per peer** [est]. A broadcast to N peers pays the queued-send round trip N times.

The cost is spread across five places that each solve the same problem their own way: four stream framers and a datagram path on ingress, one queue shape per link on egress, one thread or task per link for waiting, one `transport_t` per peer for addressing, and a CAN table with a clock in it.

## Decision

### 1. One frame-ingress module for every link (Q1, Q2, Q4, Q12)

**Every stream link (TCP and WS, client and server, host and C6) and UDP go through one frame-ingress module.** It reads the frame header first. A frame whose declared length exceeds the link's `max_frame` **closes the link before anything is drawn**. Otherwise the payload buffer is drawn **from the link's own source at the exact length** and the body is received straight into it (Q2). That is the shape the TCP dialer's `read_exact` already has; it now covers the server side and WebSocket too. A WebSocket payload is unmasked in place.

**The assembler stays for fragmented messages only** (Q4). An unfragmented frame is handed up as one view over its exact segment, whatever receiver tier the router installed. Fragments chain as views into a rope (ADR-0053 §5), with no copy.

**UDP gets a datagram adapter** of the same module (Q12). It receives into a `max_frame` buffer and then shares or copies the value under the copy-or-share trait of RFC-0022 Amendment 3. Only when `max_frame` exceeds 16 KiB does it fall back to peeking the length and drawing an exact segment. Batched receive (`recvmmsg`) is a later option, not part of this decision.

**On MID, a value that is stored is copied out of the lwIP `pbuf`** (Q14), because a `pbuf` is a Wi-Fi receive buffer the driver needs back. Only transient dispatch, a delivery that ends before the callback returns, borrows the `pbuf`.

Native-window hold ([ADR-0081](0081-pre-sink-ingress-native-window-hold-or-named-drop-never-parked.md) §2) is unchanged: frame ingress expresses "not yet" by not reading, so the bytes wait in the kernel or lwIP window.

### 2. Egress is one shared refcounted frame, queued per peer by reference (Q5, Q6)

**A send builds its frame once, in one refcounted block, and every peer it reaches holds a reference.** [PR #2130](https://github.com/avatarsd-llc/libtracer/pull/2130) (one shared refcounted WS TX frame per send) is the C6 instance. What a peer holds while its socket is busy follows the vertex's retention (Q5):

- a last-value vertex (retention `LAST`, the ruling's SAMPLE class) gets a **latest-wins slot**: one reference, and a newer frame replaces an unsent one;
- a STREAM vertex gets a **ring of references whose depth is the vertex depth**, so the per-peer queue is bounded by what the owner already declared.

**The producer writes, elected by try-lock** (Q6). A sender that wins `try_lock` on the socket writes directly. A sender that loses enqueues its reference and returns. The holder re-checks the queue after unlocking (the store-buffering pattern: a `seq_cst` fence on both sides, so at least one side sees the other and nothing is stranded). This deletes the `tx_handoff_t` admit/next lock pair. It keeps the rule that a producer never queues behind a stalled writer.

**A socket owned by a task gets one doorbell per burst** (Q6). Enqueueing to an empty queue arms the doorbell and posts one wake to the owning task; later enqueues in the same burst post nothing. On the C6 that replaces one control-socket round trip per frame per peer with one per burst. [ADR-0085](0085-ingress-drain-budget-waits-for-the-idle-task-not-a-clock.md) §2 is kept: a parked drain still sends the posted items from inside its wait.

**Which thread frees.** A frame reference may be dropped on the owning task rather than on the producer. Its block therefore comes from a source that is safe to free on that task, and it is never freed under a link lock. That is the lesson recorded in ADR-0072 (superseded; "the free's location is an API contract") and the per-target reclamation seam of [ADR-0080](0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md).

### 3. One wait module per node (Q7, Q14)

**Each node has one wait module, and it owns every socket wait on that node**: server sessions, dialers and UDP links alike. One per node is the rule. Running one instance per thread above about 4 000 peers (ADR-0071's deferred `SO_REUSEPORT` scale-out) would be an exception, and that exception has not been ruled. Delivery runs to completion on whatever drives the wait module. Links stop owning threads. The wait module has one adapter per target:

| Target | Adapter | Who drives it |
| --- | --- | --- |
| WIDE host | `epoll` set (`io_uring` stays reachable, per ADR-0071) | the node's I/O thread |
| MID (ESP-IDF, lwIP) | **lwIP callbacks**: a callback only enqueues a reference and rings one doorbell to the app's task | the app's task |
| NARROW (lwIP `NO_SYS=1`) | the same callbacks, no OS | **the app's loop polls** the wait module |

**Graph code never runs in the tcpip task.** On MID and NARROW the lwIP callback does no decoding, routing or delivery. It hands the `pbuf` reference across and returns.

**The wait has no timeout.** It blocks until a socket is ready or the node shuts down, and shutdown wakes it. That deletes the 100 ms receive and poll timeouts on the host and the 200 ms poll in the ESP WS client, so idle wakeups go to 0. Deadlines remain the application's, through a seam that takes `now` from the app's clock.

### 4. The C6 WebSocket server is our own, on the wait module (Q17, Q20, Q23)

**libtracer ships its own WebSocket server for the C6, built on lwIP callbacks as a wait-module adapter**, replacing the `esp_http_server` base of `httpd_ws_link_t`. Its frames go through frame ingress (§1) and its sends through the shared frame and doorbell (§2). That removes the httpd task's 12 288 B stack, the eager `sock_db` table and the four lwIP receive calls `esp_http_server` makes per WS frame [est].

**A non-upgrade HTTP request is handed to the application as raw bytes through a seam** (Q20). One port serves both, and core contains no HTTP framework. The application answers its own pages.

ADR-0085's drain budget and its idle-task wait carry over to the new server's receive context; how they are spelled there is part of that server's implementation.

### 5. One peer record per peer, with a generation; the stand-in is deleted (Q8, Q9, Q15)

**`peer_endpoint_t` is deleted** (Q8). It is a whole `transport_t` (vtable, two receiver slots, egress source; 120 B on x86, 44 B on the C6) built for every tcp/ws session and every CAN peer only to act as a directed facade. A directed send goes through the owning link with the peer's record instead.

**Each peer gets one peer record** (Q9). The record holds the peer's strings as spans drawn from the link's source, and it is freed when the peer leaves. **The record is the address, with a generation counter** (Q15). A send carrying a stale generation, one whose peer has left and whose record has been reused, is refused.

This is link state, not graph state. ADR-0044 §Decision 1 is unchanged: no vertex for an announced peer, and a transport vertex's `:children[]` stays synthesized. The accepted-session vertices of ADR-0044's 2026-08-13 amendment are unchanged too.

### 6. CAN peers are evicted (Q10)

**A CAN peer record is evicted on link events** (link down, teardown) **and by an application sweep that takes `now`.** The application decides what silence means; the link reads no clock. That deletes the `steady_clock` reads in `can_transport_t::peer_entry_t::last_heard` and `can_transport_t::pending_slice_t::arrived`. **A compile-time cap bounds the table, and a full table refuses newcomers**; it never evicts a known peer to make room. **A half-received CAN message is expired the same way.** With the `arrived` stamp gone, the app sweep that takes `now` also drops pending slices and stale partial reassemblies, and link down and teardown drop them all. No other path frees them.

### 7. Acceptance gates and the counted-metrics ledger (Q18, Q19, Q21, Q22)

**Every PR under this ADR passes these gates** (Q19):

| Gate | Bar |
| --- | --- |
| user-space payload copies per WS frame | ≤ 1.0 (WIDE, copy census). The RFC 6455 client mask pass, where present, is that one copy. |
| latency | no regression beyond the standing A/A noise band |
| RAM per peer | does not rise; falls where the PR targets it |
| idle wakeups | 0 on every link the PR touches, once the wait module carries it |
| cyclomatic complexity | the per-file total does not rise (the CCN ratchet) |

**Tests** (Q18) go through one fake link adapter at the router's receive and send seams, plus one real-socket smoke test per link kind.

**Counted metrics become a ledger** (Q21): copies, syscalls and switches per frame, RAM per peer and idle wakeups are pinned as JSON with a CI ratchet and a history, and the performance page gets a generated ledger section. The historical narrative on the performance page is deleted (Q22); anything an ADR or the spec depends on is flagged first. `bench_conn_ram` reads 0 on the shipped slab default and is fixed first ([#2134](https://github.com/avatarsd-llc/libtracer/issues/2134), heap-root or counting-source arm for the per-connection RAM bench), because the RAM-per-peer gate depends on it.

### 8. The release split (Q11, Q16, Q23)

| Release | Contents |
| --- | --- |
| **v0.20.0** | the quick wins [#2131](https://github.com/avatarsd-llc/libtracer/issues/2131) (lwIP core locking in the recommended ESP config), [#2132](https://github.com/avatarsd-llc/libtracer/issues/2132) (UDP segment sized to `max_frame`), [#2133](https://github.com/avatarsd-llc/libtracer/issues/2133) (TCP dialer one recv per frame) and [#2134](https://github.com/avatarsd-llc/libtracer/issues/2134) (fix `bench_conn_ram`); frame ingress (§1); egress (§2); RFC-0022 Amendment 3; the ledger and the performance-page cleanup (§7) |
| **v0.21.0** | the wait module and its adapters (§3); the peer record and CAN eviction (§5, §6); **last**, our own C6 WS server (§4), blocked by the client callback adapter |

## Considered options

- **Keep one thread or task per link** (status quo). Rejected: on the C6 the stack is the largest per-peer term in every client topology (12 288 B against about 300 B of libtracer state per link [est]), and each thread carries a 100 ms or 200 ms wake that the no-timers rule removes.
- **A wait module per transport rather than per node.** Rejected: it keeps one waiter per dialer, which is the per-link thread above under another name.
- **Stay on `esp_http_server` for the C6 WS server** (the round-2 Q14 position). Superseded by Q17: its per-frame receive calls, eager session table and own task are the costs §4 removes, and keeping it would leave one link outside the wait module.
- **An HTTP framework in core, or a second port for non-WS HTTP.** Rejected (Q20): one raw-bytes seam serves every application page without core parsing HTTP.
- **Keep the per-peer `transport_t` and only shrink it.** Rejected (Q8): the facade carries no state the owning link lacks; deleting a class lowers complexity by deletion.
- **Free session slots on departure but keep the facade.** Not taken: the peer record subsumes it (Q9), and its generation keeps a reused record from answering for a departed peer.
- **Expire CAN peers by silence inside the link** (ADR-0044's `peer_ttl`). Rejected (Q10): it reads a clock, and silence is a judgement only the application can make.
- **`io_uring` and batched UDP now.** Deferred, not rejected: they fit behind the WIDE adapter later.
- **A seqlock receiver slot** (the study's candidate 5). Out of scope for this ADR.

## Consequences

- **One slow sink now delays every link on the node.** Today a dialer is isolated on its own thread. The wait module trades that isolation for 0 threads per peer; a sink that blocks is an application bug the wait module makes visible.
- **The C6's tcpip core mutex becomes shared** between the wait-module callbacks, the Wi-Fi input path and app tasks once #2131 (lwIP core locking) lands. It must be measured on silicon alongside ADR-0085's pending run.
- **Two adapters of one transport must stay behaviourally identical**, ADR-0071's standing cost. The fake link adapter (§7) runs every conformance assertion against each.
- **ADR-0053 §5 is realized for WebSocket**: no flatten at ingress, and the assembler copy goes. The only contiguous egress copy left on a WS client is the RFC 6455 mask pass.
- **Receiver pays, by construction**: the exact-length payload draw (§1), the UDP `max_frame` buffer and the per-peer reference queues (§2) all come from the receiving link's own source, and the queue depth is the owner's declared vertex depth.
- **The ruled design adds no library-internal buffer and no timer**, and it moves nothing on the wire.
