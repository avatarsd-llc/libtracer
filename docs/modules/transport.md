# transport — the wire seam (L4)

```{admonition} In one paragraph
:class: tip
**`transport_t`** is the seam between the FWD router and one wire technology: `send`
framed bytes (single buffer **or** a scatter-gather `iovec`), register a
`receiver_t` for inbound frames. It never sees TLV semantics — only bytes.
Implementations: **`loopback_channel_t`** (in-process dev/test),
**`udp_transport_t`** (localhost/LAN UDP), **`tcp_transport_t`** (reliable TCP
stream, 4-byte u32-LE length-prefix framing — the prefix is transport framing,
not part of the TLV), **`transport_ws_client` / `transport_ws_server`** (the
browser↔robot WebSocket keystone, RFC 6455), **`transport_can`** (SocketCAN,
classic + CAN-FD), **`quic_transport_t`** (the separate `libtracer_quic`
module, msquic — TLS 1.3, connection migration; one bidirectional stream
carrying the same length-prefix framing as TCP; a host that talks QUIC links
the module and registers `quic_transport_factory()`; ADR-0043 Phase A — the
hosted, secure link; the MCU class keeps UDP/CAN), and
**`webtransport_transport_t`** (same module, ADR-0043 Phase B — WebTransport
over HTTP/3, the browser-reachable form of QUIC: a minimal module-private
H3/QPACK handshake layer in front of one WebTransport bidirectional stream
carrying the same length-prefix framing; kind `webtransport` via
`webtransport_transport_factory()`; the TS twin is
`@avatarsd-llc/libtracer-webtransport` — the #92 / ADR-0031 browser↔robot
path).
```

## What it does

A transport accepts a complete frame's bytes (a `FWD` frame, or a route-handle
control frame — ADVERTISE/COMPACT/HANDLE_NACK) and
emits them; inbound frames arrive on the registered receiver (which may fire on an
internal transport thread). The reference catalog defines a poll-based
`transport_vtable`; the C++ seam is callback + recv-thread — an implementation
choice that matches how a real socket's receive loop feeds the FWD router.

A transport that can hand up *owning* frames additionally implements the
**rope-receiver seam** ([ADR-0042](../adr/0042-refcounted-receiver-seam-view-delivery.md),
generalized to ropes by [ADR-0053](../adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md)): it overrides
`delivers_ropes()` and delivers each inbound frame as a `rope_t` of refcounted links — a
contiguous frame is the single-link case (e.g. `tcp_transport_t` reads a frame straight into
one segment); a scattered one (a CAN reassembly group, a fragmented WS message) crosses the
seam as the rope it already is, never a flatten copy. `fwd_router_t::add_child` installs
whichever receiver matches the link's capability; every other transport keeps the
borrowed-span receiver.

`loopback_channel_t` wires two endpoints: a frame sent on one is delivered to the
*other's* receiver on that endpoint's thread (modeling async cross-"wire" delivery).
`shutdown()` joins the receive threads before the receivers are destroyed.

## Interface

```cpp
using peer_id_t = std::array<std::byte, 16>;       // the node identity

class transport_t {
    virtual void send(std::span<const std::byte> frame) = 0;
    // Scatter-gather: ship a rope's to_iovec() as one frame, no flatten copy.
    // Default gathers+send(); native transports override (sendmsg/writev/RDMA-SGE).
    virtual void send(std::span<const std::span<const std::byte>> iov);
    using receiver_t = std::function<void(std::span<const std::byte>)>;
    virtual void set_receiver(receiver_t) = 0;
};

class loopback_channel_t {                          // dev/test transport
    loopback_endpoint_t& a();  loopback_endpoint_t& b();  // each a transport_t
    void shutdown();                                // join recv threads
};

class udp_transport_t : public transport_t {        // real UDP (M5)
    udp_transport_t(uint16_t bind_port, const std::string& peer_host, uint16_t peer_port);
    // send(span) = one sendto; send(iov) = one sendmsg(iovec) — the structural
    // batch (a composite rope in one syscall; see Performance).
};
```

## The forward hop that feeds a transport (zero-heap)

What the FWD router hands `send(iov)` on a forward hop is not a re-encoded frame:
the hop reads a few headers of the inbound frame by offset, builds the shortened
headers in small stack buffers, and scatter-gathers those heads with untouched
views of the inbound frame — **zero heap allocations**, CI-gated
(`bench_forward_heap`, `ZEROHEAP_MAX=0`).

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

## Benefits

- **One seam, many wires** — the FWD router and the whole stack above are transport-
  agnostic; a new socket transport plugs in with no changes upstream.
- **Deterministic testing** — the loopback exercises the full encode→FWD-route→decode
  path with no sockets, so forward/reply behavior is unit-testable (and the
  [benchmark](../../bench/README.md) measures it).
- **Bytes only** — a transport can't accidentally depend on graph semantics.

## API reference

Generated from `core/include/libtracer/transport.hpp` by Doxygen.

```{doxygenclass} tr::net::transport_t
:project: libtracer
:members:
```

```{doxygenclass} tr::net::bus_link_t
:project: libtracer
:members:
```

See: [interface-map](interface-map.md), [reference §communication-flows](../reference/04-communication-flows.md).
