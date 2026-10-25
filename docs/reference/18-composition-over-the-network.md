# Reference 18 — Composition over the network

```{admonition} In one paragraph
:class: tip
A multi-node libtracer deployment is **one graph**, not N programs plus glue. A vertex on
another node is reached by its **path from the caller's own root, walked through transport
vertices**: a transport vertex mounts the peer's graph under itself, so
`/net/<module>/<name>/<residual>` is an ordinary address and `read` / `write` / `await` over
it are the ordinary three verbs
([RFC-0004 — Remote operation addressing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md) §A,
[ADR-0027 — Transports and connections are vertices](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0027-transport-and-connections-are-vertices.md)).
**Nothing is replicated.** The vertex stays where its owner registered it; what crosses the
wire is an operation, never a copy of the graph. What composition *adds* is the second half of
this page: the address becomes **location-dependent**, a peer is **not the owner** (creation,
and the frame a `SUBSCRIBER` target is spelled in, are deliberately asymmetric), and the link
becomes a **participant** — partition, late join and retire are states a single-node graph
never had.
```

This page is the canonical home for the question *"what happens to the graph model when the
graph crosses a node boundary?"* It composes material specified elsewhere and does not
restate it: [03 — Addressing](03-addressing.md) owns the path grammar,
[04 — Communication flows](04-communication-flows.md) owns the per-operation sequences,
[07 — Host embedding](07-host-embedding.md) owns the per-host-DAG ↔ global-topology
distinction and loop safety, and [13 — Network formation](13-network-formation.md) owns the
create-and-bind flow that *builds* a cross-node graph. What is here is the composition
itself: what a remote vertex is, what survives the boundary unchanged, what cannot, and what
breaks.

## The three composition axes, and which one this page is about

[CONTEXT.md](../../CONTEXT.md) §Two compositions names two: the **memory rope** (a value is a
chain of views) and the **TLV tree** (a value is a nested record). The third is **graph
(address) composition** — the tree of addressable vertices by path and `:children[]`
membership. Only the third one crosses node boundaries, and it does so without a fourth
mechanism: a remote address is the same path grammar as a local one, with more segments in
front of it.

| Axis | Unit | Crosses a node boundary as |
| --- | --- | --- |
| Memory (rope) | view / segment | payload bytes, invariant end to end ([02 — Graph model](02-graph-model.md) §payload invariance) |
| TLV (structure) | child TLV | the same bytes; a forwarder never re-encodes ([12 — Deployment profiles](12-deployment-profiles.md) rung 2) |
| **Graph (address)** | **vertex / path** | **a longer path — the subject of this page** |

## A vertex on another node

### The mount is the whole mechanism

A transport vertex has a **dual nature** (RFC-0004 §A). Its `:`-facets — `:settings`, `:acl`,
`:children` — are the *link's own* control surface and resolve locally. Its `/`-subtree is
**the peer's graph, mounted**: any `/`-segment below it is not local, it is an address on the
peer, reached by forwarding the unresolved suffix.

```
On node A (a web UI):

  /net/ws-client/board-01/can/can0/ow/t-probe
  └──────────┬──────────┘└────────┬─────────┘
   A's mount run for the            the residual — an address
   link to board-01                 in BOARD-01's frame of reference
```

Node A resolves its own mount run, strips it whole, and forwards `/can/can0/ow/t-probe` to
board-01; board-01 resolves *its* mount run `/can/can0`, strips it, and forwards `/ow/t-probe`
over the CAN bus. Each hop strips **its whole mount run** — `net/<module>/<name>[/<peer>]` —
never one segment ([RFC-0014](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) S2a,
[ADR-0061 — Per-transport mount routing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)).
The segments already carry the identifiers, so **the path needs no separate naming layer** and
no destination field: the path-suffix *is* the address.

Send-side routing and receive-side mounting are **duals and MUST agree** — the suffix a caller
routes *through* a transport vertex equals the prefix that vertex mounts inbound data *under*
(RFC-0004 §A, consistency requirement). They are one operation, not two that happen to match.

Drawn as trees, the composition is a graft: nothing is copied across the seam, and each node's
own tree is unchanged by being mounted somewhere else.

```{mermaid}
flowchart LR
    subgraph A["node A — a web UI"]
        A0["/"] --> A1["net"]
        A1 --> A2["ws-client"]
        A2 --> A3["board-01<br/>(connection vertex)"]
        A0 --> A4["view/live"]
    end
    subgraph B["node B — board-01"]
        B0["/"] --> B1["can"]
        B1 --> B2["can0<br/>(connection vertex)"]
        B0 --> B3["state/mode"]
    end
    subgraph C["node C — a 1-Wire probe on the bus"]
        C0["/"] --> C1["ow/t-probe"]
    end
    A3 -. "mounts B's tree" .-> B0
    B2 -. "mounts C's tree" .-> C0
```

From A, the probe is `/net/ws-client/board-01/can/can0/ow/t-probe`. From B it is
`/can/can0/ow/t-probe`. From C it is `/ow/t-probe`. All three name the same vertex; none of
them is *the* name.

### What "the vertex is remote" does and does not mean

**It does not mean a copy, a proxy object, a stub, a lease, or a handle to something far
away.** The caller holds a *path*. Every piece of the vertex — its store, its role, its
`:schema`, its `:acl`, its `:subscribers[]` — stays with the owner and is evaluated there.
The only thing that travels is an `FWD` frame carrying one operation and its own route
(RFC-0004 §B).

Three consequences follow directly, and each is a thing implementers reach for and will not
find:

- **There is no global name.** The same physical vertex has different addresses from different
  vantage points, because the address encodes the route (RFC-0004 §A, *location-dependence*).
  A path is relative to the caller's root, like a URL or a mount path.
- **There is no end-to-end correlation-id and no per-hop request state.** A reply routes back
  along the `src` route the request accumulated on the way in; a hop may reboot mid-operation
  and the reply still routes. Matching a reply to a *specific* outstanding request is the
  **originator's** concern, never a field in `FWD`: the originator seeds a reply route it can
  recognise, or matches by `src` suffix (RFC-0004 §D and its erratum of 2026-10-23;
  [§Acknowledged one-shot operations and safe retry](#acknowledged-one-shot-operations-and-safe-retry)).
- **The node-scoped aliases are not global names either.** A **PAIR** (the 8-byte
  `(index, generation)` element a chain is built of) is meaningful only on the host that
  issued it — which is exactly why a chain is a *stack* of elements rather than one identifier
  ([RFC-0029](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0029-one-path-primitive.md);
  CONTEXT.md §Path element). RFC-0027's path label is retired (RFC-0029 §5.3).

The one identity that *is* node-scoped and byte-stable across every path to a node is the
`:identity` facet — a pre-serialized record every vertex of a node serves byte-identically,
resolved above the `READ` gate. It is the key a topology walk uses to recognise "the same node
reached two ways"; nothing in the protocol performs that match
([07 — Host embedding](07-host-embedding.md) §node identity,
[RFC-0011](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0011-node-identity-facet.md)).

## What stays identical across the boundary

The list is long, and that is the point of the design: the boundary is crossed by making the
remote case *the same case*.

| Property | Why it survives | Where it is fixed |
| --- | --- | --- |
| The verb set is `read` / `write` / `await` | `subscribe` is a field-write, not a verb; there is no `connect` | [ADR-0006](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0006-read-write-await-api-no-connect.md), RFC-0004 §D |
| The API call is byte-for-byte the caller's local call | only the path is longer; the forwarding layer is invisible to application code | [07](07-host-embedding.md) §the address space is global, the API is local |
| A delivery **is** a write | a remote delivery is `FWD{op=WRITE}` — the identical frame a one-shot command uses, and the target cannot tell them apart | RFC-0004 §D (load-bearing) |
| The subscriber edge is **producer-held**, target singular | the producer fans out; the consumer stores nothing | [ADR-0026](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0026-consumer-initiated-subscription-client-write.md) |
| Delivery **terminates at the target** | no re-fan from the target's own `:subscribers[]`, local or remote | [RFC-0007](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0007-delivery-terminates-at-target.md), [ADR-0051](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0051-delivery-terminates-at-target-no-dispatch-limits.md) |
| Every subscription is a **subtree** subscription | bubbling is a property of the producer's local dispatch, which is where it runs | [RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md) §A |
| Authorization is evaluated **at the target**, per operation | a `FWD` is gated twice — the forward right on each transport vertex it crosses, the operation's own right at the final hop; no new ACL machinery | RFC-0004 §F |
| A composed branch read answers a remote reader too | the terminus applies the *ordinary local operation*: `op_resolve_walk.hpp` calls `graph.read`, which takes the branch/leaf fork in `core/src/graph.cpp` | [RFC-0016](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0016-composed-branch-read.md) |
| Payload bytes are invariant from publication to every subscriber | a forwarder adds addressing, not bytes | [02](02-graph-model.md) §payload invariance, [12](12-deployment-profiles.md) rung 2 |
| A retired path answers `tr::path::not_found` | the same answer as never-existed, from any distance | [RFC-0009](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §C |
| Roles stay invisible; `:schema` is the authority | a remote reader introspects exactly as a local one does | [11 — Vertex roles](11-vertex-roles-and-aggregation.md) |

## What necessarily differs

Six differences, each of them a decision rather than an omission.

### 1. The address is a route, so it is location-dependent

This is a consequence, not a defect (RFC-0004 §A). It has two prices worth designing against.
**Provenance** a consumer needs must travel *in the delivered data*, never be inferred from the
route or from which subscription produced the delivery — the target is subscription-unaware
([RFC-0003](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0003-bridged-wildcard-delivery-path.md);
CONTEXT.md §SUBSCRIBER direction). And **depth costs bytes**: for realistically named mounts
the 1024-byte `PATH` budget binds well before the 255-segment cap, giving a diameter of roughly
30–50 hops at 3-segment mount runs
([13](13-network-formation.md) §connection direction and folding,
[RFC-0023](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0023-path-segment-cap-repriced-32-to-255.md)).

### 2. A peer is not the owner — creation is asymmetric

A **local** data write to a nonexistent vertex creates it, `mkdir -p` style, gated by the
`WRITE` bit on the nearest existing ancestor (the `CREATE` bit before RFC-0036). A **remote** fieldless `FWD{WRITE}` to an
unresolved `dst` does **not** create — it answers `tr::path::not_found`
(RFC-0005 §D amendment 1, [#1139](https://github.com/avatarsd-llc/libtracer/issues/1139)).
Creation authority is *local-or-governed-channel*: a peer creates through the **creator
endpoint**, where the creation is typed, catalogued and ACL-gated
([ADR-0059](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md);
the flow is [13](13-network-formation.md)).

The asymmetry is the ruling, not an accident: the two callers differ in exactly the property
that matters — one is the owner of the graph, the other is a peer.

### 3. The wire subscribe door is mount-routed only

A `SUBSCRIBER` carrying a `PATH` child has that path resolved **from the receiving producer's
own root**, by the same strip-K mount descent a `FWD` `dst` receives
([RFC-0021](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0021-wire-subscriber-target-frame-of-reference.md) §4.A).
That is what lets a third party wire two *other* nodes together and depart: because the edge
stores the **mount**, it is not keyed to the writer's session, so the writer's departure does
not evict it (§4.C). The end-to-end recipe is gated by
`core/tests/orchestrate_depart_test.cpp` — three real in-process nodes, a real socket
teardown, and the assertion that delivery continues after the orchestrator leaves.

What a peer may **not** spell over the wire is a purely *local* target on the producer.
RFC-0021 amendment 1 (2026-08-15) rejects that arm permanently: a `PATH` matching no mount
keeps the arrival-session meaning, and the in-process door
(`fwd_router_t::subscribe_toward`, `graph_t::subscribe`) keeps the full frame of reference. The
ground is blast radius — the wire door is peer-triggered, and permitting it would let a remote
peer provoke edge allocations on arbitrary local vertices of the producer's graph, an address
space the peer chooses rather than one the producer registered.

A target that names a mount it cannot deliver through — the mount exactly with no residual, or
a bus link's own connection NAME — is **refused** with `tr::path::invalid`, never silently
degraded
([RFC-0020](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0020-bus-name-not-a-routable-next-hop.md);
RFC-0021 §4.F). Directed delivery to a *bus peer* target is likewise rejected today, pending
[#741](https://github.com/avatarsd-llc/libtracer/issues/741): a peer has no directed registry
entry to store, and binding it would produce a subscription that silently broadcasts
(`core/include/libtracer/fwd_router.hpp`, `subscribe_toward`).

### 4. There is no cross-node atomicity, and no cross-node order

Ordering is **per-producer** and is carried by the producer's own monotonic
`origin_timestamp`; coherence across producers is a `(origin, ts)` sample group, which means a
*coordinated trigger*, not an inferred one
([ADR-0019](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0019-per-producer-monotonic-origin-timestamp.md);
[13](13-network-formation.md) §no global ordering across folds). A branch write is admitted
all-or-nothing and *applied* per leaf, so cross-leaf atomicity is not promised
(RFC-0005 §B); the composed branch read is the same non-promise on the read side — each leaf is
a consistent refcounted store, the composed reply is not a transaction (RFC-0016 §Summary).
Cluster consensus, CRDTs and vector-clock causality are explicitly **out of scope for v1**;
layer them above libtracer if a deployment needs them
([04](04-communication-flows.md) §network partition and recovery).

### 5. The link is in the call path

A local operation cannot fail because of a wire. A composed one can, in ways with no local
analogue: an op on a dormant `DIAL` link triggers a dial and waits for one bounded connect
attempt before serving or answering `link-down`; an op on a down or reconnecting link fails
fast; a `DIAL` socket is refcount-gated and closes when nothing is bound through it, while the
connection *vertex* persists ([13](13-network-formation.md) §link liveness). Whether a link is
up is itself a readable, `await`-able, subscribable value on the connection vertex — the graph
answers the question with the same three verbs it answers everything else with.

### 6. Alias forms exist only across links

Two alias forms exist, both of them *aliases for an address*, none of them a second address
space, and each degrading to the canonical string rather than failing:

| Form | Scope | Degrades to | Where |
| --- | --- | --- | --- |
| Bound path (`PATH_REF`) | one 8-byte ref per **host** on the route | dropped and re-minted from the canonical original — never repaired | RFC-0024 |
| ~~Path label~~ | retired (RFC-0029 §5.3, slice S3) | — | RFC-0027 |

The canonical string `PATH` remains the mint key and the fallback in both, which is what
keeps one spelling per address (CONTEXT.md §Path element).

The per-link route handle (`ADVERTISE` / `COMPACT` / `HANDLE_NACK`) is **retired**: delivery is
the `PAIR` chain, which carries the whole route in the frame and holds no state at any hop. A
transport may still keep a link-local alias private to its two ends (CAN's native-ID map); the
router never sees it.

## Where the deployment spectrum changes the answer

NARROW / MID / WIDE below name the ends of the spectrum
[12 — Deployment profiles](12-deployment-profiles.md) enumerates as rungs: **NARROW** is a
constrained single-transport leaf (rung 1, profile P1, header-elided framing), **MID** is a
forwarder holding ≥ 2 transports (rung 2, P2), **WIDE** is a full host (rungs 4–5, P3).

- **Address cost is settled for NARROW and open for MID/WIDE.** On a header-elided transport
  there is no room for a route at all — the CAN ID *is* the path, the `identity↔path` map is
  mandatory, and a link-local alias is therefore not a choice
  ([ADR-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0022-transport-framing-modes-elided-full-tlv-advertise.md),
  [14 — CAN transport](14-can-transport.md)). On full-TLV transports the default is
  **full-route and stateless** for every delivery — a constrained node forwarding 50 cold reads
  holds **zero** per-flow state, and so does one forwarding a running stream (chain delivery).
- **No hop holds per-flow state.** RFC-0027's path-label tables were the one per-hop table in
  the path plane, and RFC-0029 slice S3 deleted them: a PAIR is the owner's own index entry, so
  a hop validates it against the graph it already has (RFC-0029 §9.1).
- **Partition detection follows the transport kind, not the rung — and NARROW's usual kinds
  have none.** The link-departure hook fires only on connection-oriented transports, so a
  header-elided bus leaf gets no transport-level signal at all (§Partition, below).

## The failure modes composition introduces

### Partition

**What the wire does.** When a connection-oriented link's session dies, the transport fires the
link-departure hook `fwd_router_t::link_down`, and
`graph_t::evict_link_edges` deactivates and reclaims every subscriber edge whose stored link is
that link. That is why a producer stops addressing a dead session; the header records what it
releases — roughly 90 bytes of route/link/caller state per edge, against the ESP32-C6's measured
~27 KB per browser session that motivated it. The router holds no per-flow route state beyond
the edges, so there is nothing mid-chain to unwind. A delivery whose chain names an element the
reconnect invalidated fails that hop's generation check and is dropped there; it carries an empty
`src`, so no error comes back to the producer. The producer's edge recovers through its own link's
departure, which evicts it, or through a re-learn once a departure clears its chain
([RFC-0029](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0029-one-path-primitive.md) §7.2, §8).

**What it does not do.** Three gaps, all deliberate and all load-bearing when designing a
deployment:

- **Connectionless kinds and buses never fire it.** UDP has no closure event and CAN is an
  announce census; both honestly never call the hook (RFC-0009 §D.5). At **NARROW** — a leaf on
  a bus — partition is therefore not signalled by the transport at all, and a consumer's own
  `await` timeout or the advertise census is the only evidence.
- **Eviction is not parking, and re-binding is the application's job.** The `(link, route)`
  split of a mount-routed subscription is computed once at bind time; if the link later tears
  down the binding is evicted with it, and the helper explicitly "does not track topology
  churn" (`fwd_router.hpp`, `subscribe_toward`). Re-establishment is the reconnecting party's
  responsibility — a consumer re-issues its subscribe-write from firmware or NVS config
  ([13](13-network-formation.md) §self-healing without a coordinator).
- **A lost reply is silence.** Because there is no correlation-id and no per-hop request state
  (RFC-0004 §D), a one-shot whose reply is lost to a partition produces no answer rather than an
  error, and `core/include/libtracer/fwd_router.hpp` exposes no reply-deadline surface — ending
  the wait is the caller's concern. That holds for a remote `await` too, whether or not it reaches
  its terminus: the deadline is the requester's (RFC-0004 Amendment 3). The terminus parks a
  waiter and answers on the next change, and enforces no `await_timeout`.

**What recovery is.** A `DIAL` link with a standing binding re-dials with `backoff` and has no
give-up bound and no terminal-failure state; a consumer distinguishes "transiently retrying"
from "unreachable" by applying its **own** display threshold to the propagated `reconnecting`
state ([13](13-network-formation.md) §link liveness). There is **no automatic graph-state
merge**: last-write-wins by timestamp is the stated conflict policy, and a lower-timestamp
write made during the partition is silently discarded
([04](04-communication-flows.md) §network partition and recovery).

```{admonition} Realisation status — partition
:class: note
The link-departure eviction is implemented and wired onto every connection-oriented transport
(ws server / ws client / tcp, the ESP-IDF adopted-mode link, QUIC and WebTransport), per
RFC-0009 §D.5. The **liveness engine** driving `link_state_t` transitions automatically
(RFC-0014 §4 S5, `tr::net::self_heal_link_t`,
[#492](https://github.com/avatarsd-llc/libtracer/issues/492)) runs for kinds registered
`self_heal_dial`, consuming `backoff` and `connect_timeout` — which since
[#1548](https://github.com/avatarsd-llc/libtracer/issues/1548) is every built-in
point-to-point DIAL kind (`udp`, `tcp`, `ws`). Elsewhere — provided links, LISTEN links, bus
kinds — the value is still set by the caller. Of the connection-config keys,
`keepalive` is accepted and ignored
([13](13-network-formation.md), [connection-config module page](../modules/connection-config.md)).
So the recovery *semantics* above are specified and the *automation* of them is partly not yet
true.
```

### Late join

A node that joins an already-running graph — a browser opening a UI, a rebooted leaf, a peer
dialling in after an outage — has to acquire state it was not present for. The model gives it
four instruments, and withholds history.

- **Prime with one composed read, then subscribe for the tail.** A plain `READ` of a parent
  with ≥ 1 registered child serves the folded `POINT` tree of its registered subtree, each node
  carrying that vertex's stored TLV verbatim (RFC-0016). This replaces a recursive
  `:children[]` walk plus one read per leaf; RFC-0016 §Motivation records the reference
  deployment's 76-request prime and a measured **22.6 s → 1.15 s** time-to-all-endpoints against
  the predecessor firmware's single aggregate snapshot, with the request trickle rather than
  bandwidth as the dominant cost. That figure is evidence for the shape of the cost, not a
  number an implementation must hit.
- **Ask for the latch.** A subscription may set `durability_request` (bit 5 of the packed
  `delivery_policy` in the `SUBSCRIBER`'s `SETTINGS` child), which delivers the producer's
  latched last value at admission
  ([RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md) §3.A;
  `core/include/libtracer/subscriber.hpp`). It is the **only** one of the three policy fields
  consumed today — `priority` is stored and read back verbatim, awaiting the transport work that
  would honour it, and `reliability` is stored and read back verbatim awaiting **nothing**: the
  pressure arm is the receiving vertex's own declaration
  ([RFC-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0025-stream-class-values.md)
  §Erratum 2026-08-24).
- **New vertices announce themselves by existing.** A child's *appearance* is just its first
  write bubbling to the parent's subscribers, so a subtree subscriber learns about vertices
  created after it joined without polling `:children[]` (RFC-0005 §A; RFC-0030 §7.3).
- **Addressing re-establishes itself in-band.** A chain delivery holds no per-flow state, so
  there is nothing to re-advertise on (re)connect; a PAIR the joiner does not know draws a
  `NOT_FOUND`-class error and the sender falls back to the full string path and re-learns from
  the next reply, with no withdraw frame, no lease and no TTL (RFC-0029 §6.3); a CAN transport's
  `identity↔path` map re-learns from advertise frames
  ([14](14-can-transport.md), [ADR-0030](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0030-can-transport-dynamic-in-transport-map-advertise-reassembly.md)).

**What a late joiner does not get is history.** It sees the latch, if it asked for one, and
everything after — nothing before. A STREAM vertex's history ring is drain-only and is not
addressable by index; a consumer that wants a queue makes **its own** receiving vertex a STREAM
(CONTEXT.md §Element addressing). And *finding* a peer to join to is out of the graph's hands:
the discovery modules are catalogued and **none is realised**, so a late joiner today dials a
configured address ([07](07-host-embedding.md) §discovery,
[10 — Module catalog](10-module-catalog.md)).

### Retire

Retirement is where composition's asymmetries are sharpest, because a name disappearing on one
node is an event the other nodes are structurally unable to observe.

- **Removal is owner-initiated and local.** There is no wire remove-verb. The owner calls
  `retire()`, or a peer *writes a request the device executes* — a `NAME` write on a creator
  endpoint, which is an ordinary write to an ordinary vertex whose owner's logic chooses to
  honour it (RFC-0009 §A.1, ADR-0059). No operation removes a vertex; a device removes a vertex.
- **Retirement is not observable as a distinct state.** A retired path reads
  `tr::path::not_found` — byte-identically to never-written and never-existed. There is no
  delivered marker and no notification; a peer that cares learns by reading and finding nothing
  (RFC-0009 §C).
- **Retiring evicts the retired vertex's own edges** (§D.3) — and, for a *connection* vertex,
  cascades: subscriptions routed through the link are cascade-evicted rather than left dangling,
  and the order is fixed (retire, deliver, un-route, then close and destroy the transport), because
  a link must stop being *routable* before it stops being *alive* or in-flight frames arrive at
  a destroyed transport (RFC-0009 §E.2; [13](13-network-formation.md) §boundaries).
- **A retired *target* does not evict its producer's edge.** This is the one that only exists
  because of composition. An edge whose delivery target has been retired stays registered and
  MUST NOT be silently dropped, because the producer has no way to learn the target's fate:
  delivery is a write, and a write to a retired path is not an error the producer observes
  (RFC-0009 §D.4). The RFC names it a real dangling-edge cost rather than papering over it.
- **Revival is asymmetric, like creation.** A local data write to a retired path revives it —
  fresh and re-virginized: no ACEs, no handlers, no value, no subscribers of the retired owner
  (RFC-0009 §E.1, §B.6). A remote peer cannot do this: its fieldless write to an unresolved
  address is `not_found` (RFC-0005 §D amendment 1). A remote consumer therefore cannot resurrect
  what an owner retired; it can only ask the owner, through a surface the owner provides.
- **Stale aliases cannot deliver to a name's new occupant.** This is what the generation fields
  are for. A vertex ref's generation is compared against the vertex's retirement stamp on every
  use and **saturates rather than wraps**. A wrapped generation would let a stale reference
  validate falsely and deliver to a path's new occupant, which is the mis-route class the
  design closes by construction rather than by digest width (RFC-0024, RFC-0029;
  CONTEXT.md §Vertex ref).

```{admonition} Realisation status — retire
:class: note
RFC-0009's retirement semantics are accepted, and its subscriber-eviction half (§D) settles
behaviour that already shipped. The open question the RFC itself flags is §Discussion 1: a web
UI **cannot** retire a remote connection — it can only ask the device to, through a
device-provided surface — and whether the transport plane should expose such a surface is
unruled. The gate on the reference implementation's retirement behaviour is
`core/tests/retire_test.cpp`.
```

## Acknowledged one-shot operations and safe retry

An application often needs to know that one operation landed: creating or clearing a link, a
`:children` write, a settings commit, an OTA or upload chunk, a command verb. It also needs to
retry that operation when it hears nothing back, over any carrier, including a UDP link that
acknowledges nothing. Both come from what is already on the wire. **There is no operation id, no
correlation id and no duplicate-detection state at the receiver**
([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md) §D;
[ADR-0040](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0040-net-plane-is-explicit-source-routed-only.md)). Telling attempts apart
happens at the client
([ADR-0044](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)),
and the caller owns the retry ([ADR-0066](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0066-element-write-is-a-single-attempt-cas.md)).

### The acknowledgement is the reply to a `WRITE` with a non-empty `src`

A `WRITE` whose `src` is non-empty is answered by at most one `REPLY`: `kind=RESULT` with an empty
payload when it was applied, or `kind=ERROR` carrying `STATUS{ERROR}` when it was refused
(RFC-0004 §D and its erratum of 2026-10-23). A `WRITE` with an empty `src` is answered by nothing,
whatever happened (RFC-0004 Amendment 2), so it can never be acknowledged. A node under memory pressure can also drop a
reply and count it (`reply_iov_dropped`, [22 §2](22-backpressure-and-sizing.md)), so a missing
reply means only that no answer arrived. Ask for the
acknowledgement by seeding a reply route; do not wait for a `STATUS` write on a subscription.
Those are application data on the standing plane, not acknowledgements.

The originator matches the reply to its request from the reply's own route:

- The reference core's `fwd_router_t::originate` gives each request a reply route of its own, a
  fresh one-segment token, unless the caller names one.
- The TypeScript client matches by **`src` suffix**: a reply goes to the oldest outstanding
  request whose `dst` ends with the reply's `src`
  ([#1530](https://github.com/avatarsd-llc/libtracer/issues/1530);
  [04 §Multi-hop FWD forwarding](04-communication-flows.md#multi-hop-fwd-forwarding)).

**The suffix caveat.** Two outstanding writes to the **same path** have the same suffix, so the
suffix cannot tell their replies apart, and matching falls back to arrival order. Replies on one
link carry no ordering promise, so the fallback can hand each caller the other's answer. If it
matters which attempt was answered, give each attempt a **fresh reply route**: a reply endpoint of
its own, so the reply's `dst` names the attempt. That is sender-side only. No other node reads the
route as anything but a path, and nothing changes on the wire.

*Forthcoming:* [RFC-0030](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0030-host-api-walks-the-graph-reply-is-a-remote-write.md)
§8–§9 (accepted; implementation
[#1946](https://github.com/avatarsd-llc/libtracer/issues/1946)) turns the reply into an ordinary
`WRITE` to the requester's reply endpoint, an empty `STATUS` for an applied `WRITE` and an `ERROR`
for a refused one, matched by the token the requester seeds as the last segment of `src`. A fresh
token per attempt is then the rule, and a duplicate or late reply is dropped and counted at the
reply endpoint. The pattern on this page does not change: the token is still a `src` segment, not a
field.

### A retry is safe because of the operation's shape

When no reply arrives, the application cannot know whether the operation was applied: the request
or the reply may have been lost (the lost-ack ambiguity). The retry is safe only if applying the
operation twice leaves the same state as applying it once. Design each operation to be like that:

| Operation | Why repeating it is safe |
| --- | --- |
| **Link create** | It names the link it creates. A repeated create answers `PATH_IN_USE` and creates no second link ([ADR-0073](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0073-naming-authority-the-application-mints-one-predicate-gates.md)). Read `PATH_IN_USE` after a retry as "already done". |
| **Link clear** (a `NAME` written to the creator endpoint) | It names the link to remove. A name already gone is a no-op success ([RFC-0014](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) §2; `core/src/transport_vertex.cpp:transport_vertex_t::endpoint_remove_locked`), so a retry answers `RESULT`, unlike a repeated create's `PATH_IN_USE`. Do not wait for an error to mean "already done". Other removals, such as retiring an application vertex, carry no such rule: check each one before retrying it. |
| **A `:children` write** on an application vertex | Creating a child is a `SPEC` that names it, so a repeat answers `PATH_IN_USE`, as a link create does, and never makes a second child (`core/src/graph_fields.cpp:graph_t::create_child`). |
| **A settings write** | Each settings write sets one field, or one `settings.app.*` leaf, to a whole value, so a repeat stores the same value ([03 §Atomicity of multi-field writes](03-addressing.md#atomicity-of-multi-field-writes)). Knobs that must land together go in one `settings.app.*` leaf. |
| **An OTA or upload chunk** | Address each chunk by its **offset** (in the path or the payload). A repeat overwrites the same bytes with the same bytes. Do not append. |
| **A command verb** | A verb that acts ("open the valve", "start the run") is not idempotent by shape. Put the application's own **sequence number** in the payload, and have the handler drop one it has already applied. That state is the application's, in its handler; the library keeps none. |

An operation that is none of these, for example one that increments a value, is made safe the same
way as a command verb, or by rewriting it as a whole-value write.

### The deadline and the retry belong to the application

The library starts no timer and reads no clock for a one-shot operation, and the link stays
fire-and-forget. The requester owns the deadline, as it already does for an `AWAIT`
([RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md) Amendment 3). The application
decides how long to wait, on its own clock:

1. Send the `WRITE` with a reply route, and note the attempt's deadline from the application's own
   `now`.
2. When the reply arrives, the operation is done: `RESULT` means applied, and `ERROR` carries the
   reason. A `BACKPRESSURE` error asks for a slower retry, not a different operation
   ([22 §2](22-backpressure-and-sizing.md#2-the-two-pressure-arms-stated-once), the reliable arm).
3. When `now` passes the deadline with no reply, cancel the pending request, then retry, with a
   fresh reply route if attempts must be told apart, and back off between attempts. A reply that
   arrives after the cancel answers a request that no longer exists, and is ignored.
4. On a link that acknowledges nothing (UDP, CAN), this loop is the only acknowledgement there is.
   On every link, a missing reply means "no answer arrived", not "the peer is gone"
   ([04 §Waiting for a reply that a drop consumed](04-communication-flows.md#waiting-for-a-reply-that-a-drop-consumed)).

## Confirming a stream: none, a per-op reply, or a batched watermark

A stream needs a different answer from a one-shot operation. **The library adds no acknowledgement
for a stream or a batch, and none is forced on any flow.** Confirmation is always the application's
choice. A subscriber delivery never replies: its `src` is empty, so nothing answers it, whatever
happened (RFC-0004 Amendment 2). When a stream's sender needs to know what arrived, the receiving
application tells it, at application level, with one small write that covers a whole batch.

### Pick one of three

| | No confirmation | A per-op `REPLY` | A batched watermark |
| --- | --- | --- | --- |
| **Use it for** | latest-value fields and streams where freshness beats completeness | one-shot operations ([above](#acknowledged-one-shot-operations-and-safe-retry)) | a stream whose sender must release or re-send what did not arrive |
| **Round trips** | none | one per operation, and the sender waits for it | none waited on; one small write per batch, travelling the other way |
| **Sender RAM** | nothing | one pending request per outstanding operation | the frames it chooses to keep until they are confirmed: its own buffer, which it sizes |
| **Receiver RAM** | nothing | one reply frame per operation, drawn from egress, and droppable under pressure (`reply_iov_dropped`, [22 §2](22-backpressure-and-sizing.md#2-the-two-pressure-arms-stated-once)) | one sequence number in its handler |
| **How a loss shows** | a counted drop, or a gap marker on a STREAM ring ([22 §2](22-backpressure-and-sizing.md#2-the-two-pressure-arms-stated-once)) | no reply before the application's deadline | the watermark stops advancing |

**A stream never uses ping-pong per frame.** Waiting for a reply to each frame before sending the
next caps throughput at one frame per round trip, keeps a pending request alive for every frame in
flight, and puts a reply frame on the return path for every frame sent. On a lossy link it also
stalls the stream on every lost reply. A per-op `REPLY` is for operations, not for frames.

### The batched watermark

1. **The sender numbers its frames.** It puts its own sequence number in each frame's payload. The
   library adds no sequence number to a delivery; this one is the application's.
2. **The receiver tracks how far it has got.** Its handler keeps the highest sequence number up to
   which every frame has arrived. When the number jumps, frames were lost on the way.
3. **The receiver writes back "received up to N".** Every K frames, or on its own clock, it writes
   one value carrying `N` to a vertex the sender owns for the purpose (for example
   `/app/cam/confirmed`), through the mount that reaches the sender. The write has an empty `src`,
   so nobody waits for it and nothing answers it.
4. **The sender acts on it when it arrives.** It releases the frames it kept up to `N`, and it may
   re-send from `N + 1`. Both are application decisions; the library keeps no window and starts no
   timer.

A lost watermark costs nothing: the next one carries a larger `N` and replaces it, because it is a
whole-value write, and repeating it stores the same state ([§A retry is safe because of the
operation's shape](#a-retry-is-safe-because-of-the-operations-shape)). The receiver chooses K, and
with it the trade: a smaller K frees the sender's buffer sooner and costs more writes on the return
path; a larger K costs the sender more buffer.

*Forthcoming:* [#2252](https://github.com/avatarsd-llc/libtracer/issues/2252) (approved, v0.23.0)
covers the latest-value case without any confirmation. When a latest-value (conflate) frame to a
peer is dropped rather than parked, the egress slot for that peer and vertex marks it dirty in one
bit, aiming for 0 B, and once the link can be written again the vertex's **current** value is
re-encoded and sent. The drop is still counted, nothing new goes on the wire, and the subscriber
still sends no reply. A latest-value field therefore reaches every subscriber once there is room,
and the watermark is left for STREAM flows, where every frame matters.

## Pitfalls

- **Treating a remote vertex as a replica.** There is no copy anywhere. A value a consumer
  holds locally is a *delivery* it received, not a mirror of the producer's store; a mirror is a
  deliberate application role a vertex may implement ([11](11-vertex-roles-and-aggregation.md)
  role 3), never something the graph maintains.
- **Expecting a path to be portable between nodes.** It is not: the address encodes the route
  from the caller's own root. A path composed on one node and handed to another names something
  else, or nothing. The composable thing a third party mints offline is the target *in the
  producer's frame* (RFC-0021 §4.B.1) — which is a different sentence from "the path is global".
- **Reading a successful `:subscribers[]` append as proof the flow works.** The fan-out gate
  answers on the producer at bind time; the fan-in gate answers on the consumer at *delivery*
  time. A bind the producer accepts can still be denied on every delivery, and across a node
  boundary the orchestrator that issued it may be gone by then
  ([13](13-network-formation.md) §pitfalls).
- **Assuming a partition will be signalled.** On UDP and CAN the link-departure hook never fires
  (RFC-0009 §D.5), and even where it does, it evicts rather than parks — a consumer that does not
  re-issue its subscribe-write after a reconnect has a link that is up and a flow that is gone.
- **Expecting a retired remote vertex to announce itself.** It answers `not_found` and nothing
  else. Any UI that shows a device's endpoints must treat disappearance as a poll result, not as
  an event it will be told about (RFC-0009 §C).
- **Building a topology walk on transport addresses.** `FWD` loop-freedom protects a *delivery*
  because `dst` is consumed monotonically; it protects nothing about a recursive enumeration. A
  walker keys its own visited set on the `:identity` record, or it revisits the same node through
  a second link and does not terminate ([07](07-host-embedding.md) §loop safety).

## Where the pieces are specified

| Concern | Canonical home |
| --- | --- |
| Path grammar, address scopes, canonicalization | [03 — Addressing](03-addressing.md) |
| Per-operation sequences, including partition and recovery | [04 — Communication flows](04-communication-flows.md) |
| `FWD`, `FIELD`, `PATH_REF` byte layouts | [05 — Protocol TLVs](05-protocol-tlvs.md) |
| Per-host DAG, global topology, loop safety, node identity | [07 — Host embedding](07-host-embedding.md) |
| Building a cross-node graph: discover → create → bind → depart | [13 — Network formation](13-network-formation.md) |
| Header-elided framing and the in-transport `identity↔path` map | [14 — CAN transport](14-can-transport.md) |
| The normative remote-addressing model | [RFC-0004](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md) |
| Canonical vocabulary (path-as-route, vertex ref, path element, peer symmetry) | [CONTEXT.md](../../CONTEXT.md) |
