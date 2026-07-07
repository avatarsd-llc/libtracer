# CAN transport: a dynamic identity↔path map held *inside* `transport_can`, a structured 29-bit ID, and advertise+id-match reassembly — no roles, self-healing

Status: accepted

[ADR-0022](0022-transport-framing-modes-elided-full-tlv-advertise.md) established **header-elided** framing (the transport's native identity *is* the path; the TLV header never hits the bus) and named a "static `identity↔path` map held by the transport adapter." Designing the CAN transport for real forced three concrete questions ADR-0022 left open: **where the map lives and whether it's static or dynamic; how a path maps to a CAN ID; and how a payload larger than one frame is reassembled.** The maintainer's steer was decisive: *"the CAN map is dynamic config inside `transport_can`; there is no orchestrator or router role; genuinely the system is pure-decentralized, self-healing."*

## Decision

**1. The map is dynamic config held *inside* `transport_can` — not static, not held by a privileged node.** Any CAN-running node carries its own `identity↔path` map; it is mutable at runtime and **self-establishes decentrally via in-band advertise frames** (an advertise frame carries the `id↔path` manifest; lean id-matched frames follow). A rejoining node re-announces its own mappings, so the binding is **self-healing** with no coordinator. The constrained CAN leaf stays dumb (a compile-time CAN-ID scheme); the map machinery lives in `transport_can` on whatever node runs it. There is **no gateway/orchestrator role** ([reference 13](../reference/13-network-formation.md)).

**2. The CAN ID is a structured 29-bit extended identifier:** `[protocol-version prefix | node | endpoint]`. The version prefix *is* discovery-layer versioning on CAN (a distinct ID prefix per protocol version, per [CONTEXT.md](../../CONTEXT.md) *Discovery-layer versioning*). Because **lower CAN ID = higher bus arbitration priority**, the path→ID assignment *also* sets real-time priority — a CAN-specific knob the map exposes. CAN-FD is supported; classic 11-bit IDs are a constrained fallback.

**3. Multi-frame payloads reassemble via libtracer's own address-shift slicing / advertise+id-match — not ISO-TP.** Each CAN frame is a slice; `(origin_peer_id, ts) + index` chains slices into a rope at the reassembly layer (`mem_can_reassembly` → `view_can_frames`). This is the *same* mechanism that "spans a 9-byte elided CAN sample → a GB advertised rope group" ([CONTEXT.md](../../CONTEXT.md) *Advertise + id-match*), so CAN stays uniform with the rest of the stack.

## Considered options

- **Static config map (TOML, frozen at deploy), or a map held by a gateway "adapter" role.** Rejected per the decentralization steer: the map is dynamic and lives in `transport_can` on any node; nothing privileged holds the network's wiring. (ADR-0022's "static map held by the adapter" wording is hereby refined to "dynamic map inside `transport_can`.")
- **ISO-TP (ISO 15765-2) for multi-frame transport.** Rejected: it bolts on a separate flow-control state machine. libtracer already has address-shift slicing + advertise+id-match for exactly this, and reusing it keeps one reassembly model across CAN, UDP, and QUIC.
- **Flat per-message CAN-ID table with no structure.** Rejected: a structured 29-bit ID carries the version prefix (discovery-layer versioning) and lets ID assignment double as bus-priority assignment.

## Consequences

- **Pure-decentralized, self-healing CAN.** No node is special; a dropped/rejoined node re-advertises its own mappings; the map is never a single point of truth.
- **One reassembly model** (address-shift / advertise+id-match) spans CAN, UDP scatter-gather, and QUIC — no CAN-specific transport protocol to maintain.
- **Priority is a first-class map property** (via the ID assignment), exposing CAN arbitration to the deployment without a side channel.
- Implementation + the `docs/reference/14-can-transport.md` write-up are tracked in **#55**; this ADR is the decision it builds against.
