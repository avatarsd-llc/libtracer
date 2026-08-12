# Pre-sink ingress is held in the transport's native flow-control window or dropped with a named counter — never parked inside the library

Status: **accepted** (maintainer-ratified 2026-08-12, grilling session for [#1114](https://github.com/avatarsd-llc/libtracer/issues/1114)). This is the once-for-all hold/drop ruling that issue's definition-of-done demanded — recorded here exactly once and cited by the siblings [#1101](https://github.com/avatarsd-llc/libtracer/issues/1101), [#1102](https://github.com/avatarsd-llc/libtracer/issues/1102) and [#1103](https://github.com/avatarsd-llc/libtracer/issues/1103). Composes with [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) (the receiver seam and its no-library-buffer commitment) and [ADR-0041](0041-terminus-arena-decode-span-contract.md) §5 (memory policy lives at the injection seams).

## Context

Every transport link is wired by `transport_vertex_t::make_connection`, which installs the receiver sink via `fwd_router_t::add_child` **after** the link object exists. Anything the link decodes in that span reaches an empty `receiver_slot_t` and is dropped **silently** — `deliver` returns without moving `dropped_rx()` or `malformed_rx()` when no sink is installed (`core/include/libtracer/receiver_slot.hpp`).

The DIAL half of this window was closed for the core `ws` client ([#1025](https://github.com/avatarsd-llc/libtracer/issues/1025)) and `tcp` ([#1045](https://github.com/avatarsd-llc/libtracer/issues/1045)) with a two-phase bring-up: the constructor connects, the owner installs the sinks, then `start_receiving()` arms the receive thread. That mechanism does not generalize, for three distinct reasons the sibling issues document:

1. **LISTEN links have no deferral point at all** ([#1114](https://github.com/avatarsd-llc/libtracer/issues/1114)). `start_receiving()` deliberately no-ops for a listener (`core/src/transport_tcp.cpp:135-139` — arming would spawn a second serve loop onto an already-accepted peer's fd), and the accept loop is spawned from the constructor, i.e. inside `make_connection`, before `add_child` runs. Reproduced 3/3 on `quic`: a peer that connects and pushes inside that window loses the frame with **no counter moving**, while a positive-control second frame after sink install is delivered.
2. **Some transports own no receive thread to withhold** ([#1101](https://github.com/avatarsd-llc/libtracer/issues/1101)). On `quic`/`webtransport`, msquic's callback thread drives every `QUIC_STREAM_EVENT_RECEIVE`; on `webtransport` specifically, the module-private H3/QPACK state machine **must keep consuming** handshake bytes on that same thread even while payload delivery is withheld — the session's first data frame can follow the extended-CONNECT response in the same callback batch.
3. **Some links dial from their receive thread** ([#1102](https://github.com/avatarsd-llc/libtracer/issues/1102)). The ESP-IDF-native WS client's constructor does not connect; its recv thread does the dialling, with reconnect and backoff, so there is no "the connection is up" instant in the constructor to defer around.

Three issues were converging on the same unmade choice — hold bytes in the transport's own flow-control window, park decoded frames under an injected bound, or drop with a named counter — and answering it three different ways per PR was the outcome to avoid.

## Decision

**1. No library-internal parking of pre-sink ingress, ever.** The "park decoded frames under an injected bound" arm is **banned**. It conflicts head-on with the standing no-library-internal-buffers commitment ([ADR-0041](0041-terminus-arena-decode-span-contract.md) §5, [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) §2, the Stage-2 net-plane model of [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)): a parked-frame queue is a library-owned buffer with a library-chosen bound and a library-invented drop class, exactly the memory-policy ownership those ADRs moved to the injection seams. No transport, no target, no config trait re-opens this arm.

**2. Where the transport has a native flow-control window, ingress waits there.** "Not delivered yet" is expressed by **not consuming**, so the bytes stay in a buffer that is not libtracer's and backpressure propagates by the transport's own rules:

- **`tcp` / `ws` (core client and server): don't read the socket.** Unread bytes sit in the kernel receive window; the peer's TCP stack throttles. This is what the [#1025](https://github.com/avatarsd-llc/libtracer/issues/1025)/[#1045](https://github.com/avatarsd-llc/libtracer/issues/1045) DIAL deferral already achieves by withholding the receive thread, now stated as the principle rather than the mechanism.
- **`quic` / `webtransport`: per-stream receive disable** (msquic's per-stream receive enable/disable), re-enabled by `start_receiving()`. Bytes wait in msquic's flow-control window and stream-level flow control backpressures the peer. On `webtransport` this gates **delivery only**: the H3/QPACK state machine keeps consuming control/handshake bytes throughout ([#1101](https://github.com/avatarsd-llc/libtracer/issues/1101) step 1 — split "the H3 machine consumes" from "the frame is delivered"; the receive disable is applied to the WT session's data stream, never the control plumbing).
- **ADR-0042 §2 permits this explicitly.** That section constrains buffers *libtracer* holds; bytes left in a kernel socket buffer or an msquic flow-control window are the transport substrate's, not ours. (§2 is unamended — both of ADR-0042's errata touch §3.)

**3. Where the link's window is its connection, defer the dial.** The ESP-IDF WS client ([#1102](https://github.com/avatarsd-llc/libtracer/issues/1102)) holds pre-sink ingress by **not dialling yet**: the recv thread's dial loop waits for `start_receiving()` before its first connect, so no peer can push before the sink exists. Reconnects need no further gating — every re-dial happens after the link was armed. `start_receiving()` stays idempotent and inert on a link that never comes up, because the creation path arms every link unconditionally.

**4. Where holding is impossible, drop with a named counter.** Datagram transports (`udp`) and the bus (`can`, [#1103](https://github.com/avatarsd-llc/libtracer/issues/1103)) have no per-peer window to close: a datagram not consumed is a datagram the substrate drops on its own schedule, and CAN's callback registration cannot be delayed without starving the link's liveness bookkeeping (`last_heard`, the reassembly sweep, the ADR-0044 hello-advertise answers — all must keep running). There, a frame decoded before the sink exists is **dropped, and a named counter ticks** — a new, distinctly named drop cause in the transport's existing counter surface (the `dropped_rx()` convention), never folded into an existing cause and never silent. Honest loss, visible loss.

**5. Delivery is gated per accepted peer — the accept loop is never withheld.** On the LISTEN side the deferral point is each accepted peer's delivery path, opened when the owner arms the link. Withholding the accept loop itself is **rejected**: it leaves the listen backlog to absorb connections, and on a bounded backlog the kernel starts refusing them — converting a silent frame drop into a visible connection refusal on every graph write that creates a listener. Both LISTEN sub-windows get the same answer: before any peer is accepted there is nothing to hold; a peer accepted before the sink exists waits in its own native window per point 2 (or drops per point 4).

Both sides of every transport are covered by exactly one of these arms:

| Transport | DIAL side | LISTEN side | Issue |
| --- | --- | --- | --- |
| `tcp` | thread deferral (shipped, #1045) | per-peer: don't read the accepted fd | #1114 |
| `ws` (core) | thread deferral (shipped, #1025) | per-peer: don't read the accepted fd | #1114 |
| `quic` | per-stream receive disable | per-peer, per-stream receive disable | #1114 |
| `webtransport` | per-stream receive disable, H3 keeps consuming | per-session data-stream receive disable | #1101 |
| `ws` (ESP-IDF client) | defer the dial (latch before first connect) | — (no listener) | #1102 |
| `udp` | drop + named counter | drop + named counter | — |
| `can` | — (no dial/listen split) | drop + named counter; liveness bookkeeping never gated | #1103 |

## Considered options

- **Park decoded frames under an injected bound.** Rejected (point 1). Even injected, the *queue* is library-owned state with a library-defined overflow policy; it also pins segments across an unbounded owner-side gap, the amplification ADR-0042 §3 only accepts under an explicit per-vertex opt-in. The native window gives the same "nothing is lost" outcome with the buffer owned by the layer that already prices it.
- **Withhold the accept loop until the sink is installed.** Rejected (point 5): it trades a silent frame drop for kernel connection refusals during every listener-creating graph write.
- **Drop silently everywhere** (status quo, spelled honestly). Rejected: the #1114 reproduction shows a real frame vanishing with zero counters moved; where drops must exist (point 4) they are named, and where they need not exist (points 2–3) they are removed.
- **One uniform mechanism for all transports.** Rejected: the evidence in #1101/#1102/#1103 is precisely that the transports' concurrency shapes differ (owned thread / foreign callback thread / dialling recv thread / bus callback). The uniform thing is the *ruling* — native window or named drop — not the code.

## Consequences

- The per-transport work lands via the sibling issues, each citing this ADR instead of re-deciding: [#1101](https://github.com/avatarsd-llc/libtracer/issues/1101) (webtransport receive-disable arm), [#1102](https://github.com/avatarsd-llc/libtracer/issues/1102) (defer-the-dial arm), [#1103](https://github.com/avatarsd-llc/libtracer/issues/1103) (drop+count arm), and [#1114](https://github.com/avatarsd-llc/libtracer/issues/1114) itself (the LISTEN-side per-peer gates, with the `map_lock_gate_t`-driven `make_connection` reproduction and the non-vacuous guards its definition-of-done specifies — revert the production hunk, show the guard reddens).
- `start_receiving()` acquires a precise meaning on every transport: **open this link's delivery gate** — arm the withheld thread, re-enable the stream's receive, release the dial latch, or (drop-arm transports) nothing beyond what is already armed. It remains idempotent and safe on a link that never connected.
- The transports in the drop arm gain a **new named drop cause** in their counter surface; a deployment that sees it moving is watching the sink-install window, not guessing.
- The empty-`receiver_slot_t` silent return stops being the load-bearing behavior anywhere: after the siblings land, no frame reaches `deliver` before a sink exists except on the drop-arm transports, where the counter ticks first.
- [ADR-0042](0042-refcounted-receiver-seam-view-delivery.md) §2 is confirmed, not amended: the no-library-buffer commitment now also governs the pre-sink window, and the kernel/msquic-held bytes this ADR relies on are explicitly outside its scope.
