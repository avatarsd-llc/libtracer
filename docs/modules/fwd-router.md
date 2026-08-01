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
(`type_t::ROUTER`, `core/include/libtracer/tlv.hpp:40`) with no implemented mechanism behind it;
source routing needs none.

Three dispositions, decided by resolving the **first `dst` segment** against the registry:

| First `dst` segment resolves to | Disposition |
| --- | --- |
| a registered transport child | **forward** — strip the segment from `dst`, prepend this node's NAME for the inbound link to `src`, scatter-gather onward |
| a local non-transport vertex | **terminus** — decode into an arena, apply the op, build `FWD{REPLY}`, send it back over the link the request arrived on |
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
    explicit fwd_router_t(graph::graph_t& graph,
                          std::pmr::memory_resource* mr = std::pmr::get_default_resource(),
                          mem::block_source_t* rx = &mem::heap_source());

    // `name` is this node's NAME for `link`: the dst segment that routes onward through it,
    // and the segment prepended to src on the way back. 1..3 segments. Optional per-child
    // failable source; null falls back to the router's.
    void add_child(std::string name, transport_t& link, mem::block_source_t* rx = nullptr);
    bool remove_child(std::string_view name);   // removal, not departure
    void link_down(std::string_view link_name); // departure: evict edges + drop label state

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
    void add(std::string name, transport_t& link);        // rebinds an existing name
    bool erase(std::string_view name);                    // tombstones in place
    const child_t* by_segments(std::span<const std::string_view> segs) const;  // the demux
    transport_t*   by_name(std::string_view name) const;
    static transport_t* resolve_peer(const child_t&, std::string_view peer);
    std::size_t size() const noexcept;  std::size_t live_size() const noexcept;
};

}  // namespace tr::net
```

Signature source: `core/include/libtracer/fwd_router.hpp:91-93` (constructor), `:137`
(`add_child`), `:164-175` (the sink function-pointer types);
`core/include/libtracer/child_registry.hpp:193,227,242,286,327,337`.

## Routing one inbound frame

```{mermaid}
flowchart TB
    IN["inbound frame on child NAME"] --> RAW["on_raw observer"]
    RAW --> PEEK["offset peek: first dst segment"]
    PEEK --> DEMUX{"child_registry_t<br/>by_segments"}
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
  (`core/include/libtracer/fwd_router.hpp:186-190`). A sink that wants contiguous bytes holds
  `const view_t m = reply.materialize()` and reads `m.bytes()`; a **single-link reply — the common
  case — is returned zero-copy, no allocation and no copy**, and only a multi-link reply pays one
  flatten, on demand. The escape hatch sits at the consumer, so the router never pays for a
  consumer that did not need contiguity. `m` must stay alive while its span is read.
- **The default delivery leg copies nothing.** A full-route `FWD{WRITE}` fan-out scatter-gathers a
  fresh stack head, the stored return-route bytes, an empty `src`, and one span per link of the
  stored value (`core/src/fwd_router.cpp:1217-1172`). The `COMPACT` leg is the one that flattens,
  because a `COMPACT` wraps a contiguous payload (`core/src/fwd_router.cpp:1180`) — single-link, that
  flatten is a zero-copy adopt.
- **Delivery is nothrow and drops rather than aborts.** Every per-delivery allocation on the writer
  thread is failable: a failed flatten, frame build or `iovec` reserve **drops that one delivery**
  — a subscriber misses a value under exhaustion, which is valid delivery behaviour — instead of
  raising an exception that `-fno-exceptions` would turn into `abort()`. A dropped fresh
  `ADVERTISE` self-heals through the peer's `HANDLE_NACK`. See
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
right and denied with `PERMISSION_DENIED` otherwise (`core/src/graph.cpp:1564-1566`). Under
[RFC-0014 — creator endpoint, connection lifecycle and link liveness](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md)
that gate relocates onto the creator endpoint's own ACL and gains its removal counterpart: a `NAME`
write is gated on `WRITE` — **not** `DELETE` — per
[RFC-0009 — vertex removal and subscriber eviction](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md) §A.2.

**Mount and routing are the same path.** A created connection lives at `/net/<module>/<name>` and
routes by exactly that path: the routing key *is* the mount path, so the registry's precomputed
NAME run is exactly the prefix a hop prepends to `src` and the forward path assembles nothing per
hop (`core/src/transport_vertex.cpp:188-195,205-212`;
[ADR-0061 — per-transport mount routing, strip-K L5 demux](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0061-per-transport-mount-routing-strip-k-l5-demux.md)).
The `/net/<module>` grouping vertex is created lazily on first use, with `graph_.find` itself as the
dedupe rather than a second source of truth (`core/src/transport_vertex.cpp:214-222`). Because a
connection is addressed under `/net/<module>/`, a first-level local vertex cannot shadow one.

**Module naming has a default and an override.** An undeclared `(kind, role)` pair mounts under
`<kind>-client` for `DIAL` and `<kind>-server` for `LISTEN`
(`core/src/transport_vertex.cpp:150`), which is what makes an externally registered transport work
with no extra declaration. A transport whose shape that gets wrong declares itself explicitly
through `register_module` (`core/src/transport_vertex.cpp:133`,
`core/include/libtracer/transport_vertex.hpp:256`).

**Liveness is the connection vertex's value.** `link_state_t` is six states —
`DORMANT`, `DIALING`, `RECONNECTING`, `UP`, `LISTENING`, `BIND_FAILED`
(`core/include/libtracer/transport_vertex.hpp:96-103`). `DIAL` links use the first four; `LISTEN`
links report listen-socket reachability with the last two, never a per-accepted-peer state. The
value is a 1-byte `VALUE` on the vertex, so it is `await`-able and subscribable: `subscribe
/net/<module>/<name>` streams every transition. The liveness *engine* that would drive these
automatically is not implemented — the value is set by the caller, and a config-constructed socket
reports `UP` or `LISTENING` at creation (`core/src/transport_vertex.cpp:296-297`).

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
the L4 `settings_t` that every sensor vertex carries — conflating the two would put `addr`, `port`
and `kind` into the universal settings surface of every vertex in the graph.

The record carries only the keys **every** transport kind shares. A kind's private configuration —
a QUIC certificate and key path, for instance — never lands here; that kind's own factory parses it
out of the raw config `SETTINGS` TLV handed to it alongside these settings.

## Pitfalls

- **A child NAME of four or more segments registers a child that resolves for nothing.** The mount
  descent matches key widths derived from the `net / <module> / <name> / <peer>` grammar, so a
  4-segment name silently misroutes: every forward to it misses and falls through to the terminus,
  while `size()` and `live_size()` report it as healthy. Debug builds assert; release behaviour is
  unchanged. The bound is derived from the addressing grammar, not chosen, so widening it is an
  addressing-model change.
- **`remove_child` is removal; `link_down` is departure.** A link that merely dropped must take
  `link_down`, which evicts subscriber edges and label state but keeps the registry entry. Calling
  `remove_child` on a reconnecting `DIAL` link permanently unroutes it, because under the RFC-0014
  model a connection's *vertex* outlives its *socket*. Conversely, call `remove_child` **before**
  destroying the `transport_t`, or a forward can resolve a freed object.
- **A sink's `ctx` must outlive every possible delivery.** Sinks fire on transport receive threads,
  possibly several concurrently, and clearing a sink is the only thing that stops future calls.
- **`materialize()` returns an owner, not a span.** Holding `reply.materialize().bytes()` past the
  end of the full expression reads freed memory; bind the `view_t` to a named object first.
- **Configure children and sinks before frames flow.** `add_child` and the sink setters are
  control-plane calls; the forward path deliberately takes no lock, so concurrent reconfiguration
  is not part of the contract.
- **A stale `COMPACT` label is dropped, never fatal.** A label with no ingress binding on its link
  produces a `HANDLE_NACK` back to the producer, which re-advertises. An implementation that treats
  an unknown label as a protocol error breaks the self-heal.

## API reference

```{doxygenclass} tr::net::fwd_router_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::child_registry_t
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
