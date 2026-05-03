# Reference 04 — Communication Flows

> **Status**: stub. Canonical content lives in [../plans/04-graph-and-endpoint-api.md](../plans/04-graph-and-endpoint-api.md) until promoted.

## What goes here when filled

Sequence diagrams (Mermaid or ASCII) for every protocol-level flow:

- **Read** — subscriber → vertex → return last-known-value (or block under reliability=reliable).
- **Write** — publisher → vertex → fanout to each subscriber slot → ownership transfer.
- **Await** — subscriber → vertex → block until next write or timeout → return.
- **Subscribe** — subscriber → `write(/path:subscribers[], sub_tlv)` → slot allocated → liveness heartbeat begins.
- **Unsubscribe** — `write(/path:subscribers[N], NULL)` → slot cleared → fanout shrinks.
- **Field-write QoS update** — `write(/path:settings.deadline_ns, 5000000)` → vertex schema updated → next-fanout uses new deadline.
- **Bridge republish** — incoming TLV on transport module → bridge vertex → write into local graph → fanout to local subscribers.
- **Address-shift fanout** — large payload split across `ep[0..N]` → each slice independently routed → subscriber reassembles.
- **Deadline expiry** — sample published at T, deadline T+D, subscriber not drained by T+D → drop + `STATUS` notification on `/path:status`.
- **Liveness loss** — heartbeat missed N consecutive periods → vertex marks subscriber dead → slot reclaimed → `STATUS` on `/path:status`.
- **Network partition** — bridge transport down → bridge vertex marks remote-side stale → on recovery, discovery module triggers re-bind.
