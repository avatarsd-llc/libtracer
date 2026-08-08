<!--
SPDX-License-Identifier: CC-BY-4.0
SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
-->

# fwd-router — FWD source routing and the /net plane (L4)

```{admonition} In one paragraph
:class: tip
**`fwd_router_t`** is the node's hop-by-hop `FWD` forwarder and the whole of its remote-operation
surface. It binds one local `graph_t` to a set of named transport children held in a
**`child_registry_t`**, and on each inbound frame it does exactly one of three things: strip the
first `dst` segment and forward, resolve a local terminus and reply, or hand a fully-consumed
`REPLY` to the reply sink. It is **stateless per request** — the forward route is the shrinking
`dst` and the return route is the growing `src`, both carried in the frame — so a hop may reboot
mid-operation and the reply routes regardless. Connections themselves are vertices under `/net`
(`transport_vertex_t`), created in band and addressed at `/net/<module>/<name>`.
```

## What it does

The net plane is **explicit-source-routed `FWD` only**
([ADR-0040 — the net plane is explicit-source-routed only](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0040-net-plane-is-explicit-source-routed-only.md)).
A remote endpoint is addressed by its full path through transport vertices; each hop consumes the
segments that name its own link, so `dst` shrinks toward the target while `src` accumulates the way
back. That makes a route **loop-free by construction** and needs **no per-hop dedup state**: there
is no flooding, and no `(origin, ts)` suppression table. Two parallel links to the same peer are
therefore *two different explicit addresses* — deliberate redundancy chosen by the addresser, not
auto-multipath discovered by the router. `0x0D ROUTER` is a reserved, decodable wire code
(`type_t::ROUTER`, `core/include/libtracer/tlv.hpp:42`) with no implemented mechanism behind it;
source routing needs none.

Four dispositions. Three are decided by resolving the **first `dst` segment** against the registry; the fourth is decided by the `dst`'s own type code, because a bound address has no segment to resolve:

| First `dst` segment resolves to | Disposition |
| --- | --- |
| a registered transport child | **forward** — strip the segment from `dst`, prepend this node's NAME for the inbound link to `src`, scatter-gather onward |
| a local non-transport vertex | **terminus** — decode into an arena, apply the op, build `FWD{REPLY}`, send it back over the link the request arrived on |
| a `PATH_REF` `dst` with >1 element | **bound hop** — consume element 0 (bounds, generation, ACL at the dereferenced vertex), egress the residual over the link it names (§bound hop) |
| nothing, on a `REPLY` | **terminal reply** — the accumulated return route is fully consumed, so this node is the originator; the frame goes to the reply sink |

A `REPLY` routes by the same step but never accumulates `src`: a reply expects no reply
([RFC-0004 — remote operation addressing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0004-remote-operation-addressing.md) §B).

The forward hop builds **no decoded tree at all**. It reads the frame's headers by offset, writes
the shortened `dst` and grown `src` heads into small stack buffers, and hands the transport those
heads plus untouched views of the inbound frame. Only a terminus request decodes, and it decodes
into an arena drawn from a failable block source rather than the container resource — so a
peer-sized frame arriving behind no ACL cannot exhaust the heap silently. Registry lookups take no
lock; the control-plane mutex covers `add_child` / `remove_child` only, and the forward path never
takes it.

### The bound hop

A `dst` that is a `PATH_REF` rather than a canonical `PATH` takes a fourth disposition, and it is
decided by the element **count** rather than by the registry ([RFC-0024 — bound paths](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0024-bound-paths-node-scoped-vertex-ref-source-routing.md) §3.4/§5):
**one** element left means this node is the terminus and the element names its target vertex;
**more than one** means this node is a hop, and it consumes element 0 — bounds-check the index,
compare the generation, evaluate the ACL at the dereferenced vertex for the operation's own right
— then egresses the residual over the link that vertex names. No mount descent runs at all: the
`resolve_mount_*` family is not entered, there is no digest fold and no segment compare.

One peek decides all four dispositions. The `dst`'s form — canonical `PATH`, bound `PATH_REF`, or
neither — is read once, from the three headers a frame leads with, and the two forms are mutually
exclusive by that type code. Asking the canonical question and the bound question as separate
walks costs the bound form a whole second parse of the same bytes while buying the canonical form
nothing, and it measured a bound terminus slower than the canonical terminus it exists to beat.

The element→link join is one integer per child, recorded at `add_child`: a child's mount run **is**
its connection vertex's canonical key, so the router resolves that vertex once and remembers its
slot index. It is not a route table — one entry per link, sized by the graph and never by the
traffic — and a child registered before its connection vertex exists simply has none, which makes
every bound route through it fall back to canonical rather than misroute. A **bus** child never
records one, deliberately: a bus mount's own `send()` broadcasts and a bus peer has no vertex, so
no element can name either.

An opcode the build cannot name is dropped rather than forwarded: §6.2 evaluates the ACL for the
operation's **own** right, and a hop that does not know an opcode does not know its right, so
charging it the `READ` right that happens to be at hand is a guess a future write-like opcode
would cross a read-only gate on. A bound `REPLY` is refused the same way.

Any validation failure is a **drop**, and never a fall-through to the local terminus: a bound frame
this node cannot route is dropped, the origin still holds the canonical path the binding was minted
from, and re-resolving canonically and re-minting is its recovery. `src` accumulates canonically
throughout, so the reply of a bound request routes home through the ordinary descent and every hop
on the way back may be a peer that does not speak the bound form at all.

A hop that forwards a mint reply either contributes its element or **strips** the answer. Every
cannot-contribute case strips — no connection vertex for the inbound link, a saturated or retired
generation, and a list already at the 255-element cap — because a relayed list that skips a hop is
not a shorter route but a wrong one: the skipped hop would later find one element left, believe
itself the terminus, and dereference another host's element against its own vertex map.

The router also carries the origin's half — `connection_ref`, `bound_egress`, `adopt_binding` and
`bound_dispatch` — because both halves are the same act: consume element 0, dereference it,
egress. The origin's element is the one no peer can supply, since the hop out of this node is the
one hop nobody else sees.

Alongside the routing legs the router installs the graph's **remote-delivery sink**: a write to a
vertex that carries a remote subscriber fans out as `FWD{WRITE}` addressed by that subscriber's
stored return route, or — when the subscription is compact-flagged — as a lean `COMPACT` bearing a
label bound by a prior `ADVERTISE`.

## Interface

```cpp
namespace tr::net {

class fwd_router_t {
    // graph: terminus op resolution. mr: the route-handle label tables (ADR-0039 §1).
    // rx: the NOTHROW block source the terminus decode arena draws from (ADR-0065).
    // flat: the byte backend EVERY rope flatten draws from, forward AND terminus (#730/#766).
    // max_label_bindings_per_link: 0 = unbounded. egress: the reply-egress backend (#795).
    // FOUR independent memory seams, each defaulting to the global heap on its own.
    explicit fwd_router_t(graph::graph_t& graph,
                          std::pmr::memory_resource* mr = std::pmr::get_default_resource(),
                          mem::block_source_t* rx = &mem::heap_source(),
                          mem::mem_backend_t* flat = &mem::heap_backend(),
                          std::size_t max_label_bindings_per_link = 0,
                          mem::mem_backend_t* egress = &mem::heap_backend());

    // `name` is this node's mount RUN for `link`: the leading dst segments that route onward
    // through it, and the run prepended to src on the way back. ANY width (#523) -- the
    // descent makes one registry pass and matches each slot against the prefix of its own
    // width, so the only bound is the path-depth budget. Returns false, always (not only in
    // debug), for a name no address could ever name: empty, with an empty segment, or wider
    // than kMaxSegments. Optional per-child failable source; null falls back to the router's.
    bool add_child(std::string name, transport_t& link, mem::block_source_t* rx = nullptr);
    bool remove_child(std::string_view name);   // removal, not departure
    void link_down(std::string_view link_name); // departure: evict edges + drop label state

    // Bind a LOCAL producer's subscription toward a MOUNT-PATH target (#739): resolves
    // `target` through the SAME strip-K cached descent the forward path uses (ADR-0061),
    // derives (link, return route), and admits through graph_t::subscribe_wire — so a
    // caller never hand-splits, and an arbitrarily nested /net/A/net/B/x target works.
    // Bind-time resolution, link-lifetime durability: teardown drops the binding, and
    // re-binding is the application's job. A bus-PEER first hop answers INVALID_PATH.
    graph::result_t<void> subscribe_toward(const graph::path_t& producer,
                                           const graph::path_t& target);

    // Per-frame sinks: function pointer + opaque context, never std::function (ADR-0047).
    using reply_fn_t            = void (*)(void* ctx, const view::rope_t& reply);
    using inbound_fn_t          = void (*)(void* ctx, std::string_view inbound,
                                           const wire::tlv_t& fwd);
    using raw_fn_t              = void (*)(void* ctx, std::string_view inbound,
                                           std::span<const std::byte> frame);
    using compact_delivery_fn_t = void (*)(void* ctx, std::span<const std::byte> route,
                                           std::span<const std::byte> payload);
    using stale_label_fn_t      = void (*)(void* ctx, std::string_view inbound,
                                           std::uint16_t label);
    void on_reply(reply_fn_t, void* ctx = nullptr) noexcept;
    void on_inbound(inbound_fn_t, void* ctx = nullptr) noexcept;
    void on_raw(raw_fn_t, void* ctx = nullptr) noexcept;
    void on_compact_delivery(compact_delivery_fn_t, void* ctx = nullptr) noexcept;
    void on_stale_label(stale_label_fn_t, void* ctx = nullptr) noexcept;

    // Route-handle producer side (RFC-0004 §E.1).
    std::uint16_t advertise(std::string_view link_name, std::span<const std::byte> route_path);
    void send_compact(std::string_view link_name, std::uint16_t label,
                      std::span<const std::byte> payload);
    void clear_link(std::string_view link_name);

    void on_frame(std::string_view inbound_name, std::span<const std::byte> frame);
    const child_registry_t& registry() const noexcept;
    const route_handle_t&   handles()  const noexcept;
};

class child_registry_t {                 // the one NAME -> link demux table (ADR-0037)
    struct child_t { std::string name; std::atomic<transport_t*> link; /* tombstone = null */
                     std::atomic<bool> multi_peer; std::uint64_t name_digest;
                     std::vector<std::byte> mount_tlv; bool live() const noexcept; };
    bool add(std::string name, transport_t& link);        // rebinds; false = no slot
    bool erase(std::string_view name);                    // tombstones in place
    // ONE pass, each slot matched against the prefix of its OWN seg_count (#523) — so a
    // mount of any width resolves and there is no per-width retry. Longest match wins.
    template <class SegAt> const child_t* longest_prefix(SegAt&& at) const;  // the demux
    transport_t*   by_name(std::string_view name) const;
    static transport_t* resolve_peer(const child_t&, std::string_view peer);
    std::size_t size() const noexcept;  std::size_t live_size() const noexcept;
};

}  // namespace tr::net
```

Signature source: `core/include/libtracer/fwd_router.hpp:176` (constructor), `:245`
(`add_child`), `:301` (`subscribe_toward`), `:397-409` (the sink function-pointer types);
`core/include/libtracer/child_registry.hpp:209` (`add`), `:458` (`resolve_peer`), `:473`
(`erase`), `:499` (`entry_by_name`), `:520` (`by_name`), `:561`/`:571` (`size`/`live_size`).

## Routing one inbound frame

```{mermaid}
flowchart TB
    IN["inbound frame on child NAME"] --> RAW["on_raw observer"]
    RAW --> PEEK["offset peek: first dst segment"]
    PEEK --> DEMUX{"child_registry_t<br/>longest_prefix"}
    DEMUX -->|"resolves to a link"| FWD["strip dst segment<br/>prepend inbound NAME to src"]
    FWD --> SG["stack-built heads<br/>+ untouched frame views"]
    SG -->|"send(iov)"| OUT(["transport_t"])
    DEMUX -->|"no link, op != REPLY"| TERM["arena decode<br/>(nothrow block source)"]
    TERM --> RES["op_resolver_t against graph_t"]
    RES --> RPL["FWD{REPLY} back over the inbound link"]
    DEMUX -->|"no link, op == REPLY"| SINK["reply sink<br/>rope-native, no flatten"]
    classDef zc fill:#dcfce7,stroke:#166534
    class SG,SINK zc
```

## Consequences

- **Stateless hops.** No per-request table means no timeout sweeper, no correlation map, and no
  memory that scales with concurrent operations. The cost is that the frame carries its own route:
  address size grows with hop count, which is what `ADVERTISE`/`COMPACT` route handles exist to
  amortise on a steady flow.
- **A reply is delivered as a rope, never flattened by the router**
  (`core/include/libtracer/fwd_router.hpp:411-420`). A sink that wants contiguous bytes holds
  `const view_t m = reply.materialize()` and reads `m.bytes()`; a **single-link reply — the common
  case — is returned zero-copy, no allocation and no copy**, and only a multi-link reply pays one
  flatten, on demand. The escape hatch sits at the consumer, so the router never pays for a
  consumer that did not need contiguity. `m` must stay alive while its span is read.
- **The default delivery leg copies nothing.** A full-route `FWD{WRITE}` fan-out scatter-gathers a
  fresh stack head, the stored return-route bytes, an empty `src`, and one span per link of the
  stored value (`core/src/fwd_router.cpp:1809`). The `COMPACT` leg is the one that flattens,
  because a `COMPACT` wraps a contiguous payload (`core/src/fwd_router.cpp:1772`) — single-link, that
  flatten is a zero-copy adopt, and multi-link it draws from the router's injected `flat` backend
  (#730), not the global heap.
- **All rope flattens on the forward AND terminus paths draw from the injected seam.** `flat`
  started (#730) as the router's own four sites — the two ingress control-frame sub-rope flattens,
  the cold bus-name rejection flatten, and the per-delivery `COMPACT` egress one. The terminus half
  was not covered: the resolver's rope-tier flattens, one call below `resolve_terminus_rope`, took
  `rope_t::materialize`'s default global-heap backend, so a **fragmented** request from a peer
  allocated outside a bounded node's slab no matter what it had injected. The router now passes the
  same pointer to its `op_resolver_t` (#766), which threads it through the rope-tier node reader,
  so one injection covers both paths. A refused terminus flatten is answered by value: an addressed
  `kind=ERROR STATUS{BACKPRESSURE}` reply, or — when the refusal hit the reply's own route bytes,
  leaving no trustworthy address — a drop. Never a reply built on a short span. Since #793 it also
  covers the rope tier's **single-link** ownership copy, which used to reach the global heap
  through `view::over_bytes` — so `own_wire`'s two branches no longer allocate from two different
  allocators depending on where the peer's fragmentation fell. Since #801 it covers the **span
  (arena) tier's** `own_wire` as well — that tier's only allocating site, and the one a
  span-delivering child (a synchronous CAN/UART link) takes on every ordinary WRITE — so which
  allocator a stored value's bytes come from no longer depends on which transport delivered the
  frame. What `flat` still does **not**
  cover: the terminus arena (that is `rx`'s nothrow `block_source_t`, bounded by a different seam,
  not unbounded) and the reply head segment (genuinely unbounded on both tiers; it answers
  exhaustion by value, and folding it into `flat` would silently re-scope an injection callers have
  already sized — see
  [failable allocation and backpressure](../design/allocation-and-backpressure.md)).
- **Delivery drops rather than aborts on the value path — with one named residual.** The flatten,
  frame build and `iovec` reserve on the writer thread are all failable: each **drops that one
  delivery** — a subscriber misses a value under exhaustion, which is valid delivery behaviour —
  instead of raising an exception that `-fno-exceptions` would turn into `abort()`. A dropped fresh
  `ADVERTISE` self-heals through the peer's `HANDLE_NACK`. The residual is the label store: a
  **compact-flagged** flow's first delivery on a link resolves its label *before* those three steps
  (`fwd_router.cpp:1774`), and that allocates its `link_tables_t` and its egress entry from the
  `std::pmr::memory_resource` (`route_handle.cpp:34-42`, `:236-237`), which reports exhaustion by
  throwing — so that one leg can still abort under `-fno-exceptions`
  ([#603](https://github.com/avatarsd-llc/libtracer/issues/603)). A flow that is not
  compact-flagged never reaches it. See
  [failable allocation and backpressure](../design/allocation-and-backpressure.md).
- **Per-frame sinks are a function pointer plus a context, not `std::function`**
  ([ADR-0047 — build-time closed module sets and compile-time seams](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0047-build-time-closed-module-sets-compile-time-seams.md)).
  These fire on the per-frame receive path, where type-erasure machinery — code size, a heap
  capability, exception paths — is the largest avoidable embedded liability. It is the same call
  shape `graph_t`'s `subscriber_fn_t` makes at L4. Passing `nullptr` clears a sink.
- **A per-child block source is a pointer branch, not a lookup.** Each transport owns its receive
  thread, so a source parked on that child's receiver context is touched by exactly one thread —
  the per-thread shape obtained by ownership rather than by a lock — and a bounded node giving each
  child its own slab makes the bound per-peer, so one noisy link cannot starve another's decode
  ([ADR-0067 — bounded recycling source and per-owner topology](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0067-bounded-recycling-source-and-per-owner-topology.md) §3).

## The /net connection model

A connection is a **vertex**, not a hidden table entry
([ADR-0027 — transports and connections are vertices](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0027-transport-and-connections-are-vertices.md)),
so it is addressable, readable, subscribable and removable by the same operations as any other
vertex.

**Creation is an ordinary write.** A `SPEC` appended to the `/net` catalog field —
`write /net:children[] += SPEC{...}` — instantiates a connection. The SPEC's config carries a
`kind` selector naming a registered transport factory (`core/src/transport_vertex.cpp:53` documents
the config shape; `kind` is read at `:64`), plus the universal keys `addr`, `port`, `role`,
`keepalive`, `max_frame`, `backoff` and `connect_timeout`. Two catalog child types are registered
against the graph, `client` and `listener` (`core/src/transport_vertex.cpp:99,103`), which supply
the role default. Extra transport kinds join the catalog through `register_transport_type`
(`core/src/transport_vertex.cpp:128`) — that is how the QUIC module extends a node without this
file ever learning about it.

**The write is ACL-gated.** The `:children[]` append is gated on the parent vertex's `CREATE`
right and denied with `PERMISSION_DENIED` otherwise (`core/src/graph.cpp:1855-1857`). Under
[RFC-0014 — creator endpoint, connection lifecycle and link liveness](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
that gate relocates onto the creator endpoint's own ACL and gains its removal counterpart: a `NAME`
write is gated on `WRITE` — **not** `DELETE` — per
[RFC-0009 — vertex removal and subscriber eviction](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §A.2.

**Mount and routing are the same path.** A created connection lives at `/net/<module>/<name>` and
routes by exactly that path: the routing key *is* the mount path, so the registry's precomputed
NAME run is exactly the prefix a hop prepends to `src` and the forward path assembles nothing per
hop (`core/src/transport_vertex.cpp:223-230,240-247`;
[ADR-0061 — per-transport mount routing, strip-K L5 demux](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)).
The `/net/<module>` grouping vertex is created lazily on first use, with `graph_.find` itself as the
dedupe rather than a second source of truth (`core/src/transport_vertex.cpp:249-257`). Because a
connection is addressed under `/net/<module>/`, a first-level local vertex cannot shadow one.

**Module naming is declared-only, by the application**
([ADR-0073](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0073-naming-authority-the-application-mints-one-predicate-gates.md)
§4). There is no derived default and no library-side auto-registration: linking a built-in
transport registers no module name, and an undeclared `(kind, role)` pair fails creation with
`SCHEMA_NOT_FOUND` (`core/src/transport_vertex.cpp:167`). The application declares each module
under a name it chooses through `register_module` (`core/src/transport_vertex.cpp:133`,
`core/include/libtracer/transport_vertex.hpp:267`), a minting boundary gated by the shared
segment-validity predicate — a reserved-character name answers `INVALID_PATH`. The built-in
transports export *suggested*-name constants (`kWsClientSuggestedModule`, …) an application may
adopt; `/net` itself is likewise only the recommended root convention (a constructor default).

**Creation is all-or-nothing.** A connection is built in three steps — register the identity
vertex, insert the `conns_` entry, wire the link into the router's `child_registry_t` — and only
the last can be refused: `add_child` answers `false` when the registry cannot grow, and it is the
only place that can say so (`core/include/libtracer/fwd_router.hpp:245`,
`core/include/libtracer/child_registry.hpp:209`). A refusal unwinds the first two in reverse —
retire the vertex, then erase the entry, which destroys the config-constructed socket — publishes
no liveness, and answers `BACKPRESSURE` (`core/src/transport_vertex.cpp:347-350`). Discarding that
`bool` left a connection reporting `UP` that no `dst` resolved, no inbound frame reached, and
`remove_child` did not know about — a ghost a peer could mint by creating connections until the
registry slab exhausted. A `provide_link` staging is consumed only once the wiring has succeeded
(`core/src/transport_vertex.cpp:356`), so a retry after the pressure clears still finds its link.

**Liveness is the connection vertex's value.** `link_state_t` is six states —
`DORMANT`, `DIALING`, `RECONNECTING`, `UP`, `LISTENING`, `BIND_FAILED`
(`core/include/libtracer/transport_vertex.hpp:96-103`). `DIAL` links use the first four; `LISTEN`
links report listen-socket reachability with the last two, never a per-accepted-peer state. The
value is a 1-byte `VALUE` on the vertex, so it is `await`-able and subscribable: `subscribe
/net/<module>/<name>` streams every transition. The liveness *engine* that would drive these
automatically is not implemented — the value is set by the caller, and a config-constructed socket
reports `UP` or `LISTENING` at creation (`core/src/transport_vertex.cpp:370-372`).

**The accepted direction, and what is not realised.** RFC-0014 replaces the single global
`/net:children[]` catalog with a **per-module creator endpoint** at `/net/<module>/conn`, whose own
`:schema` is that module's config catalog. That endpoint is accepted and **is not implemented**; the
creation surface a peer can address is the `/net:children[]` catalog described above. One further
boundary is open by design: RFC-0014 delivers the *link*, while third-party multi-hop `SUBSCRIBER`
origination — making one node subscribe to another and then departing — is a separate unanswered
question, so an orchestrator can create the wires' **links** in band without being able to
originate the **wires** ([#491]).

### Connection settings are transport-private

`conn_settings_t` (`core/include/libtracer/transport_vertex.hpp:120`) and `conn_role_t` (`:77`) are
a **device-private `:settings` facet** of a connection vertex
([ADR-0021 — the colon-field plane is the vertex ioctl](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0021-colon-field-plane-is-the-vertex-ioctl.md)
draws the standard / device-private line). They live on the `tr::net` leaf record and are **never**
the L4 vertex `:settings` surface — conflating the two would put `addr`, `port` and `kind` into
the universal settings surface of every vertex in the graph. (That surface's core namespace is
now empty anyway — RFC-0022 §3.B — which makes the separation structural rather than merely
disciplined.)

The record carries only the keys **every** transport kind shares. A kind's private configuration —
a QUIC certificate and key path, for instance — never lands here; that kind's own factory parses it
out of the raw config `SETTINGS` TLV handed to it alongside these settings.

Both families — the universal keys and every kind's private ones — are tabulated key by key on
[connection config](connection-config.md).

## Pitfalls

- **`add_child` returning `false` is not advisory.** A mount name of **any** width registers and
  resolves ([#523](https://github.com/avatarsd-llc/libtracer/issues/523)) — the descent makes one
  registry pass and matches each slot against the prefix of that slot's own `seg_count` — so there
  is no width pitfall left. What `add_child` does refuse, always and not only in debug, is a name
  no address could ever name: empty, containing an empty segment, or wider than
  `graph::kMaxSegments`; it also refuses when the registry cannot grow. `false` means **nothing**
  was registered, so a caller that ignores it wires a link that is audible on its transport and
  resolvable by no `dst`, while `size()` and `live_size()` report the registry as healthy.
- **`remove_child` is removal; `link_down` is departure.** A link that merely dropped must take
  `link_down`, which evicts subscriber edges and label state but keeps the registry entry. Calling
  `remove_child` on a reconnecting `DIAL` link permanently unroutes it, because under the RFC-0014
  model a connection's *vertex* outlives its *socket*. Conversely, call `remove_child` **before**
  destroying the `transport_t`, or a forward can resolve a freed object.
- **A NAME owns one receiver context, for the router's life (#884).** `remove_child` tombstones
  the child's `child_rx_ctx_t` where it stands — it stays on the lock-free published chain,
  because a receive thread may be walking it, but it answers no lookup — and `add_child` of that
  same NAME revives it rather than appending a second. Two consequences callers rely on: a
  re-added child resolves to its *current* tenancy on the bound path (`connection_ref` /
  `hop_mint` re-resolve `conn_slot` per registration, so a child that gained a connection vertex
  between registrations becomes bindable, and one re-added as a bus mount stops being), and
  create/remove churn on a stable name set neither leaks a context nor lengthens the chain the
  bound hop walks. `receiver_ctx_count()` is the assertable form of the second, the twin of
  `child_registry_t::size()` — which has had this rule since #494/#521.
- **A sink's `ctx` must outlive every possible delivery.** Sinks fire on transport receive threads,
  possibly several concurrently, and clearing a sink is the only thing that stops future calls.
- **`materialize()` returns an owner, not a span.** Holding `reply.materialize().bytes()` past the
  end of the full expression reads freed memory; bind the `view_t` to a named object first.
- **Configure children and sinks before frames flow.** `add_child` and the sink setters are
  control-plane calls; the forward path deliberately takes no lock, so concurrent reconfiguration
  is not part of the contract.
- **A bus link's own connection NAME is not a routable next-hop.** A `dst` that names a
  multi-peer link's NAME with a residual below it — rather than naming one of its peers — is
  **rejected** with `tr::path::invalid` (`0x0021`), never forwarded
  ([RFC-0020](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0020-bus-name-not-a-routable-next-hop.md)).
  Forwarding it would reach the bus endpoint's `send()`, which **broadcasts**, so one directed
  request would draw one reply per peer and scramble any client that correlates replies FIFO.
  Naming the mount *exactly* still terminates locally, and a peer-directed hop
  (`/net/<module>/<name>/<peer>/…`) still forwards — only the broadcast shape is refused.
- **A stale `COMPACT` label is dropped, never fatal.** A label with no ingress binding on its link
  produces a `HANDLE_NACK` back to the producer, which re-advertises. An implementation that treats
  an unknown label as a protocol error breaks the self-heal.

## The rest of the plane

The router is the orchestrator, but most of the plane's surface is in the pieces
it delegates to — and each of those was extracted precisely so its rules could be
tested against hand-built frames with no live transport.

- **`op_resolver_t`** is the terminus half. When a `dst` names a vertex on *this*
  node, it applies the operation (READ / WRITE / AWAIT, plus any `:field`
  selector) against the graph and builds the reply as a rope: one exactly-sized
  head segment prepended to refcount-clones of the vertex's stored payload, never
  a serialize into a fresh buffer. It is local-only by construction — a `dst`
  that does not resolve locally is answered `NOT_FOUND`, and hop-by-hop
  forwarding stays the router's job.
- **`route_handle_t`** is the per-node label store behind delivery compaction.
  Read literally, "a delivery is a FWD WRITE" makes every streamed sample
  re-carry its full return route; a per-link label aliases that route instead,
  and each hop *swaps* the label the way a CAN ID is re-resolved against each
  bus. Binding is advertise-driven and re-advertise on reconnect is the
  self-heal. A flow that is not flagged for compaction allocates nothing here,
  which is what preserves the stateless-forwarder property for everything else.
- **The frame view** (`fwd_hdr_t`, `fwd_pre_t`, `dst_seg_walk_t`,
  `control_head_t`, `fwd_rebuild_t`, `stack_writer`) is the offset-dispatch
  cluster the forward hop reads a frame by: one header read as absolute offsets,
  the forward-versus-terminus peeks, the control-frame head peek, a fixed-capacity
  stack byte writer, and the shrunk-`dst` / grown-`src` head rebuild. Everything
  is templated over a cursor concept and yields **offsets, never spans**, so the
  same logic serves a contiguous frame and a link-walking rope and the caller
  re-slices from its own cursor.
- **The grammar** (`tr::wire::grammar`) is the shared TLV header/trailer core
  underneath all of that; it is documented with the codec on
  [frame-codec](frame-codec.md).

## API reference

```{doxygenclass} tr::net::fwd_router_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::child_registry_t
:project: libtracer
:members:
```

### Terminus resolution

```{doxygenclass} tr::graph::op_resolver_t
:project: libtracer
:members:
```

```{doxygenenum} tr::graph::fwd_op_t
:project: libtracer
```

```{doxygenenum} tr::graph::reply_kind_t
:project: libtracer
```

### Delivery compaction

```{doxygenclass} tr::net::route_handle_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::resolved_binding_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::handle_binding_t
:project: libtracer
:members:
```

### The FWD frame view

```{doxygenstruct} tr::net::fwd_hdr_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::fwd_pre_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::dst_seg_walk_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::control_head_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::fwd_rebuild_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::stack_writer
:project: libtracer
:members:
```

```{doxygenfunction} tr::net::read_fwd_header
:project: libtracer
```

```{doxygenenum} tr::net::fwd_dst_kind_t
:project: libtracer
```

```{doxygenfunction} tr::net::peek_fwd_dst_any
:project: libtracer
```

```{doxygenfunction} tr::net::peek_fwd_dst
:project: libtracer
```

```{doxygenfunction} tr::net::peek_fwd_dst_ref
:project: libtracer
```

```{doxygenfunction} tr::net::read_path_ref_element
:project: libtracer
```

```{doxygenstruct} tr::net::reply_mint_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::no_mint_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::net::peek_reply_mint
:project: libtracer
```

```{doxygenfunction} tr::net::rebuild_reply_mint
:project: libtracer
```

```{doxygenfunction} tr::net::peek_fwd_first_dst_seg
:project: libtracer
```

```{doxygenfunction} tr::net::peek_fwd_op
:project: libtracer
```

```{doxygenfunction} tr::net::peek_control
:project: libtracer
```

```{doxygenfunction} tr::net::rebuild_fwd_forward(const Cursor&, std::span<const std::byte>, std::string_view, std::size_t, const fwd_pre_t*, MintFn)
:project: libtracer
```

```{doxygenfunction} tr::net::rebuild_fwd_forward(const Cursor&, std::string_view)
:project: libtracer
```

```{doxygenfunction} tr::net::encode_mount_tlv
:project: libtracer
```

```{doxygenvariable} tr::net::kDstSegCacheSlots
:project: libtracer
```

```{doxygenvariable} tr::net::kFwdHead1Cap
:project: libtracer
```

```{doxygenvariable} tr::net::kFwdSrcHdrCap
:project: libtracer
```

```{doxygenvariable} tr::net::kFwdMaxIov
:project: libtracer
```

### The /net connection surface

```{doxygenclass} tr::net::transport_vertex_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::conn_settings_t
:project: libtracer
:members:
```

```{doxygenenum} tr::net::conn_role_t
:project: libtracer
```

```{doxygenstruct} tr::net::slim_net_t
:project: libtracer
:members:
```

## See also

- [transport — the wire seam](transport.md) — what `fwd_router_t` sends through.
- [graph — vertices & dispatch](graph.md) — the terminus the router resolves against.
- [interface map](interface-map.md) — the net plane in the cross-cutting view.
- [Two nodes over a wire](../examples/two-node-fwd.md) — a runnable two-node forward and reply.
- [Reference 13 — network formation](../reference/13-network-formation.md) and
  [Reference 04 — communication flows](../reference/04-communication-flows.md) — the same model
  described implementation-independently.
- [Failable allocation and backpressure](../design/allocation-and-backpressure.md) — the drop rule
  and the block-source seam.
- [Concurrency and scaling](../design/concurrency/README.md) — where this implementation's measured
  costs live, with their conditions.

[#491]: https://github.com/avatarsd-llc/libtracer/issues/491
