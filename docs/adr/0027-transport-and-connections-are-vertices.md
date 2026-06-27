# A transport — and each connection within it — is a first-class `/` vertex, created and configured through the same in-band API as any other vertex

[ADR-0021](0021-colon-field-plane-is-the-vertex-ioctl.md) established the `:` field plane as a vertex's `ioctl` and **rejected** turning a vertex's *control facets* (`/v/acl`, `/v/subscribers`) into `/` sub-vertices, because that dissolves one-identity atomicity. [ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md) made vertex creation an in-band, ACL-gated `:children[]` write. The open question for third-party network formation: **how does an orchestrator (typically a web UI) bring up a transport link — e.g. a QUIC connection from B to A?** A first sketch squeezed it into a `transport_quic:peers[]` field. This ADR rejects that and places the transport in the path tree.

## Decision

**A transport is a vertex, and each connection/listener inside it is its own vertex** — addressed with `/`, configured with `:`, created with `:children[]`, observed with `await`, and ACL-gated like everything else:

```
/mydevice/net/quic/                       ← transport vertex
            ├─ server/      :settings{ bind_addr, port, role=LISTEN }
            └─ peers/
                 └─ <peerA>/ :settings{ addr, port, role=DIAL, keepalive }
                            :acl{…}   (read):stats   await (link up / down)
```

This **does not contradict [ADR-0021](0021-colon-field-plane-is-the-vertex-ioctl.md)**, and the distinction is the whole point:

- **`:` facets** are control surfaces *of one data vertex's identity* (a sensor's `:acl`, `:subscribers`). Making *those* `/` children would dissolve the vertex — rejected by ADR-0021, still rejected.
- **`/` vertices** are *genuinely distinct subsystem identities with their own lifecycle, stats, and children*. A transport, and each live connection, **is** such a thing — it is created and destroyed, it has up/down state to `await`, it has per-connection stats and its own ACL. It is not a facet of a sensor; it is its own component. So it belongs in the path tree.

Rule of thumb: **distinct lifecycle/identity ⇒ `/` vertex; scalar config of a thing ⇒ `:settings` on that thing.** (So a connection's `addr`/`port`/`role` are its `:settings`, not `/params/<addr>` path nodes.)

Two consequences make the model close on itself:

1. **Opening a connection is the same in-band creation as instantiating a controller ([ADR-0017](0017-in-band-vertex-creation-controller-orchestration.md)).** The transport vertex supports `:children[]`; its catalog is `{client, listener}`. A web UI opens a QUIC link with the identical mechanism it uses to create a PID controller:
   ```
   write /B/net/quic:children[] += SPEC{ type=client, peer=A, addr=A_addr, role=DIAL }
   ```
   A node is therefore **one path tree** — data endpoints, controllers, *and* transports — all read/written, created, ACL'd, `await`'d, and reconciled uniformly. This is what lets a third party form the whole network with nothing but ordinary vertex writes (the "uniformly exploit the edge-node capability from the web UI" requirement).

2. **Default link direction: the consumer dials, the producer pushes** — the SSE / HTTP-server-streaming shape that pairs with consumer-initiated subscription ([ADR-0026](0026-consumer-initiated-subscription-client-write.md)). The consumer (client) opens the link by creating a `role=DIAL` connection on *its own* transport; the producer accepts and fans out back over it. This also resolves NAT for the common case — the constrained leaf dials *out*. `role` is an explicit per-connection setting, so the direction is **overridable**: a constrained-producer-with-many-consumers (or NAT-on-both-sides) flips to dialing *out* to a **router** (any node with ≥2 transports; [ADR-0014](0014-router-cycle-termination-hop-count.md), [reference 07](../reference/07-host-embedding.md)). Because any node can route, the network folds arbitrarily, bounded only by `MAX_HOPS`.

## Considered options

- **Transport config as a `:settings`/`:peers[]` field on some vertex.** Rejected: a connection has independent lifecycle, up/down state, stats, and ACL — it is an identity, not a facet. Cramming N connections into one field loses per-connection `await`/ACL and breaks the uniformity that makes orchestration generic.
- **Out-of-band transport config (a TOML file / private API).** Rejected: a third-party orchestrator could then not form links with the same read/write it uses for everything else; it would need a side channel and per-device adapters.
- **Connection parameters as `/`-path nodes (`/quic/peers/<addr>/params/<port>`).** Rejected: blurs the `:`-vs-`/` rule. The *connection* is the identity (`/…/peers/<peerid>`); its scalars are `:settings`.

## Consequences

- **The web UI is not special.** It is an ephemeral peer with delegated admin that writes transport-connection vertices and subscriber edges into other devices, then disconnects — leaving those devices linked. Orchestration needs no orchestration-specific protocol.
- **Transport links participate in the same ACL, `await`, and reconcile machinery** as data and controllers — one model, one mental model.
- **Dial direction is a deployment knob**, not a protocol rule; the consumer-dials default + router fallback covers leaf-out, fan-out-server, and folded-mesh topologies. The end-to-end flow is in [reference/13](../reference/13-network-formation.md).
