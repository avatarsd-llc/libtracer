# Reference 19 — Transports are vertices

```{admonition} In one paragraph
:class: tip
A transport is not a side channel bolted onto the graph — it **is** a vertex in it. Every
connection a node holds is an ordinary `/` vertex at `/net/<module>/<name>`: created by an
in-band write, addressed by path, admitted by the same ACL gate, `await`-able and
subscribable on its liveness value, and retired by the same retire as any other vertex
([ADR-0027 — A transport, and each connection within it, is a first-class `/` vertex](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0027-transport-and-connections-are-vertices.md)).
That is what lets a third party form a whole network with nothing but vertex writes
([13 — Network formation](13-network-formation.md)) and lets a caller address a peer's
sensor without opening a socket or naming a URI ([07 — Host embedding](07-host-embedding.md)).
It is not free, and the costs are stated below: a mount run spends path bytes at every hop,
the vertex and the link it names are two lifecycles that can disagree, the plane mints
structural vertices nobody declared, and creation runs a socket constructor inside a
control-plane critical section on a receive thread. The rule that keeps the price bounded is
**leanness**: the shared `tr::net::conn_settings_t` carries only the keys every transport
kind means the same thing by, and a kind's private config is parsed by that kind's own
factory out of the raw config TLV
([ADR-0043](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0043-quic-webtransport-optional-module-msquic.md) §3, §5).
```

This page is the canonical statement of the commitment itself — *what* is a vertex, what it
buys, what it costs, and the standing rule that bounds the cost. It is not the formation
flow (that is [13](13-network-formation.md)), not the per-hop routing story (that is
[CONTEXT.md §Path-as-route](../../CONTEXT.md) with [07](07-host-embedding.md)), not the CAN
specifics ([14](14-can-transport.md)), and not the key-by-key config vocabulary (that is the
[connection-config module page](../modules/connection-config.md)). The reference
implementation's seam is `tr::net::transport_vertex_t`
(`core/include/libtracer/transport_vertex.hpp`, `core/src/transport_vertex.cpp`); the
original ticket is [#83](https://github.com/avatarsd-llc/libtracer/issues/83).

---

## What is a vertex, and what is not

The net plane registers four kinds of thing in the path tree. They differ in role, and the
differences are load-bearing:

| Position | What it is | Role | Value |
| --- | --- | --- | --- |
| `/net` | The net root — the `:children[]` creation target the constructor registers. Conventionally `/net`; the name is the constructor's default, overridable per node, never a library rule. | `role_t::STORED_VALUE` | none (a structural position) |
| `/net/<module>` | One **module** — a *(transport kind, role)* pair, declared by the application (`register_module`, `core/include/libtracer/transport_vertex.hpp:379`). Minted eagerly when the module is declared (`core/src/transport_vertex.cpp:210`) and lazily on first creation (`:525`). | `role_t::STORED_VALUE` | none (a structural position) |
| `/net/<module>/conn` | The module's **creator endpoint**. A write is *executed*, never assigned: the payload's TLV type selects create (`SPEC`) from remove (`NAME`) — `core/src/transport_vertex.cpp:255`. | `role_t::HANDLER` | none — write-only and valueless |
| `/net/<module>/<name>` | The **connection vertex**: one link's identity, its config, and (when config-constructed) the socket it owns. | `role_t::STORED_VALUE` | the 1-byte link-liveness state |

Three things are deliberately **not** vertices:

- **A peer of a bus link.** A bus transport's audible peers are synthesized on every read of
  the connection vertex's `:children[]` from the transport's own live-traffic table; no
  vertex is ever created for a peer, so a node stays O(its own links) rather than O(the
  network) ([ADR-0044](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md);
  [07](07-host-embedding.md) §node identity).
- **A connection's config.** `addr`, `port`, `role`, `keepalive`, `max_frame`, `backoff` and
  `connect_timeout` are creation-time config carried in the `SPEC`, parsed into the
  transport-private `conn_settings_t`. They are not a vertex `:settings` surface — that core
  namespace was emptied outright by
  [RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
  §3.B — and they are not `/` children either.
- **`:stats`.** ADR-0027's worked example showed a per-connection `:stats` facet. It was
  never implemented and is not in the field namespace; a read or write of it answers
  `ERROR{tr::schema::not_found}` (ADR-0027 erratum 2026-07-30,
  [#583](https://github.com/avatarsd-llc/libtracer/issues/583); whether it should exist is
  [#584](https://github.com/avatarsd-llc/libtracer/issues/584)).

The dividing rule is ADR-0027's, unchanged: **distinct lifecycle and identity ⇒ a `/`
vertex; scalar config of a thing ⇒ config on that thing.** A connection is created and
destroyed, has up/down state to observe, and has its own ACL, so it is an identity. A port
number is not. Only the *spelling* of the second half has moved: ADR-0027 wrote it
`:settings`, and RFC-0022 §3.B later emptied that namespace, so a connection's scalars now
live on the transport-private record reached through the kind's own config door
([ADR-0021](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0021-colon-field-plane-is-the-vertex-ioctl.md)
§Decision 3, device-private facets).

---

## What "is a vertex" buys

### Uniform addressing

A connection vertex **mounts its peer's graph under itself**: its `:`-facets are the link's
own control surface, and its `/`-subtree is the peer's tree, reached by forwarding the
unresolved suffix ([CONTEXT.md §Path-as-route](../../CONTEXT.md)). So `/net/can/can0/wheel/left`
is one address that a caller reads with the same call it uses on `/sensor/temp` — no socket
handle, no URI, no destination field, and no second naming layer
([07](07-host-embedding.md) §per-host view).

This is not merely a spelling convenience: it is why the mount path and the routing key are
the *same string*. Creation composes `net/<module>/<name>` once and registers it both as the
graph key and as the router's child name — "the routing key IS the mount path"
(`core/src/transport_vertex.cpp:500`, `:507`,
[ADR-0061](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)) —
so a hop's `src` prefix needs no per-hop assembly, and a route through a link cannot name a
vertex the graph does not have. A design that kept transports outside the tree would have to
keep those two namespaces in agreement by hand.

### Uniform lifecycle

Creation and removal are ordinary writes, gated by the ordinary write path, and they collapse
onto one control distinguished by the written TLV's type
(`core/src/transport_vertex.cpp:255`). Removal un-routes first, then retires the identity
vertex, then destroys the socket — so a forward can never reach freed memory, and the path
re-virginizes for a later connection of the same name
([RFC-0009](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §B.6).

The uniformity extends *inward*, not only outward. There are two creation doors — the
[RFC-0014 — Creator endpoint: connection lifecycle and link liveness](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
creator endpoint and the superseded `:children[]` spelling
(`core/src/transport_vertex.cpp:107`, `:111`) — and they share one body from the staged-link
lookup onward, so they cannot drift into two creation semantics. Only the module resolution
differs: the endpoint knows its module positionally, from its own path; the `:children[]`
door derives it from the SPEC's `kind` through a declared *(kind, role)* mapping, or from a
unique staging (`core/src/transport_vertex.cpp:451`).

```{mermaid}
flowchart TD
    W["write /net/ws-server/conn = SPEC{name, config}"] --> G["graph_t: WRITE gate admits, then on_write"]
    G --> D{"payload TLV type"}
    D -->|"SPEC"| C["resolve the module's declaration: it fixes kind and role"]
    D -->|"NAME"| R["remove: un-route, retire the vertex, close the socket"]
    D -->|"anything else"| E["ERROR type_mismatch — never an ordinary assign"]
    C --> S{"a link staged for this module/name?"}
    S -->|"yes"| L["use the staged transport (borrowed; the caller owns it)"]
    S -->|"no"| F["the kind's factory constructs the socket (the vertex owns it)"]
    L --> V["register the identity vertex at /net/module/name"]
    F --> V
    V --> RT["router add_child: the NAME-to-link demux entry"]
    RT --> LS["publish liveness: UP for DIAL, LISTENING for LISTEN"]
```

### Uniform introspection

**The connection vertex's value is its liveness state** — a 1-byte `link_state_t`
(`core/include/libtracer/transport_vertex.hpp:99-106`) emitted as an ordinary `VALUE` TLV
(`core/src/transport_vertex.cpp:73`). Because it is a vertex value and not a side-channel
callback, all three primitives already work on it: `read` it, `await` it, or **subscribe** to
`/net/<module>/<name>` and receive every transition without polling (assign-then-deliver,
[RFC-0008](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0008-vertex-operations-assign-propagate.md) §D).
A monitoring peer needs no transport-specific verb and no per-transport statistics facet —
which is why the absence of `:stats` costs less than it looks.

The same holds for enumeration: `/net:children[]` lists the modules, `/net/<module>:children[]`
lists that module's connections, and a bus connection's `:children[]` lists the peers audible
right now. One walk sees a node's whole wiring.

---

## What it costs

### 1. Every hop spends path bytes

A mount run is three segments (`net/<module>/<name>`, plus a fourth for a bus peer), and a
forwarding hop consumes the whole run rather than one segment (RFC-0014 S2a, ADR-0061). The
segment cap is 255, but the 1024-byte `PATH` budget binds first: at 20 B/hop for
`/net/can/c0` and 32 B/hop for `/net/ws-client/board-01`, the reachable diameter is roughly
**30–50 hops** ([13](13-network-formation.md) §connection direction and folding, arithmetic
over [RFC-0023](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0023-path-segment-cap-repriced-32-to-255.md)
§4.3 rather than a routed measurement). A transport kept outside the path tree would spend
none of that budget. Putting it in the tree is what makes the route explicit and loop-free by
construction, and the bytes are what that costs.

### 2. Two lifecycles under one identity

The vertex is explicit — created solely by a `SPEC` write or an owner-local registration,
removed solely by a `NAME` write or an owner-local retire. The link underneath is a state
machine that is supposed to run itself (RFC-0014 §4). They can disagree, and the disagreement
is visible on the wire: a vertex can exist while its link is dormant, and a `LISTEN` vertex
reporting `listening` says its listen socket is bound — **not** that any peer is attached.

**The liveness engine is not implemented.** Today the value is written by whoever knows: a
config-constructed socket publishes `UP` or `LISTENING` at creation
(`core/src/transport_vertex.cpp:657`, `:659`), and a provided link reports through
`set_link_state`. The automatic dial / backoff / reconnect transitions RFC-0014 §4 describes
are RFC-0014 S5 and do not run yet.

### 3. Structural vertices nobody declared

Making the plane addressable means minting positions that hold no application datum: the net
root and each `/net/<module>` segment. They are registered with `role_t::STORED_VALUE` and
carry no field descriptor table, so an embedder walking `graph_t::for_each_vertex` sees them
as value vertices whose owner forgot to describe them — byte-identical `:schema` shape,
differing only in the NAME. Only the object that minted them can tell:
`transport_vertex_t::is_structural(key)`, and the graph deliberately answers no such question
in general, because an application's own position-holder (a `/zone` with nothing but
children) is indistinguishable from a connection vertex on every graph-visible surface
([11 — Vertex roles and aggregation](11-vertex-roles-and-aggregation.md) §structural
vertices, [#1096](https://github.com/avatarsd-llc/libtracer/issues/1096)). Even that
predicate answers by **name match, not provenance**: a vertex an application registered at
`/net/<module>` first answers `true`.

### 4. Control-plane work runs on a data thread

Because creation is a write, it arrives on whichever transport's receive thread delivered the
frame, and the graph invokes it outside its own map lock. So the class serializes every
control-plane mutation on its own mutex, and that critical section **constructs sockets** —
it can block for milliseconds, which is why it is a plain mutex rather than an
interrupt-disable section or a spinlock
([ADR-0063](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0063-connection-table-lock-free-reads-trait-serialized-writes.md)
§3 and its erratum 1). The declared lock order is `transport_vertex_t` → `fwd_router_t` →
`graph_t` → the vertex stripe, and **nothing on the forward or delivery path takes any of
them**. A peer's `write` to a creator endpoint can therefore dial a socket; a peer's data
write never waits behind one.

### 5. Creation is a peer-drivable resource

A door on the wire is a door an unfriendly peer can knock on. Two mitigations are in the
tree, and one is not:

- The registry's refusal is the whole creation's verdict: when `add_child` cannot grow, the
  creation rolls back — retire the vertex, drop the entry, destroy the socket — and answers
  `BACKPRESSURE` (`core/src/transport_vertex.cpp:629`, `:632`). Without that, a bounded node
  could be driven to publish connections that no `dst` resolves and no removal can take down.
- `SPEC` naming an existing name answers `PATH_IN_USE`, and the reserved `conn` name is
  refused in both directions, so the endpoint cannot be made to destroy itself.
- **Not yet true:** the `CREATE`(0x08)-for-create / `WRITE`(0x02)-for-remove gating split of
  RFC-0014 (S2c) is not implemented. Today the endpoint is admitted by the ordinary write
  gate — `graph_t::store_value` runs a handler's `on_write` only after `acl_allows` admitted
  the write, and hands it the identical subject — so create and remove are one right, not two
  ([13](13-network-formation.md) §Realisation status).

### 6. No reconfiguration door

The only accessor for a connection's parsed settings, `transport_vertex_t::settings_of`,
hands out a **const** view, and there is no post-creation config write: a re-`SPEC` is
`PATH_IN_USE`, never a reconfiguration. A peer whose IP changed is therefore retired
(`NAME`) and re-created (`SPEC`), which un-routes the link and cascade-evicts the
subscriptions routed through it. That is the cost of "the vertex is the identity, the config
is creation-time": there is no in-place edit of a thing whose whole existence is defined at
creation.

### 7. The config vocabulary is not yet self-describing

Uniform introspection covers the liveness value and the children, but not the *creation
catalog*: `:schema`-as-catalog on the creator endpoint is RFC-0014 S3 and is not implemented.
Worse than absent — the endpoint is hidden from the module's `:children[]` (S4), so §6's
`read <module>/conn:schema` probe is the *only* sanctioned way to find it, and that probe
currently answers the generic whole-vertex `:schema` (an EMPTY `SETTINGS`) rather than the
module's catalog. So a creator today must know a kind's config keys out of band, from the
[connection-config module page](../modules/connection-config.md), rather than by reading the
endpoint. Stated plainly because it is the one place the "everything is in the graph" claim
does not yet reach.

---

## The lean rule: no kind-specific fields on the shared record

**Standing requirement.** `tr::net::conn_settings_t` carries only the keys **every** transport
kind means the same thing by. A kind's private configuration never lands there — the kind's
own factory parses it from the raw config `SETTINGS` TLV it receives alongside the parsed
universal settings (`core/include/libtracer/transport_vertex.hpp:123`, ADR-0043 §3, §5).

The mechanism is the factory signature: a factory is
`(const conn_settings_t&, const wire::tlv_t* raw_config) -> result_t<unique_ptr<transport_t>>`,
registered at runtime through `transport_vertex_t::register_transport_type`
(`core/src/transport_vertex.cpp:140`). The central parse reads the universal keys and nothing
else (`core/src/transport_vertex.cpp:49`, `:60`); unknown pairs are ignored, so a newer peer
may send keys this node has never heard of. `quic` reads its own `cert` / `key` / `ca` /
`insecure`, `ws` and `tcp` read `peer_named` / `max_peers`, `can` reads its bus identity —
and none of them can see another's vocabulary
([connection-config](../modules/connection-config.md)).

### Why the rule exists

- **Optionality.** QUIC is a separate link target that the core never references; a device
  without the module contains zero QUIC schema (ADR-0043 §1, §5). A `cert` field on the
  shared record would put that vocabulary — and its bytes — on every node, including the
  16 KB class that cannot carry TLS at all.
- **Open/closed.** `transport_vertex.cpp` never learns what msquic is. Adding a transport
  kind is linking a target and calling one registration function; it is not an edit to the
  file that composes paths. That is what makes an out-of-tree kind — an embedder's own — a
  first-class participant rather than a fork.
- **A shared key that only one kind reads is a dead key.** The failure mode is already
  visible *inside* the universal set: `keepalive_ms` is parsed and no consumer anywhere in
  the tree reads it, and `backoff_ms` / `connect_timeout_ms` are parsed but dormant until the
  S5 liveness engine lands (`core/include/libtracer/transport_vertex.hpp:151`, `:154`;
  [13](13-network-formation.md)). One record carrying N kinds' private vocabulary would be
  that condition by construction rather than by accident — and a mistyped or misplaced key is
  silently ignored, so nothing would report it.
- **Layering.** L4 never learns what a `client` or a `listener` is: the graph owns the
  addressing, and this `tr::net` seam owns the catalog entry (the file header of
  `core/include/libtracer/transport_vertex.hpp`). A kind-private field on the shared record
  drags one kind's vocabulary up into the layer that composes every path.

### What the rule is not

It is not "the record never grows". `max_frame` was added to the universal set later, as a
per-connection receive cap that every framed kind honours — the length-prefix streams read it
off their u32 prefix, `ws` off the RFC 6455 frame header, `0` meaning the protocol default,
and it only ever tightens. That is the test a candidate key must pass: **every kind reads it,
and every kind means the same thing by it.** A key that fails the test belongs in the kind's
factory, however convenient the shared record looks.

### The same discipline, one layer down (NARROW / MID / WIDE)

Where a kind needs something the plane owns, the plane exposes a **seam**, not a field.
`transport_vertex_t::egress_source` hands an out-of-tree factory the very egress store the
built-in factories wire into their sockets, so a `quic` or `can` link is bounded by the same
budget rather than silently falling back to the process heap. That composition has a
deliberate spectrum
([ADR-0079](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0079-allocation-store-composition-defaults-to-per-plane-mid.md)):
the default is **MID** — one store per plane, which fences a net-plane flood off from the
graph; a **WIDE** deployment folds everything onto one store; a **NARROW** one ignores this
accessor and injects its own per-thread or per-connection source, buying zero contention at
the cost of per-store slack (≈14 KB on an ESP32-C6-class target, an estimate ADR-0079 marks
as owing a measured high-water-mark census). The point for this page: the knob is an injected
store reached through an accessor, not a `store` field on `conn_settings_t` — the same rule,
applied to a resource instead of a config key.

---

```{admonition} Realisation status
:class: note
**Implemented.** Connection vertices at `/net/<module>/<name>`, mounted and routed at that
same key; the per-module creator endpoint and its `SPEC`/`NAME` dispatch (RFC-0014 S2b),
including wire-driven removal; the liveness *value* as the vertex value, `await`-able and
subscribable; application-declared modules with no library-derived fallback
([ADR-0073](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0073-naming-authority-the-application-mints-one-predicate-gates.md) §4);
the structural-vertex predicate; the lean factory signature and the runtime kind catalog;
hiding `conn` from `/net/<module>:children[]` (S4 — the endpoint is not a member connection,
but stays addressable so the §6 creatability probe still reaches it).

**Not implemented.** The liveness engine that drives the DIAL transitions (S5) — values are
written by the creating call and by `set_link_state`; `:schema`-as-catalog on the endpoint
(S3); the `CREATE`/`WRITE` gating split (S2c). RFC-0014's byte-level clauses — the liveness encoding among them — become normative on
its conformance-vector merge; until then the values in
`core/include/libtracer/transport_vertex.hpp:99-106` are the reference encoding.
```

## Pitfalls

- **Reading `listening` as "a peer is attached".** A `LISTEN` vertex's liveness reports
  listen-socket reachability only. Accepted peers are the connection vertex's synthesized
  `:children[]`, and a server with zero peers is healthy.
- **Treating the connection vertex as a config record.** Its value is liveness; its config is
  creation-time and const thereafter. There is no `:settings` edit that moves a peer's
  address, and a re-`SPEC` answers `PATH_IN_USE`.
- **Assuming a delegated create right exists today.** The `CREATE`/`WRITE` split is specified
  and unimplemented; a peer that may write the endpoint may both create and remove.
- **Inferring structural-ness from path shape.** "Two segments under the root" claims every
  application `/zone/<child>` as well. Ask the object that minted the vertex.
- **Adding a kind's key to `conn_settings_t` "just for now".** The record is shared by every
  kind and by every node that links the core; a key only one factory reads is a key the other
  kinds carry and no one can be told about (the `:schema` catalog that would advertise it is
  S3, unimplemented).
- **Expecting a per-transport statistics facet.** `:stats` was never implemented; a read of
  it is `tr::schema::not_found`.

## Boundaries

- **The peer's tree below a mount is not this page.** Everything about how the suffix is
  forwarded, stripped and replied to is [CONTEXT.md §Path-as-route](../../CONTEXT.md),
  [07](07-host-embedding.md) and [13](13-network-formation.md).
- **"Transport" here means a connection, not a wire technology.** What a given kind does with
  the bytes — CAN's header elision and advertise map, WebSocket's session authentication —
  is [14](14-can-transport.md) and [16](16-websocket-session-auth.md).
- **A staged link is not yet a vertex.** `provide_link` (`core/src/transport_vertex.cpp:408`)
  hands the plane a pre-built transport — the test/manual seam for loopback channels and
  kinds the catalog does not cover — but it registers nothing. The vertex appears when a
  `SPEC` for that `<module>/<name>` binds it, and removing that connection leaves the
  borrowed link alone.
- **This is not an API page.** The reference implementation's signatures are its own headers;
  what is normative about creation, removal and liveness is RFC-0014 and the spec, and what
  is descriptive-but-canonical is stated here.
