# Network formation — how a third party wires a graph across nodes

```{admonition} In one paragraph
:class: tip
A libtracer network is formed by **ordinary vertex writes** — there is no
orchestration-specific protocol. A third party (typically a **web UI**) joins as
an **ephemeral peer with delegated admin**, then on other devices it (1) **creates**
controllers and **transport connections** the same in-band way ([ADR-0017](../adr/0017-in-band-vertex-creation-controller-orchestration.md),
[ADR-0027](../adr/0027-transport-and-connections-are-vertices.md)), and (2) **binds**
data flows by issuing **consumer-initiated subscribe-writes** into producers'
`:subscribers[]` ([ADR-0026](../adr/0026-consumer-initiated-subscription-client-write.md)).
It then **disconnects, leaving the wired devices talking to each other.** A node is
one path tree — data endpoints, controllers, and transports — all addressed,
created, ACL'd, `await`'d, and reconciled uniformly. **There are no privileged
roles**: "orchestrator" and "router" below are *transient situations any peer can be
in*, never fixed roles. The network is **pure-decentralized and self-healing** — it
depends on no central authority, and bindings re-establish themselves on reconnect.
```

This document consolidates the formation flow that the rest of the suite describes
only in pieces ([04-communication-flows](04-communication-flows.md) covers the *data*
plane; this is the *formation* plane). Nothing here is new wire behavior — it
composes already-specified mechanisms.

## The hats a peer can wear (not roles)

None of these is a fixed role or a privileged node — they are **hats any peer wears
transiently**. The same peer is a producer on one edge and a consumer on another;
"orchestrator" just means *a peer currently holding admin and issuing formation
writes*; "router" (below) just means *a peer that happens to have ≥2 transports*. The
network has **no central authority** and is **self-healing**.

| Hat | What it means (transient) |
| --- | --- |
| **Owner** | A peer holding the provisioned root token that bootstraps a device's ACL and **delegates admin** ([CONTEXT.md](../../CONTEXT.md) *ACL / subject-token*). |
| **Orchestrating** | A peer the owner granted `WRITE_ACL` (admin) that is *currently* issuing formation writes. Usually a **web UI**, joining **temporarily**. Not architecturally special — just a peer doing vertex writes, then leaving. |
| **Producing** | A vertex that holds an edge and fans out (e.g. `/A/sensor`). |
| **Consuming** | A vertex that receives delivery (e.g. `/B/in`). **Control-passive, data-rich.** |

A peer that is *orchestrating* is an **edge that exists temporarily → modifies
bindings → departs**, leaving producer and consumer wired. Because formation is just
vertex writes, the cables it patches outlive the hand that plugged them — and because
nothing privileged holds the graph together, a rebooted or reconnected peer re-forms
its own bindings (§self-healing) with no coordinator present.

## The five steps

```{mermaid}
sequenceDiagram
    participant O as Orchestrator (web UI, temp admin)
    participant B as Consumer device B
    participant A as Producer device A
    Note over O,A: 0. discover peers (mDNS / static)
    Note over O,B: 1. owner delegates admin → O
    O->>B: 2. write /B/net/quic:children[] += SPEC{client, peer=A, role=DIAL}
    B->>A: QUIC dial (consumer dials)
    O->>A: 3. write /A/sensor:subscribers[] += SUBSCRIBER{target=/B/in}
    Note over A: A:acl authorizes the subscriber (fan-out gate)
    A-->>B: 4. fan-out: delivery = ordinary write to /B/in
    Note over B: B:acl on /B/in authorizes the writer (fan-in gate)
    O--xO: 5. orchestrator disconnects — A↔B persist
```

### 0. Discover

Peers are found by a discovery module emitting `(peer_id, transport_label,
transport_address)` tuples — `discovery_static` (pre-configured) or
`discovery_mdns` (dynamic announce). Version compatibility is settled here, not
per-frame (a distinct service name / port / CAN-ID prefix per protocol version;
[ADR-0013](../adr/0013-v1-scope-boundaries.md)). See [07-host-embedding](07-host-embedding.md).

### 1. Delegate admin (bootstrap of trust)

A device persists **identity only** — a stable `peer_id` (and, later, a PKI key as a
stronger subject-token). It does **not** persist graph wiring. The owner peer grants
the orchestrator `WRITE_ACL` on the subtree it may manage (NFSv4-style ACE with
`INHERIT`; [ADR-0020](../adr/0020-acl-nfsv4-style-aces-with-inheritance.md)). The
orchestrator now holds delegated admin for the duration of its session.

### 2. Create — controllers *and* transport connections, one mechanism

Creation is an in-band `:children[]` write of a `SPEC` naming a **device-catalog
type** ([ADR-0017](../adr/0017-in-band-vertex-creation-controller-orchestration.md)).
The same mechanism brings up a transport link, because a transport — and each
connection — is itself a vertex ([ADR-0027](../adr/0027-transport-and-connections-are-vertices.md)):

```
write /B/ctrl:children[]    += SPEC{ type=pid,    path=/B/ctrl/0 }       # a controller
write /B/net/quic:children[] += SPEC{ type=client, peer=A, addr=A_addr, role=DIAL }  # a link
```

A controller exposes its own **input-port and output-port** vertices; it
**subscribes to nothing at creation** (the patch-cable model — creation exposes
ports, binding is separate).

### 3. Bind — consumer-initiated subscribe-writes

Data flow is established by the **consumer acting as a client**
([ADR-0026](../adr/0026-consumer-initiated-subscription-client-write.md)): a `write`
into the **producer's** `:subscribers[]`, carrying the consumer as `target`. The edge
is **producer-held** (the producer fans out); the consumer holds nothing.

```
write /A/sensor:subscribers[] += SUBSCRIBER{ target = /B/ctrl/0/in }
write /B/ctrl/0/out:subscribers[] += SUBSCRIBER{ target = /B/actuator }
```

The orchestrator issues these on the consumer's behalf; a device's **firmware or
NVS config** issues the *identical* write on boot. Same operation, different driver
— there is no privileged "default binding".

### 4. Run — delivery is a write; the two ACLs guard both directions

Delivery to a target is an **ordinary write**, indistinguishable from a direct one
(the target is subscription-unaware at runtime). Protection is the two endpoints'
ordinary ACLs, with **no extra machinery**:

| Direction | Guard | Question it answers |
| --- | --- | --- |
| **Fan-out / confidentiality** | producer's `:acl` | who may subscribe to me? |
| **Fan-in / sink protection** | consumer's `:acl` on the target (+ firmware arity) | who may write into me? |

So "multiple publishers will not feed a single sink" is enforced **device-locally**,
even with no orchestrator present — a single-input sink rejects a second writer via
its own ACL. Rejection lands at **delivery time on the consumer** (REST-server-auth
shape), not at bind time on the producer.

### 5. Depart — the wiring persists

The orchestrator disconnects. The created controllers, transport connections, and
subscriber edges remain in the devices (RAM, or NVS if the device persists them).
**Two devices keep talking with no third party present** — the patch cable stays.
A rebooted leaf re-establishes its links and subscriptions by re-issuing the same
client-writes from firmware/NVS config.

## Connection direction and folding

The default that pairs with consumer-initiated subscription is **the consumer dials,
the producer pushes** (SSE / server-streaming shape) — it also lets a constrained
leaf dial *out* through NAT. `role` is an explicit per-connection `:setting`, so it
**overrides**: a constrained producer with many consumers, or NAT on both sides,
flips to dialing out to **any peer that has ≥2 transports** (a forwarding hop — *not*
a "router" role).

**Any node with ≥2 transports forwards** (forwarding is a required capability the
moment a node has two wires; [07-host-embedding](07-host-embedding.md)) — there is no
privileged "router" node, so the network **folds arbitrarily** — elided-CAN leaf →
full-TLV QUIC backbone → another fold — with the forwarder stateless and uniform
across framing modes. The bounds to design within:

- **Depth is capped by the route** — a `FWD` frame's `dst` names every hop and is
  consumed monotonically, so a delivery can travel exactly as far as its explicit
  source route (segment count ≤ the PATH cap of 32).
- **Loops cannot form** — a `dst` that revisits a node is malformed
  (`ERROR=INVALID_PATH`); there is no flooding, so no duplicate deliveries and no
  dedup state anywhere. Parallel links to one peer are distinct explicit addresses —
  deliberate redundancy a consumer subscribes to knowingly.
- **No global ordering across folds** — per-producer ordering only; cross-node
  coherence needs a coordinated trigger.

## Self-healing (no coordinator)

Because nothing privileged holds the graph together, recovery is **local and
automatic**, with no coordinator present:

- **Subscriptions re-form themselves.** On reconnect a consumer re-issues its
  subscribe-write from firmware/NVS config ([ADR-0026](../adr/0026-consumer-initiated-subscription-client-write.md))
  — the binding repairs without anyone re-provisioning it.
- **Transport-native bindings re-learn in-band.** Elided/lean bindings (e.g. a CAN
  `id↔path` map held *inside* the transport) re-establish from **advertise frames**
  (advertise+id-match), so a rejoining node re-announces its own mappings.
- **No central authority to lose.** Any peer can wear any hat; a departed
  "orchestrating" peer or a downed forwarding hop costs only the paths through it,
  and the rest of the mesh is unaffected.

## What is *not* here yet

- **Declarative formation.** The above is the **imperative** substrate. A planned
  tooling-domain layer lets an orchestrator apply a **desired-state network manifest**
  (nodes, controller instances, bindings, ACLs) that a **continuous reconciler**
  diffs against live state (`read` of `:children[]` / `:subscribers[]`) and converges
  by issuing exactly these create+bind writes — re-provisioning a node when it
  rejoins. The reconciler is tooling over this wire model; it adds no wire behavior.
- **PKI / key management** for a stronger subject-token (deferred `security_*`
  module; the ACL model is unchanged when it lands).
