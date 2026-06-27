# Direct browser-to-robot binding (rmw_tracer + browser node) is a target use case; WebTransport is its low-latency browser-facing stream transport

Status: accepted (the use case as an architectural driver, and the WebTransport direction it
implies). **Implementation is roadmap, not now**: rmw_tracer is parked ([ADR-0023](0023-ros2-binding-via-rmw-tracer.md),
[ADR-0025](0025-rmw-tracer-end-to-end-zero-copy-rcl-over-rdma.md)) under the strawberry-first
priority, and WebTransport follows `transport_ws` (#54). This ADR records the case so the
decisions it constrains are not re-litigated. Refines [ADR-0029](0029-websocket-first-transport-quic-deferred-per-link.md).

## Context — the corner case

A **browser binds directly to a ROS 2 robot**, with microsecond-class latency on the parts
libtracer controls, and **no translation layer**.

With **rmw_tracer**, a ROS 2 topic *is* a libtracer vertex (`rmw_publish` → a vertex write,
`rmw_take` → a read/await, [ADR-0023](0023-ros2-binding-via-rmw-tracer.md)). The chain that
today is `ROS 2 → DDS (CDR) → rosbridge_suite → JSON → WebSocket → browser` — serializing at
every boundary and adding milliseconds — collapses to:

```
robot controller/sensor → libtracer vertex (rmw_tracer)        ── µs, zero-copy, in-process
  → robot's libtracer node bridges the vertex over the wire
    → browser libtracer node (TS core, #56) subscribes directly  ── one decode, no re-serialize
```

The browser is a **first-class node in the robot's graph** ([reference 13](../reference/13-network-formation.md)),
and its subscription to a robot topic is the **same unified binding** as a robot-internal
controller link ([ADR-0026](0026-consumer-initiated-subscription-client-write.md)) — only with a
transport in the path (a remote, async binding). There is **one wire format from the robot's
controller pin to the browser**: no DDS CDR, no rosbridge, no JSON re-encode.

**Honest latency / zero-copy budget** (the µs are real where claimed, and not where they cannot be):
- *Intra-robot (rmw_tracer):* **µs** — zero-copy direct binding, no DDS. The headline vs the DDS
  millisecond path ([ADR-0025](0025-rmw-tracer-end-to-end-zero-copy-rcl-over-rdma.md)).
- *Per-hop libtracer overhead:* **µs, not ms** — a TLV header + a refcount bump, not a
  serialize/deserialize round-trip. This is the tax libtracer removes.
- *Robot → browser over a network:* **bounded by physics** — LAN sub-ms / WAN ms + transport
  framing. Not µs end-to-end across a network; libtracer removes the *serialization* milliseconds,
  not the *propagation* ones.
- *Browser side:* **one decode** (TLV → JS object). Zero-copy holds robot-internal and to-the-wire;
  it **ends at the browser** (JS cannot cheaply alias wire bytes into typed objects).

## Decision

1. **Treat direct browser↔robot binding as a load-bearing *target* use case.** It is what
   validates, together: the unified subscription/binding ([ADR-0026](0026-consumer-initiated-subscription-client-write.md)),
   the browser as a real node ([reference 13](../reference/13-network-formation.md)), the
   one-wire-format / cross-language guarantee, and the honest zero-copy boundary above. New
   transport/wire/API decisions are checked against it.

2. **WebTransport (HTTP/3 / QUIC) is the browser-facing *low-latency stream* transport** — ranked
   **above plain QUIC** (it is the browser-reachable form of QUIC) and **above WebSocket for
   streams** (WebSocket's single TCP stream suffers head-of-line blocking; WebTransport offers
   unreliable datagrams + independent streams). **WebSocket remains the reliable *control* link**
   (`transport_ws`, #54). WebTransport drops in behind the same `transport_t` seam
   ([ADR-0027](0027-transport-and-connections-are-vertices.md)) with no change to the binding
   layer, exactly when a latency-sensitive stream (robot telemetry/video to the browser) demands
   it. This is the concrete trigger ADR-0029 left open ("WebTransport for RTSP-rate UI streaming").

3. **The cross-match (ADR-0028) is what makes the direct binding *safe*.** A browser binding into a
   robot's graph only works if both speak byte-identical wire. The TS core + differential
   cross-match against the C++ core ([ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md))
   turns "browser and robot agree on the protocol" from a hope into a CI-enforced property — it is
   a **precondition** of this use case, not an afterthought.

## Considered options

- **Keep the rosbridge/DDS path for the browser.** Rejected as the libtracer story: it re-imposes
  CDR + JSON serialization and milliseconds at every boundary — the exact tax this case removes.
- **Use WebSocket for the low-latency streams too.** Rejected for *streams*: TCP head-of-line
  blocking serializes unrelated samples; WebTransport's datagrams/independent streams fit telemetry.
  WebSocket is kept for the reliable control plane.
- **Claim µs end-to-end browser↔robot.** Rejected as dishonest: network propagation is physics;
  libtracer removes the serialization ms, not the propagation ms (budget above).

## Consequences

- WebTransport is now a **named roadmap transport with a ranked rationale**, not an open question;
  it is pulled forward by *this* case, behind `transport_ws` and the transport seam.
- The use case is recorded as a **driver**: unified bindings, browser-as-node, the TS core +
  cross-match (building now), and the swappable transport seam are exactly its prerequisites, and
  nothing in the current build forecloses it.
- rmw_tracer stays parked, but the path to this case is **preserved and validated**; the
  implementation is queued (see the linked issue) after the strawberry-first work and rmw_tracer.

## Relates

- [ADR-0023](0023-ros2-binding-via-rmw-tracer.md), [ADR-0025](0025-rmw-tracer-end-to-end-zero-copy-rcl-over-rdma.md) — rmw_tracer + its zero-copy ROS path.
- [ADR-0026](0026-consumer-initiated-subscription-client-write.md) — the unified consumer-initiated binding the browser uses.
- [ADR-0027](0027-transport-and-connections-are-vertices.md) — the transport seam WebTransport drops into.
- [ADR-0028](0028-native-cores-kept-consistent-by-conformance-vectors.md) — the cross-match that makes the direct binding safe.
- [ADR-0029](0029-websocket-first-transport-quic-deferred-per-link.md) — WS-first / QUIC-deferred; this refines it with WebTransport's rationale.
- [reference 13](../reference/13-network-formation.md) — the browser as a network node.
