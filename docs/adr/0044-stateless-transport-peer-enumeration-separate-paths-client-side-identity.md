# Transport-peer enumeration is stateless and synthesized from live traffic; separate paths stay separate paths; matching device identities across paths is client-side logic, never core

Status: accepted (maintainer-ratified 2026-07-03, strawberry-fw migration design grilling). Extends [ADR-0040](0040-net-plane-is-explicit-source-routed-only.md) (explicit-source-routed only) to the discovery/enumeration question; builds on [ADR-0027](0027-transport-and-connections-are-vertices.md) (path-as-route) and [ADR-0030](0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md) (in-transport advertise map). Companion to [ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md) and [ADR-0046](0046-bulk-transfer-is-ordinary-auth-gated-writes.md) — the three ADRs of the strawberry-fw network-rulings set.

## Context

strawberry-fw is the first product adopting libtracer end-to-end: ESP32-C6 grow controllers, a Docker gateway, and an Angular SPA, all as libtracer nodes. Topology: browser ↔ WebSocket ↔ boards, boards ↔ CAN ↔ each other, and the single-IP-entry case — one board reachable over IP with its CAN peers *behind* it — must work.

That topology forces two questions [ADR-0040](0040-net-plane-is-explicit-source-routed-only.md) left implicit:

1. **How does a caller learn what peers sit behind a transport vertex?** A board's CAN connection at `/dev/can` has peers announcing themselves on the bus (the [ADR-0030](0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md) advertise mechanism). Does the transport vertex materialize a child vertex per announced peer — mutating the node's graph as peers come and go?
2. **What happens when the same physical device is reachable two ways?** With the browser dialing both boards directly, board B is `ws→boardB`; through the single IP entry it is also `ws→boardA/can→boardB`. Under path-as-route these are two different addresses. Is anything anywhere obliged to know they are "the same device"?

Both questions have a tempting answer — peer state on the transport vertex, identity matching in the protocol — and both tempting answers reintroduce exactly what ADR-0040 retired: per-peer mutable state on constrained devices and a protocol plane that reasons about multipath identity.

## Decision

**1. A transport vertex holds NO peer state and creates NO vertices for peers.** A read of a transport vertex's `:children[]` is **synthesized on the fly** from the live announce/heartbeat traffic of that transport kind (the CAN advertise map, WS link liveness); it is a snapshot of who is currently audible, not stored graph structure. An explicit-source-routed `FWD` through the transport vertex to an announced peer is forwarded transparently, exactly as before. **CAN peers never mutate any node's graph.** This keeps ADR-0040's invariant intact at the enumeration layer: every device stays O(its own links), never O(the network).

**2. Separate paths are separate paths — by design.** The same physical device reachable as `ws→boardB` and as `ws→boardA/can→boardB` is **two unrelated paths**, two addresses, two subscriptions, two local vertices — the [ADR-0040](0040-net-plane-is-explicit-source-routed-only.md) redundant-links argument applied to topology walking. **libtracer never matches device identities across paths**, at any layer, on any node.

**3. The deduplicated "real graph" is top-level client/app logic.** The picture in which `ws→boardB` and `ws→boardA/can→boardB` collapse into one device node lives in **no device's memory** — it is a projection the *application* computes from walking its entry points, matching whatever identity its deployment trusts (announced device ids, auth pubkeys per [ADR-0045](0045-in-graph-authentication-per-hop-ed25519-tofu-noise.md)). It MAY ship as a side utility — e.g. a TypeScript package alongside the client SDK ([ADR-0034](0034-typescript-client-sdk.md)) — but never in core and never on devices.

**4. Two discovery layers, cleanly split.** Static/mDNS-style discovery (`discovery_static` / `discovery_mdns` in the module catalog) is **only** IP-peer bootstrap — "what can I dial" — and remains deferred. Vertex/topology discovery is **walking `:children[]` in-graph**, which already works today and which point 1 extends to transport-peer enumeration. Neither layer leaks into the other: dialing produces connection vertices; walking reads them.

## Considered options

- **Auto-mount announced peers as real child vertices** (the transport vertex creates/destroys a vertex per CAN peer). Rejected: per-peer mutable state on an MCU-class node, graph churn driven by another node's liveness, and cross-node coupling — one board's reboot mutates every listener's tree. It is the ROUTER-flood state model in vertex clothing; ADR-0040 retired it deliberately.
- **A discovery module in core that builds the network map.** Rejected/deferred: the only discovery problem the *library* has is IP bootstrap ("what can I dial"), which `discovery_static`/`discovery_mdns` cover when needed; a network *map* is a per-application projection with per-application identity rules, and putting it in core would force one identity model on all deployments.
- **Protocol-level multipath/identity** (the wire layer recognizes that two routes reach one device and dedups/prefers/fails-over). Rejected: this is exactly the auto-multipath machinery [ADR-0040](0040-net-plane-is-explicit-source-routed-only.md) removed, with the same failure mode — it destroys the failover signal that explicit redundant routes exist to provide.

## Consequences

- A browser node holding N WebSocket links has **N entry points to walk**; single-IP-entry with CAN peers behind it is the same walk, one level deeper (`ws→boardA/:children` reveals `can→boardB` synthesized from bus traffic).
- **Dedup, path preference, and multipath policy are client policy** — the SPA (or its side utility) decides that two paths reach one device and which to use; devices never do.
- The transport vertex's `:children[]` read joins the `:` control plane as another synthesized facet — no new wire behavior, no stored state, consistent with the stateless net plane of [ADR-0040](0040-net-plane-is-explicit-source-routed-only.md).
- The deferred IP-bootstrap discovery track stays deferred with a crisper boundary: when it lands, it answers "what can I dial" and nothing else.
