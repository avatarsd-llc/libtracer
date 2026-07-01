# transport — the wire seam (L4)

```{admonition} In one paragraph
:class: tip
**`transport_t`** is the seam between the FWD router and one wire technology: `send`
framed bytes (single buffer **or** a scatter-gather `iovec`), register a
`receiver_t` for inbound frames. It never sees TLV semantics — only bytes.
Implementations: **`loopback_channel_t`** (in-process dev/test),
**`udp_transport_t`** (localhost/LAN UDP), **`transport_ws_client` /
`transport_ws_server`** (the browser↔robot WebSocket keystone, RFC 6455), and
**`transport_can`** (SocketCAN, classic + CAN-FD). A reliable byte-stream
transport (TCP/QUIC) remains future work.
```

## What it does

A transport accepts a complete frame's bytes (a TLV, usually ROUTER-wrapped) and
emits them; inbound frames arrive on the registered receiver (which may fire on an
internal transport thread). The reference catalog defines a poll-based
`transport_vtable`; the C++ seam is callback + recv-thread — an implementation
choice that matches how a real socket's receive loop feeds the FWD router.

`loopback_channel_t` wires two endpoints: a frame sent on one is delivered to the
*other's* receiver on that endpoint's thread (modeling async cross-"wire" delivery).
`shutdown()` joins the receive threads before the receivers are destroyed.

## Interface

```cpp
using peer_id_t = std::array<std::byte, 16>;       // ROUTER origin_peer_id

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
    classDef m fill:#fef9c3,stroke:#92400e; class EA,EB,EA2,EB2 m;
```

## Benefits

- **One seam, many wires** — the FWD router and the whole stack above are transport-
  agnostic; a new socket transport plugs in with no changes upstream.
- **Deterministic testing** — the loopback exercises the full encode→FWD-route→decode
  path with no sockets, so forward/reply behavior is unit-testable (and the
  [benchmark](../../bench/README.md) measures it).
- **Bytes only** — a transport can't accidentally depend on graph semantics.

See: [interface-map](interface-map.md), [reference §communication-flows](../reference/04-communication-flows.md).
