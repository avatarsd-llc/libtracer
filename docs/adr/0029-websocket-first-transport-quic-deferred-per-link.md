# WebSocket is the strawberry/board-to-board reliable transport now; QUIC is deferred as a per-link, gateway-and-up upgrade

The strawberry profile is **CAN + a reliable IP transport** ([reference 12](../reference/12-deployment-profiles.md)). The open question was which reliable transport to build first — and whether board-to-board (controller-to-controller) links should be the same one. [The M6 research note](../research/m6-transport-and-batching.md) left **TCP-vs-QUIC** open; the maintainer asked specifically about **WebSocket vs QUIC**, flagging QUIC's **connection migration** as the one real reason to prefer it (roaming). This ADR settles the choice without locking the project in, because a transport is a swappable seam ([ADR-0027](0027-transport-and-connections-are-vertices.md)).

## Decision

**Ship WebSocket as the reliable transport for strawberry now — for the web-UI link *and* board-to-board** — and **defer QUIC** as a per-link, gateway-and-up upgrade behind the same `transport_t` seam.

Rationale:

- **WebSocket is already the planned Rung-2 transport, ships fastest, and is comfortable on ESP32-class boards.** Board-to-board WS on a stable LAN is unremarkable (framed TCP), so the same transport serves leaf↔leaf and leaf↔web-UI.
- **The web UI is browser-bound.** A browser cannot open a raw TCP/UDP/QUIC socket — only **WebSocket** (mature) or **WebTransport** (= HTTP/3 = QUIC, newer). WS gives the web-UI orchestrator a path *for free* with no HTTP/3 server infra.
- **The roaming gap is narrow and softened.** WS (TCP) only breaks on **L3 roaming** (the IP actually changes — cellular handoff, multi-subnet mesh); **L2 roaming** (same subnet) survives. And consumer-initiated subscription ([ADR-0026](0026-consumer-initiated-subscription-client-write.md)) makes a reconnect **self-heal** — the consumer re-issues its subscribe-write on reconnect — so an L3 break is a brief blip, not a manual repair.
- **QUIC lives gateway-and-up, not on the 16 KB MCU.** When QUIC lands it is for device↔device / WAN links on capable nodes (and later browser **WebTransport** for RTSP-rate UI streaming); leaves stay on CAN/UART/WS. The lean library is **msquic** (C API, HTTP/3 + WebTransport, production-grade); revisit at implementation time.

QUIC is pulled forward **only if L3 roaming becomes the norm** for strawberry boards (cellular, vehicle-mounted, multi-subnet mesh).

## Considered options

- **QUIC first.** Rejected for now: heavier on ESP32, needs a QUIC library + HTTP/3 for the browser, and its headline advantage (connection migration) only matters under L3 roaming, which a fixed-install grow controller doesn't do. Real, but premature.
- **Different transports for web-UI vs board-to-board.** Rejected as the default: one WS transport covers both; the seam still allows a board to run *both* WS and QUIC later (WS to the UI, QUIC to a roaming peer).

## Consequences

- **Reversible by design.** When roaming or WAN needs it, `transport_quic` drops in per-link with zero change above the seam ([ADR-0027](0027-transport-and-connections-are-vertices.md)); a node may run WS and QUIC simultaneously.
- **No ADR for "QUIC the protocol"** is needed yet — only this selection. The library choice (msquic lean) is deferred to the implementation issue (#54 ships WS; QUIC is a later issue).
- Confirms reference 12's strawberry = CAN + WS, now with the board-to-board + roaming rationale recorded so it isn't re-litigated.
