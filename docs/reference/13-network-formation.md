# Reference 13 — Network formation

```{admonition} In one paragraph
:class: tip
A libtracer network is formed by **ordinary vertex writes**. A third party — typically
a web UI — joins as an **ephemeral peer with delegated admin**, then on other devices
it (1) **creates** controllers and **transport connections** through one in-band
mechanism ([ADR-0017 — Vertex creation is an in-band, ACL-gated field-write](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0017-in-band-vertex-creation-controller-orchestration.md),
[ADR-0027 — A transport, and each connection within it, is a first-class `/` vertex](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0027-transport-and-connections-are-vertices.md)),
and (2) **binds** data flows by issuing **consumer-initiated subscribe-writes** into
producers' `:subscribers[]` ([ADR-0026 — Subscription is consumer-initiated](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0026-consumer-initiated-subscription-client-write.md)).
It then **disconnects, leaving the wired devices talking to each other.** A node is
one path tree — data endpoints, controllers, and transports — all addressed, created,
ACL'd, `await`'d, and reconciled uniformly. **There are no privileged roles**:
"orchestrator" and "router" below are *transient situations any peer can be in*. The
network is **pure-decentralized and self-healing** — it depends on no central
authority, and bindings re-establish themselves on reconnect.
```

Formation uses no orchestration-specific wire behaviour. It composes the three
primitives — `read`, `write`, `await` — with the ACL and subscriber-edge surfaces
specified elsewhere in this suite: [04 — Communication flows](04-communication-flows.md)
describes the *data* plane, and this section describes the *formation* plane built
from the same operations.

## Transient hats, not fixed roles

None of the following is a fixed role or a privileged node — they are **hats any peer
wears transiently**. The same peer is a producer on one edge and a consumer on
another; "orchestrating" means *a peer holding admin and issuing formation writes*;
"forwarding" means *a peer that has ≥2 transports*. The network has **no central
authority**.

| Hat | What it means (transient) |
| --- | --- |
| **Owner** | A peer holding the provisioned root token that bootstraps a device's ACL and **delegates admin** ([CONTEXT.md](../../CONTEXT.md) *ACL / subject-token*). |
| **Orchestrating** | A peer the owner granted `WRITE_ACL` (admin) that is issuing formation writes. Usually a web UI, joining temporarily. Not architecturally special — a peer doing vertex writes, then leaving. |
| **Producing** | A vertex that holds an edge and fans out (e.g. `/A/sensor`). |
| **Consuming** | A vertex that receives delivery (e.g. `/B/in`). **Control-passive, data-rich.** |

A peer that is *orchestrating* is an **edge that exists temporarily → modifies
bindings → departs**, leaving producer and consumer wired. Because formation is just
vertex writes, the cables it patches outlive the hand that plugged them — and because
nothing privileged holds the graph together, a rebooted or reconnected peer re-forms
its own bindings (§*Self-healing without a coordinator*) with no coordinator present.

## The formation flow

```{mermaid}
sequenceDiagram
    participant O as Orchestrator (web UI, temp admin)
    participant B as Consumer device B
    participant A as Producer device A
    Note over O,A: 0. discover peers (mDNS / static)
    Note over O,B: 1. owner delegates admin → O
    O->>B: 2. write /B/net/quic-client/conn SPEC{name=a, config{addr=A_addr}}
    B->>A: QUIC dial (consumer dials)
    O->>A: 3. write /A/sensor:subscribers[] += SUBSCRIBER{target=/B/in}
    Note over A: A:acl authorizes the subscriber (fan-out gate)
    A-->>B: 4. fan-out: delivery = ordinary write to /B/in
    Note over B: B:acl on /B/in authorizes the writer (fan-in gate)
    O--xO: 5. orchestrator disconnects — A↔B persist
```

### Discovery

Peers are found by a discovery module emitting `(peer_id, transport_label,
transport_address)` tuples — `discovery_static` (pre-configured) or `discovery_mdns`
(dynamic announce); see [10 — Module catalog](10-module-catalog.md). Version
compatibility is settled here, not per-frame: a distinct service name, port or CAN-ID
prefix per protocol version ([ADR-0013 — Protocol-v1 scope boundaries](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0013-v1-scope-boundaries.md)).
See also [07 — Host embedding](07-host-embedding.md).

### Admin delegation

A device persists **identity only** — a stable `peer_id`, and a PKI key as a stronger
subject-token where one is provisioned. It does **not** persist graph wiring. The
owner peer grants the orchestrator `WRITE_ACL` on the subtree it may manage (NFSv4-style
ACE with `INHERIT`; [ADR-0020 — Access control uses NFSv4-style ACEs with inheritance](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0020-acl-nfsv4-style-aces-with-inheritance.md)).
The orchestrator holds delegated admin for the duration of its session.

### Creation — controllers and transport connections through one mechanism

Creation is an in-band write of a `SPEC` to a device-designated **creator-endpoint
vertex** ([ADR-0059 — Creation and removal are writes to a creator endpoint vertex](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md),
superseding the `:children[]` creation-field spelling of ADR-0017). The same mechanism
brings up a transport link, because a transport — and each connection within it — is
itself a vertex (ADR-0027). For a controller the `SPEC` names a **device-catalog
type**; for a transport connection the endpoint's location supplies the type, as below.

A controller exposes its own **input-port and output-port** vertices and
**subscribes to nothing at creation** — the patch-cable model: creation exposes
ports, binding is separate.

#### The creator endpoint for transport connections

[RFC-0014 — Creator endpoint: connection lifecycle and link liveness](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
(accepted) specifies one creator endpoint **per (transport, role) module** rather than
one global catalog. A creatable pair is a self-contained module mounted flat under the
net root — conventionally `/net`, which is a *recommendation* (the constructor default,
overridable per node), never a library rule — with names like `ws-client`, `ws-server`,
`quic-client`, `can`, …, each **declared by the application** through `register_module`
(modules are declared-only, ADR-0073 §4; the spellings here are the built-ins'
*suggested* names). Each module exposes a creator-endpoint child named `conn`:

```
/net/<module>/conn                          ; the creator endpoint (a / vertex, not a : field)
   write SPEC{ name, config }   → create /net/<module>/<name>, atomically
   write NAME{ <name> }         → retire /net/<module>/<name>
   read  :schema                → the module's config catalog
/net:children[]                             ; enumerate the modules
/net/<module>:children[]                    ; enumerate that module's connection vertices
```

- **Create and remove collapse onto one control**, distinguished by the TLV type of the
  written value: `SPEC` creates, `NAME` removes. Any other payload is
  `ERROR{tr::schema::type_mismatch}`; the endpoint never falls through to an ordinary
  assign. A `NAME` naming an unresolvable connection is a **no-op success**; `NAME{conn}`
  is rejected, so the endpoint cannot self-destruct.
- **The role is positional because it *is* the module.** `ws-client` is DIAL,
  `ws-server` is LISTEN, `can` is a multi-peer bus. `SPEC` therefore carries
  `{ name, config }` with no `type` and no `role` field, and each module's `:schema` is
  its own catalog — a dial target and a bind port are different catalogs, which is the
  reason to split them.
- **The path carries a name, never an address.** The created connection is addressed
  `/net/<module>/<name>`; `addr`, `port`, `keepalive`, `backoff` and `connect_timeout`
  are **creation-time config** — they travel in the `SPEC`'s `config` SETTINGS and are
  parsed into the transport-private `tr::net::conn_settings_t`. They do **not** live in
  the vertex `:settings` namespace: that core namespace was emptied outright by
  [RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
  §3.B, and `conn_settings_t` is explicitly *not part of any vertex's protocol
  `:settings` surface* — a device-private facet ([ADR-0021](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0021-colon-field-plane-is-the-vertex-ioctl.md) §Decision 3)
  reached through the transport's own config door. Nor is there a post-creation
  reconfiguration door: the only accessor, `transport_vertex_t::settings_of`, hands out
  a **const** view. So a peer's IP changing is *not* a `:settings` edit — today it means
  retiring the connection (`NAME`) and re-creating it (`SPEC`), which does tear down the
  routes under it, because `remove_connection` un-routes and retires the identity vertex.
  Routing that makes `/net/<module>/<name>` addressable is [ADR-0061 — Per-module mount routing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md).
- **Of those five keys, only `addr` and `port` are consumed today.** `keepalive` has no
  consumer anywhere in the tree — it is parsed and nothing reads it — and `backoff` /
  `connect_timeout` are parsed-but-dormant pending the §4 liveness engine
  ([#492](https://github.com/avatarsd-llc/libtracer/issues/492)). The per-key record,
  including which kinds honour `max_frame`, is the
  [connection-config module page](../modules/connection-config.md).
- **`SPEC` naming an existing name is `PATH_IN_USE`** — a re-`SPEC` is never a
  reconfiguration. A retrying orchestrator can treat that rejection as "already exists",
  so the create is idempotent-safe.
- **Creation is atomic.** One write yields a fully configured connection vertex; there
  is no live-but-unconfigured window.
- **Gating reuses the existing access-mask bits** (see [05 — Protocol TLVs](05-protocol-tlvs.md)):
  `SPEC` (create) gates on **`CREATE` (0x08)** on the endpoint's own ACL, so the create
  right is delegable without any right on the parent transport; `NAME` (remove) gates on
  **`WRITE` (0x02)**, not on `DELETE` — `DELETE` (0x10) is reserved-and-unused for
  protocol v1 ([RFC-0009 — Vertex removal and subscriber eviction](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §A.2).
  A peer can hold create-but-not-remove, or the reverse.
- **An absent creator endpoint answers `PATH_NOT_FOUND`** — the missing-`/`-vertex
  answer, and the sanctioned creatability probe, since `conn` is hidden from
  `/net/<module>:children[]`. `SCHEMA_NOT_FOUND` is reserved for the distinct
  "endpoint present, config type unknown" case.

```{admonition} Realisation status
:class: note
The per-module creator endpoint is **not implemented** in the reference implementation:
neither `/net/<module>/conn`, nor the reserved-and-hidden `conn` name, nor per-module
`:schema`-as-catalog, nor `NAME`-write removal. What is implemented is the addressing
half — a created connection mounts and routes at `/net/<module>/<name>`, with the module
name declared by the application (never library-derived — ADR-0073 §4) — reached through the `:children[]`
creation spelling that RFC-0014 supersedes. RFC-0014's byte-level clauses (the
`SPEC`/`NAME`/`config` layout, the catalog reply bytes, the liveness encoding, the gate
order, the error identities) are proposed pending code plus conformance vectors; its
declaring clauses stand on acceptance.
```

### Binding — consumer-initiated subscribe-writes

Data flow is established by the **consumer acting as a client** (ADR-0026): a `write`
into the **producer's** `:subscribers[]`, carrying the consumer as `target`. The edge
is **producer-held** — the producer fans out; the consumer holds nothing.

```
write /A/sensor:subscribers[]      += SUBSCRIBER{ target = /B/ctrl/0/in }
write /B/ctrl/0/out:subscribers[]  += SUBSCRIBER{ target = /B/actuator }
```

These two writes are a chain, not a loop. A delivery landing on `/B/ctrl/0/in` **stops
there** and never re-fans from that vertex ([RFC-0007 — SUBSCRIBER delivery terminates
at the target](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0007-delivery-terminates-at-target.md),
[ADR-0051 — Delivery terminates at the target](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0051-delivery-terminates-at-target-no-dispatch-limits.md);
see [04 — Communication flows](04-communication-flows.md)). The second hop happens
because the controller's own logic writes `/B/ctrl/0/out`, and that write fans out to
`/B/ctrl/0/out`'s own subscribers. Propagation past a target is the target's logic,
never the dispatcher's.

The orchestrator issues these writes on the consumer's behalf; a device's firmware or
NVS config issues the *identical* write on boot. Same operation, different driver —
there is no privileged "default binding".

### Delivery and the two ACLs

Delivery to a target is an **ordinary write**, indistinguishable from a direct one (the
target is subscription-unaware at runtime). Protection is the two endpoints' ordinary
ACLs, with no extra machinery:

| Direction | Guard | Question it answers |
| --- | --- | --- |
| **Fan-out / confidentiality** | producer's `:acl` | who may subscribe to me? |
| **Fan-in / sink protection** | consumer's `:acl` on the target (+ firmware arity) | who may write into me? |

So "multiple publishers will not feed a single sink" is enforced **device-locally**,
even with no orchestrator present — a single-input sink rejects a second writer via its
own ACL. Rejection lands at **delivery time on the consumer** (REST-server-auth shape),
not at bind time on the producer.

### Departure

The orchestrator disconnects. The created controllers and transport connections
remain in the devices — RAM, or NVS where the device persists them.

:::{note}
**A subscriber edge survives the orchestrator's departure when — and only when — its
target routes through a mount.** A `SUBSCRIBER` whose `PATH` names a path *through* a
transport mount, spelled in the **producer's** frame
(`/net/<module>/<link>/<consumer-path>`), binds the edge to that mount's link and the
residual below it (`graph_t::subscribe_wire`, `core/src/graph.cpp:2375`), so
`fwd_router_t::link_down` → `graph_t::evict_link_edges` on the orchestrator's session no
longer matches it and the producer keeps delivering. That is RFC-0021 §4.B.1/§4.C, and
it is what makes the departure above real for a third-party wire.

A `SUBSCRIBER` with **no** `PATH`, or one whose `PATH` matches no mount, still binds to
the **arrival session** and delivers back along the accumulated `src` — the
consumer-subscribes-for-itself shape, unchanged. A target that names a mount it cannot
deliver through (the mount exactly, or a bus link's own NAME — RFC-0020) is refused with
`tr::path::invalid`, never silently degraded. See §Boundaries.
:::
**Two devices keep talking with no third party present**; the patch cable stays. A
rebooted leaf re-establishes its links and subscriptions by re-issuing the same
client-writes from firmware or NVS config.

## Link liveness

A connection vertex and the link it names are **two lifecycles**. The vertex is
explicit — created solely by a `SPEC` write or an owner-local registration, removed
solely by a `NAME` write or an owner-local retire. The link underneath is a state
machine managed automatically (RFC-0014).

- **There is no lazy *vertex* creation.** A plain data write to an absent or retired
  `/net/<module>/<name>` does not create or revive it, a stated exception to the
  write-creates rule of [RFC-0005 — Subtree subscriptions](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md) §D
  and to revive-by-data-write (RFC-0009 §E.1) — a config-less connection would violate
  the atomicity creation protects. Re-creation is `SPEC`-only. **The only lazy
  establishment is reconnection.** (Since §D amendment 1 the *remote* arm needs no exception
  at all — a peer's write to an unresolved address is `not_found` everywhere. The exception
  still binds the local host API, which does write-create.)
- **Refcount gates `DIAL` links.** A *binding* is anything that needs the peer
  reachable — a standing subscription or `await` routed through the link, plus a
  transient hold for the duration of a one-shot `read`/`write`/`FWD`. It is **per-hop
  and local**: a multi-hop route holds a ref on *this* node's link only; the next node
  independently refs its own. The steady-state target is **socket up while refcount >
  0**; when refcount reaches 0 the socket closes and the link goes dormant **while the
  vertex persists**.
- **A `LISTEN` link ignores refcount.** Its listen socket stays bound and accepting
  until the vertex is retired — reachability is its purpose. Bindings routed through its
  accepted peers do not gate it.
- **Any op auto-wakes a dormant `DIAL` link.** The op triggers a dial, waits for one
  connect attempt bounded by `connect_timeout`, then serves or returns `link-down`. A
  `write` may therefore stall on a dial; `await` on the connection vertex — which
  resolves specifically when liveness reaches `up` and returns `link-down` on terminal
  failure — is the explicit "bring it up and wait" verb for callers that will not
  tolerate a data op blocking.
- **Self-healing is the retry loop toward that target, and runs only while refcount >
  0.** Loss while in use re-dials with `backoff`. A one-shot op's transient hold is
  released **before** self-heal is evaluated, so a lone one-shot whose dial fails drops
  the refcount to 0 and the link re-dormants with no background retry.
- **A permanently-unreachable `DIAL` peer with a standing binding retries
  indefinitely.** There is no max-attempt constant, no give-up bound and no
  terminal-failure state; `backoff` and `connect_timeout` are `:settings`, never
  hardcoded. A consumer distinguishes "transiently retrying" from "unreachable" by
  applying its **own** display threshold to the propagated `reconnecting` state — the
  wire stays honestly `reconnecting`.
- **Ops on a down or reconnecting link fail fast** with `link-down`; they never block
  forever on a dead peer.

The connection vertex's **value is its liveness state** — readable, `await`-able and
**subscribable**, so a subscription to `/net/<module>/<name>` streams transitions
without polling. This distinguishes the propagating connection vertex from the
write-only, non-propagating creator endpoint.

| State | Value | Role | Meaning |
| --- | --- | --- | --- |
| `dormant` | `0` | DIAL | The vertex exists; no socket (refcount 0). |
| `dialing` | `1` | DIAL | A connect attempt is in flight (first-ever or resumed). |
| `reconnecting` | `2` | DIAL | Retrying toward `up` between backoff waits, whether or not previously up. |
| `up` | `3` | DIAL | Socket connected, bidirectional. |
| `listening` | `4` | LISTEN | Listen socket bound and accepting (0 or more peers). |
| `bind-failed` | `5` | LISTEN | The listen socket could not bind — e.g. port in use. |

`dormant` takes `0` so a resting link is the falsy default. **The byte encoding becomes
normative on the merge of RFC-0014's conformance vectors** — the RFC defers it, so these
values are the reference encoding until then (`link_state_t`,
`core/include/libtracer/transport_vertex.hpp:98-105`). A `LISTEN` vertex's liveness
reports **listen-socket reachability**, not per-accepted-peer connectivity; accepted-peer
count and identity are exposed through `:children[]` / `:settings`. Once up, a link is
bidirectional regardless of who dialed — `role` says only *who initiates*. The liveness
engine that drives these transitions automatically is not implemented; the value is set
by the caller.

## Connection direction and folding

The default that pairs with consumer-initiated subscription is **the consumer dials,
the producer pushes** (SSE / server-streaming shape) — it also lets a constrained leaf
dial *out* through NAT. Direction is fixed by the module the connection was created
under, so it is chosen per connection: a constrained producer with many consumers, or
NAT on both sides, is served by dialing out to **any peer that has ≥2 transports** (a
forwarding hop, not a "router" role).

**Any node with ≥2 transports forwards** — forwarding is a required capability the
moment a node has two wires ([07 — Host embedding](07-host-embedding.md)). There is no
privileged router node, so the network **folds arbitrarily** — elided-CAN leaf →
full-TLV QUIC backbone → another fold — with the forwarder stateless and uniform across
framing modes. The bounds to design within:

- **Depth is capped by the route, and the route by its bytes.** A `FWD` frame's `dst` names
  every hop and is consumed monotonically, so a delivery travels exactly as far as its explicit
  source route — segment count ≤ 255 ([03 — Addressing](03-addressing.md);
  [RFC-0023](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0023-path-segment-cap-repriced-32-to-255.md); `kMaxSegments`,
  `core/include/libtracer/path.hpp:35`). For realistically named mounts the **1024-byte PATH
  budget binds first**, not the segment count: a 3-segment mount run (ADR-0061) costs its NAME
  headers plus its bytes — 20 B/hop for `/net/can/c0`, 32 B/hop for
  `/net/ws-client/board-01` — so the diameter is ≈ **30–50 hops** at 3-segment mount runs, and
  ≈ 25 at a 5-segment one. (Arithmetic over the encoding rule at
  [05 — Protocol TLVs](05-protocol-tlvs.md) §`0x06`, not a routed measurement —
  RFC-0023 §4.3, §10.)
- **A bound route is capped by hosts, not by bytes.** The second address form
  (`PATH_REF`, [05 — Protocol TLVs](05-protocol-tlvs.md) §`0x14`; [RFC-0024](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §4.3)
  spells one fixed 8-byte element per **host** rather than a run of names per hop, so its
  diameter is a host count: **≤ 255 normatively**, and ≤ 69 *reachable* today, since every
  bound path is minted from a canonical one and inherits that form's 1024-byte budget (≤ 171
  under packed segments). The canonical ceiling above is therefore the binding one in
  practice, and the bound form's own cap sits above it deliberately — it is a property of the
  element, not of whichever body grammar `PATH` happens to use.
- **Loops cannot form.** Because `dst` is consumed by at least one segment per hop (a whole
  `net/<module>/<name>[/<peer>]` mount run, RFC-0014 S2a), a physical cycle
  is harmless per-op rather than rejected. There is no revisit check — loop-freedom is
  by construction, not by rejection — and no flooding, so no duplicate deliveries and no
  dedup state anywhere. Parallel links to one peer are distinct explicit addresses:
  deliberate redundancy a consumer subscribes to knowingly. A recursive **topology walk**
  is *not* protected by this. A walking orchestrator must carry its own **client-side,
  identity-keyed visited set**, keyed on the node identity served by the `:identity`
  facet ([RFC-0011 — Node identity facet](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0011-node-identity-facet.md)) —
  a node-scoped, pre-serialized record that every vertex of a node serves byte-identically,
  resolved above the `READ` gate so an unauthenticated peer can pin it.
- **No global ordering across folds.** Per-producer ordering only; cross-node coherence
  needs a coordinated trigger.

## Self-healing without a coordinator

Because nothing privileged holds the graph together, recovery is **local and
automatic**:

- **Subscriptions re-form themselves.** On reconnect a consumer re-issues its
  subscribe-write from firmware or NVS config (ADR-0026) — the binding repairs without
  anyone re-provisioning it.
- **Transport-native bindings re-learn in-band.** Elided or lean bindings — a CAN
  `id↔path` map held *inside* the transport — re-establish from **advertise frames**
  (advertise + id-match), so a rejoining node re-announces its own mappings; see
  [14 — CAN transport](14-can-transport.md).
- **The link layer re-dials itself.** Under §*Link liveness*, a `DIAL` link with a
  standing binding retries toward `up` with `backoff` and no give-up bound, so a lost
  socket repairs beneath an unchanged subscriber edge.
- **There is no central authority to lose.** Any peer can wear any hat; a departed
  orchestrating peer or a downed forwarding hop costs only the paths through it, and the
  rest of the mesh is unaffected.

## Boundaries of the formation model

- **The model is imperative, not declarative.** It specifies the writes that produce a
  wiring, not a desired-state manifest. A reconciler that diffs a manifest against live
  state (`read` of `:children[]` / `:subscribers[]`) and converges by issuing exactly
  these create-and-bind writes is tooling over this wire model; it adds no wire
  behaviour and is out of scope here.
- **The subject token is opaque and key management is out of scope.** The ACL model
  takes a subject token from a pluggable resolver and compares it byte-for-byte; how a
  peer obtains, rotates or revokes one is not specified by the formation model. The
  `:identity` facet publishes a node's public key for pinning; it is not a key-management
  protocol.
- **Origination of a *remote-owned subscription* is specified only for a mount-routed
  target.** A cross-device wire is a subscription, not a link. A departing orchestrator
  leaves board A subscribed to a producer on board B by writing a `SUBSCRIBER` whose
  `PATH` is spelled in **B's** frame — composable offline from the link name the
  orchestrator itself minted, no read-back and no route walk
  ([RFC-0021](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0021-wire-subscriber-target-frame-of-reference.md)
  §4.B.1, [#491](https://github.com/avatarsd-llc/libtracer/issues/491)). What is **not**
  specified is a wire `SUBSCRIBER` naming a purely **local** target on the producer
  (RFC-0021 §4.B.2, unruled): that spelling keeps the arrival-session binding. The
  bus-crossing variant is refused outright, not deferred — a bus link's own NAME is not a
  routable next hop (RFC-0020).
- **Teardown is soft or hard.** *Soft* — drop the last binding; the `DIAL` link goes
  dormant, the vertex persists, and it self-heals on next use. *Hard* — a `NAME` write
  retires the vertex; subscriptions routed through the link are cascade-evicted
  (RFC-0009 §D) rather than left dangling, and a far-side producer's edge back through
  the retired link fails fast with `link-down` and is reaped.

## Pitfalls

- **Reading `listening` as "a peer is attached".** A `LISTEN` vertex's liveness reports
  only that its listen socket is bound and accepting. An implementation that surfaces it
  as connectivity shows a server as healthy with zero peers attached and as unhealthy
  with many.
- **Re-`SPEC`ing to reconfigure.** A `SPEC` naming an existing connection is rejected
  `PATH_IN_USE`. An implementation that treats reconfiguration as "create again" never
  changes the address; the address lives in `:settings`.
- **Expecting a data write to revive a retired connection.** Connection vertices are an
  exception to write-creates; the write fails and the peer stays unreachable until a
  `SPEC` recreates it.
- **Walking a folded topology without a visited set.** FWD loop-freedom protects a
  *delivery*, because `dst` is consumed monotonically; it protects nothing about a
  recursive enumeration. A walker that keys its visited set on transport address rather
  than on the `:identity` record revisits the same node through a second link and does
  not terminate.
- **Treating a successful `:subscribers[]` append as proof the flow works.** The fan-out
  gate answers on the producer; the fan-in gate answers on the consumer at delivery
  time. A bind that the producer accepts can still be denied on every delivery, and the
  orchestrator that issued it is gone by then.
- **Assuming a one-shot op leaves a link up.** The transient hold is released before
  self-heal is evaluated, so a one-shot against an unreachable peer leaves the link
  dormant with no retry in progress. Only a standing binding keeps it healing.
