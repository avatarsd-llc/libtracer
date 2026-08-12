# transport — the wire seam (L4)

```{admonition} In one paragraph
:class: tip
**`transport_t`** is the seam between the routing plane and one wire technology:
`send` framed bytes (a single buffer **or** a scatter-gather `iovec`), install a
sink for inbound frames. It never sees TLV semantics — only bytes. Implementations:
**`loopback_channel_t`** (in-process dev/test), **`udp_transport_t`**
(localhost/LAN UDP), **`tcp_transport_t`** / **`transport_tcp_server`** (reliable
TCP stream, 4-byte u32-LE length-prefix framing — the prefix is transport framing,
not part of the TLV), **`transport_ws_client`** / **`transport_ws_server`** (the
browser↔robot WebSocket keystone, RFC 6455), **`transport_can`** (SocketCAN,
classic + CAN-FD), **`quic_transport_t`** and **`webtransport_transport_t`** (the
separate `libtracer_quic` module, msquic-backed).
```

## The seam

A transport accepts a complete frame's bytes — a `FWD` frame, or a route-handle
control frame (ADVERTISE / COMPACT / HANDLE_NACK) — and emits them; inbound frames
arrive on the installed sink, which may fire on an internal transport thread. Framing
below the TLV is the transport's own business: a datagram kind needs none, a stream
kind adds a `u32-LE length ++ frame` record (`core/include/libtracer/transport_tcp.hpp`),
a CAN kind fragments and reassembles.

The reference catalog describes the same shape — a small callback-based seam, one
frame in, one frame out ([10-module-catalog.md](../reference/10-module-catalog.md)
§Transport ↔ L4). What is an implementation choice rather than a described property
is the *concurrency*: each socket transport owns a receive thread and calls the sink
from it, matching how a real socket's receive loop feeds the router. The seam's
shape is declared implementation-defined ([ADR-0013 — v1 scope
boundaries](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0013-v1-scope-boundaries.md));
two conforming nodes share the wire format, not this class.

Routing above the seam — which link a frame leaves by, how a return route is grown,
how a link is mounted under `/net` — belongs to the FWD router, described at
[fwd-router](fwd-router.md). This page stops at the byte boundary.

## Two delivery tiers

The sink comes in two forms and a transport declares which one it honors.

| Tier | Installed by | Frame lifetime | Declared by |
| --- | --- | --- | --- |
| Borrowed span | `set_receiver(fn, ctx)` | valid only for the callback | the default |
| Owning rope | `set_rope_receiver(fn, ctx)` | refcounted; may be kept, subroped, forwarded | `delivers_ropes()` returns `true` |

A transport that can hand up *owning* frames implements the rope-receiver seam
([ADR-0042 — refcounted receiver seam,
view delivery](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0042-refcounted-receiver-seam-view-delivery.md),
generalized to ropes by [ADR-0053 — lazy rope-backed decode, view partial-path
routing](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md)):
it overrides `delivers_ropes()` (`core/include/libtracer/transport.hpp:458`) and
delivers each inbound frame as a `rope_t` of refcounted links over segments drawn
from a host-injected `mem_backend_t`. A contiguous frame is the single-link case; a
scattered one — a CAN reassembly group, a fragmented WebSocket message — crosses the
seam as the rope it already is, never a flatten copy. Ownership is the whole point:
the borrowed span dies when the callback returns, so a receiver that must outlive
the callback needs this tier.

There is deliberately no adapter that wraps a borrowed span into a rope; such a rope's
refcounts would lie about lifetime. `fwd_router_t::add_child` (`core/src/fwd_router.cpp:646`)
therefore branches on the link's declared capability and installs exactly one sink —
the rope form for an owning link, the span form otherwise (`fwd_router.cpp:753,690`, and
`fwd_router.cpp:707,714` for the peer-named bus equivalent).

Every socket transport in the tree declares the owning tier: UDP
(`transport_udp.hpp:111`), TCP client and server (`transport_tcp.hpp:207,372`),
WebSocket server and client (`transport_ws.hpp:225,416`), CAN
(`transport_can.hpp:510`), QUIC (`transport_quic.hpp:153`) and WebTransport
(`transport_webtransport.hpp:158`). The borrowed-span path is the base-class default
and the tier an out-of-tree transport gets for free.

## Point-to-point links and bus links

A point-to-point link carries one peer, so the child NAME the router registers it
under fully addresses the far side. A **bus** link reaches many peers over one wire
and exposes them through the optional `bus_link_t` facet
(`core/include/libtracer/transport.hpp:66`, [ADR-0044 — stateless transport peer
enumeration](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0044-stateless-transport-peer-enumeration-separate-paths-client-side-identity.md)):
`enumerate_peers` synthesizes the currently-audible names from the wire's own live
traffic, `peer_link` resolves one name to a directed sending endpoint, and
`set_peer_receiver` / `set_peer_rope_receiver` replace the flat sink with one that
tags each inbound frame with the sending peer's name. No vertex is created for a
peer and no peer state is stored.

`transport_t::bus()` returns the facet or `nullptr`. CAN always returns it
(`transport_can.hpp:491`); the TCP and WebSocket **servers** return it when
configured peer-named — one implementation, on the slot-server base both of them
inherit (`posix_endpoint.hpp:409`); every other kind keeps the `nullptr` default.

Whether a link's peer-named tier exists is one query, `bus_link_t::peer_named()`
(`transport.hpp:137`): the constructed flag for the two stream servers
(`posix_endpoint.hpp:419`), `true` by construction for a kind that is a bus outright.
`bus_link_t` **refuses** each of its peer-named wiring calls — `set_peer_receiver`,
`set_peer_rope_receiver`, `set_peer_down_notifier` — while it is false. That refusal matters
because `bus_link_t` is a public base: on a flat server the setters are reachable by an
explicit upcast past the null `bus()`, and admitting one used to flip the link into
peer-named delivery the null `bus()` had denied.

For the two stream servers, whose mode is a wiring-time choice, the same flag is the whole
answer: `bus()`, the per-frame tier select and the departure seam all read it, so a
**peer-named** server delivers only on the peer tier (an unwired one drops rather than
handing a many-peer link's frame up untagged) and a **flat** one only on the flat tier. A
kind that is a bus outright keeps its own delivery precedence — CAN still falls back to the
flat sink for a single-peer consumer that wired no bus facet, which the gate does not
disturb because `peer_named()` is true there.

Departure follows the same split. A **peer-named** server evicts exactly the departed peer
(`notify_peer_down(name)`); a **flat** server has one routing identity for every peer it
carries — the registered child NAME — so its only seam is the whole link
(`transport_t::notify_down`), and it therefore waits until the **last** open session departs
(`posix_endpoint.cpp:499`). Firing it on a mid-life close would evict the surviving peers'
edges along with the departed one's.

## QUIC and WebTransport

Both live in the separate `libtracer_quic` target, configured by
`LIBTRACER_WITH_QUIC` (`core/CMakeLists.txt:312`, default `OFF` because msquic must
be installed). Core itself contains no `#ifdef` and no msquic reference: the module
extends the transport catalog through `register_transport_type`, registering
`quic_transport_factory()` under kind `quic` and `webtransport_transport_factory()`
under kind `webtransport`.

- `quic_transport_t` — TLS 1.3, connection migration, one bidirectional stream
  carrying the same length-prefix framing as TCP. The hosted, secure link; the MCU
  class keeps UDP and CAN ([ADR-0043 — QUIC/WebTransport optional module,
  msquic](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0043-quic-webtransport-optional-module-msquic.md)
  Phase A).
- `webtransport_transport_t` — WebTransport over HTTP/3, the browser-reachable form
  of QUIC: a module-private minimal H3/QPACK handshake layer (`core/src/wt_h3.hpp`,
  never installed) in front of one WebTransport bidirectional stream carrying the
  same framing (ADR-0043 Phase B). The browser side is the TypeScript
  `transport-webtransport` package ([ADR-0031 — direct browser-to-robot binding and
  WebTransport](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0031-direct-browser-to-robot-binding-and-webtransport.md)).

One msquic dependency serves both, because QUIC is the substrate WebTransport
requires.

Both kinds read four kind-private config keys off a `:children[]` creation SPEC —
`cert`/`key` on the LISTEN side and the DIAL-side trust pair `ca`/`insecure`. A
SPEC-created dialer **verifies its peer's certificate by default**, so reaching a
self-signed peer takes one of those two keys explicitly; the key-by-key reference is
[connection config](connection-config.md).

## Interface

```cpp
using peer_id_t = std::array<std::byte, 16>;        // the node identity

class transport_t {
    virtual void send(std::span<const std::byte> frame) = 0;
    // Scatter-gather: ship a rope's to_iovec() as one frame, no flatten copy.
    // The default gathers into a temporary; native transports override
    // (sendmsg/writev/RDMA SGE).
    virtual void send(std::span<const std::span<const std::byte>> iov);

    // Two inbound sinks, {fn, ctx} — no type erasure. ctx must outlive delivery.
    using receiver_fn_t      = receiver_slot_t<>::span_fn_t;  // borrowed span
    using rope_receiver_fn_t = receiver_slot_t<>::rope_fn_t;  // owning rope
    void set_receiver(receiver_fn_t fn, void* ctx) noexcept;
    void set_rope_receiver(rope_receiver_fn_t fn, void* ctx) noexcept;
    template <typename F> void set_receiver(F& sink) noexcept;       // lvalue only
    template <typename F> void set_rope_receiver(F& sink) noexcept;  // lvalue only

    virtual bool delivers_ropes() const;                // false by default
    using down_fn_t = void (*)(void* ctx);
    void set_down_notifier(down_fn_t fn, void* ctx) noexcept;
    virtual bus_link_t* bus();                          // nullptr = point-to-point
};

class loopback_channel_t {                          // dev/test transport
    loopback_endpoint_t& a();  loopback_endpoint_t& b();  // each a transport_t
    void shutdown();                                // join recv threads
};

class udp_transport_t : public transport_t {
    udp_transport_t(std::uint16_t bind_port, const std::string& peer_host,
                    std::uint16_t peer_port,
                    mem::mem_backend_t* backend = &mem::heap_backend(),
                    std::size_t max_frame = 0, std::size_t recv_stack = 0);
    // send(span) = one sendto; send(iov) = one sendmsg(iovec) — a composite rope
    // in one syscall. Datagrams land in segments from `backend`; exhaustion drops
    // the datagram and ticks dropped_rx. `max_frame` (the universal :settings key,
    // 0 = kMaxDatagram) is the largest datagram accepted — a longer one is refused
    // and ticks malformed_rx instead of being delivered, while the backend can
    // furnish max_frame + 1 bytes (below that it truncates first, #1074).
};
```

`loopback_channel_t` wires two endpoints: a frame sent on one is delivered to the
*other's* receiver, on that endpoint's receive thread, modeling asynchronous
cross-wire delivery. `shutdown()` joins both receive threads before the registered
receivers are destroyed.

## The forward hop that feeds a transport

What the router hands `send(iov)` on a forward hop is not a re-encoded frame: the
hop reads a few headers of the inbound frame by offset, builds the shortened headers
in small stack buffers, and scatter-gathers those heads with untouched views of the
inbound frame. The hop costs **0 heap allocations / 0 bytes**, measured by replacing
the global `operator new`/`delete` with a counting wrapper around exactly one hop
(`bench_forward_heap`, single-threaded, `ZEROHEAP_MAX=0` enforced in `perf.yml`;
[ADR-0038 — net-plane performance
model](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)).

```{mermaid}
flowchart LR
    IN["inbound frame bytes"] --> PEEK["offset peek:<br/>first dst NAME"]
    PEEK --> DEMUX["child_registry_t<br/>NAME → transport"]
    DEMUX --> SG["stack-built heads<br/>+ untouched frame views"]
    SG -->|"send(iov) — one syscall"| T(["transport_t"])
```

## Two nodes over a wire

```{mermaid}
flowchart LR
    FA["fwd_router A"] -->|send| EA["endpoint a"]
    EA -->|enqueue| QB[("inbox B")]
    QB -->|recv thread| EB["endpoint b"]
    EB -->|receiver| FB["fwd_router B"]
    FB -->|FWD REPLY send| EB2["endpoint b"]
    EB2 -->|enqueue| QA[("inbox A")]
    QA -->|recv thread| EA2["endpoint a"]
    EA2 -->|receiver| FA
    classDef m fill:#fef9c3,stroke:#92400e
    class EA,EB,EA2,EB2 m
```

## Consequences

- **One seam, many wires** — the router and the whole stack above it are
  transport-agnostic; a new socket transport plugs in with no change upstream.
- **Deterministic testing** — the loopback exercises the full
  encode → FWD-route → decode path with no sockets, so forward and reply behavior is
  unit-testable and benchable.
- **Bytes only** — a transport cannot accidentally depend on graph semantics,
  because no graph type crosses the seam.
- **Capability, not configuration** — the delivery tier is a property the transport
  declares, so a link that cannot honor owning delivery can never be asked to.

## Pitfalls

- **A borrowed span dies at the callback's return.** Storing the span, or a `view_t`
  built over it, hands the router a dangling window on the next receive. A receiver
  that keeps the frame installs the rope sink and requires `delivers_ropes()`.
- **`ctx` must outlive every possible delivery.** The receive thread is already live
  when the connection opens, so a sink whose context is a local — or whose context
  is destroyed before `shutdown()` — races a frame already in flight. Both sinks and
  both notifiers must be installed before frames flow.
- **"Before frames flow" is not free on a DIAL link.** A transport that connects and
  spawns its receive thread in one constructor leaves no such window: the peer's push
  is provoked by our own connect, so its first message can be decoded before the owner's
  next statement runs, and an empty sink drops it with no counter moving
  ([#1025](https://github.com/avatarsd-llc/libtracer/issues/1025)). `start_receiving()`
  is the second phase that opens the window — a no-op default, so an owner calls it
  unconditionally as its last wiring step; `transport_ws_client` and `tcp_transport_t`
  honor it when constructed with `defer_recv`
  ([#1045](https://github.com/avatarsd-llc/libtracer/issues/1045)), which is how
  `transport_vertex_t` builds a SPEC-created `ws` or `tcp` dialer. The ESP-IDF-native WS
  client honors it too, in its own shape: its recv thread does the dialling, so
  `defer_recv` holds the **first dial** behind the `start_receiving()` latch (ADR-0081's
  defer-the-dial arm, [#1102](https://github.com/avatarsd-llc/libtracer/issues/1102)) —
  one-shot, since reconnects happen only on an already-armed link — and the embedder
  passes the flag itself, there being no `ws` factory on a chip target. `quic` and
  `webtransport` still take the no-op default;
  whether the window is reachable on each has its own follow-up (#1100–#1101). `udp` has
  no DIAL constructor of this shape — it binds an ephemeral port, never `::connect`s and
  sends nothing in the constructor, so no peer can learn its source port to push to.
- **On a bus the window needs no provocation at all.** `can` has no dial to defer and no
  peer flow-control window to hold bytes in, and its RX callback cannot be withheld
  without starving the liveness bookkeeping it drives (`last_heard`, the
  pending/reassembly sweeps). Any bystander traffic already on the wire lands in the
  window, so the answer is [ADR-0081](../adr/0081-pre-sink-ingress-native-window-hold-or-named-drop-never-parked.md)
  §4's other arm — drop, and tick `transport_can::dropped_presink()`
  ([#1103](https://github.com/avatarsd-llc/libtracer/issues/1103)).
- **The callable sugar binds by address.** `set_receiver(F& sink)` and
  `set_rope_receiver(F& sink)` take an lvalue; a temporary lambda does not compile,
  and a callable destroyed early dangles exactly like a stale `ctx`.
- **Overriding `send(iov)` is not optional for a scatter-gather wire.** The base
  implementation gathers into a temporary buffer and, when that allocation fails,
  **drops the frame** rather than aborting (`transport.hpp:341`). A transport with a
  native `sendmsg`/`writev` that does not override it silently pays a copy per
  forward hop and inherits a drop path it did not intend.
- **The link-down notifier is a routing seam, not a log hook.** It re-enters the
  routing plane to evict the departed link's subscriber edges, so it must be fired
  with no internal transport locks held; a connectionless kind simply never fires it.

## Shared scaffolding

Three pieces are shared by every transport rather than reimplemented in each, and
they are the reason a new binding is small.

- **`receiver_slot_t`** is the one home of the delivery-tier mechanism: the
  guarded storage for a receiver pair, the per-frame snapshot, and the
  owning-rope-versus-borrowed-span tier select. Its callbacks are plain
  `{function pointer, context}` pairs, so taking a snapshot on the receive path
  is a trivial copy under an uncontended lock rather than a heap allocation. Its
  `Tag...` pack is how a bus link prepends the sending peer's name to both sinks.
- **`posix_endpoint_t`** is the receive-thread scaffold every POSIX socket
  transport shares: the stop flag, the thread lifecycle, and the bounded
  poll-and-recheck idioms that let a blocking socket loop notice a clean
  shutdown. `stream_endpoint_t` adds what only the *stream* transports need — the
  peer-fd atomic, the write mutex, and the teardown-under-write-lock ordering
  that keeps a concurrent send from writing to a reused descriptor. UDP keeps its
  datagram shape and uses only the base. `slot_server_t` is one tier further up,
  for the MULTI-peer stream servers: it owns the slot vector, the accept/poll/
  teardown machinery and the `bus_link_t` query trio, so `transport_tcp_server`
  and `transport_ws_server` differ only in their framing and handshake — the two
  hooks it dispatches into them.
- **`register_builtin_transports`** is how a node's transport catalog gets
  populated. Each `register_*_transport` lives in its own translation unit,
  compiled only when that transport is enabled, so a build that drops a transport
  leaves neither a compiled factory nor a dangling call to it. No preprocessor
  macro selects a transport: selection is which translation units get compiled.

## API reference

### The seams

```{doxygenclass} tr::net::transport_t
:project: libtracer
:members:
:protected-members:
```

```{doxygenclass} tr::net::bus_link_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::receiver_slot_t
:project: libtracer
:members:
```

```{doxygenenum} tr::net::link_state_t
:project: libtracer
```

```{doxygentypedef} tr::net::peer_id_t
:project: libtracer
```

### The POSIX scaffold

```{doxygenclass} tr::net::posix_endpoint_t
:project: libtracer
:members:
:protected-members:
```

```{doxygenclass} tr::net::stream_endpoint_t
:project: libtracer
:members:
:protected-members:
```

```{doxygenclass} tr::net::slot_server_t
:project: libtracer
:members:
:protected-members:
```

### Datagram and stream transports

```{doxygenclass} tr::net::udp_transport_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::tcp_transport_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::transport_tcp_server
:project: libtracer
:members:
```

### WebSocket

```{doxygenclass} tr::net::transport_ws_client
:project: libtracer
:members:
```

```{doxygenclass} tr::net::transport_ws_server
:project: libtracer
:members:
```

The WebSocket wire layer itself — opcodes, frame decode, the accept-key
computation — is pure and lives in `tr::net::ws`:

```{doxygenenum} tr::net::ws::opcode_t
:project: libtracer
```

```{doxygenstruct} tr::net::ws::frame_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::net::ws::decode_frame
:project: libtracer
```

```{doxygenfunction} tr::net::ws::decode_frame_checked
:project: libtracer
```

```{doxygenvariable} tr::net::ws::kMaxControlPayload
:project: libtracer
```

```{doxygenvariable} tr::net::ws::kNoPayloadCap
:project: libtracer
```

```{doxygenfunction} tr::net::ws::encode_frame
:project: libtracer
```

```{doxygenfunction} tr::net::ws::encode_frame_header
:project: libtracer
```

```{doxygenfunction} tr::net::ws::encode_server_control
:project: libtracer
```

```{doxygenfunction} tr::net::ws::encode_client_control
:project: libtracer
```

```{doxygenfunction} tr::net::ws::encode_client_frame
:project: libtracer
```

```{doxygenfunction} tr::net::ws::try_encode_client_frame
:project: libtracer
```

```{doxygenfunction} tr::net::ws::accept_key
:project: libtracer
```

### QUIC and WebTransport (the optional module)

```{doxygenclass} tr::net::quic_transport_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::quic_dial_tls_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::webtransport_transport_t
:project: libtracer
:members:
```

```{doxygenstruct} tr::net::webtransport_dial_tls_t
:project: libtracer
:members:
```

```{doxygenfunction} tr::net::quic_transport_factory
:project: libtracer
```

```{doxygenfunction} tr::net::webtransport_transport_factory
:project: libtracer
```

### In-process loopback (development and test)

```{doxygenclass} tr::net::loopback_channel_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::loopback_endpoint_t
:project: libtracer
:members:
```

### Catalog registration

```{doxygenfunction} tr::net::register_builtin_transports
:project: libtracer
```

```{doxygenfunction} tr::net::register_udp_transport
:project: libtracer
```

```{doxygenfunction} tr::net::register_tcp_transport
:project: libtracer
```

```{doxygenfunction} tr::net::register_ws_transport
:project: libtracer
```

CAN is a stack of its own — the ID codec, the advertise stream, the splitter and
the reassembler as well as the binding — and has [its own page](can.md).

See: [can](can.md), [fwd-router](fwd-router.md), [interface map](interface-map.md),
[reference §communication flows](../reference/04-communication-flows.md),
[bench suite](https://github.com/avatarsd-llc/libtracer/blob/main/bench/README.md).
