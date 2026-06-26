# transport — the wire seam (L4)

```{admonition} In one paragraph
:class: tip
**`transport_t`** is the seam between the bridge and one wire technology: `send`
framed bytes, register a `receiver_t` for inbound frames. It never sees TLV
semantics — only bytes. Today the only implementation is **`loopback_channel_t`**, an
in-process dev/test transport (two endpoints, an in-memory queue, a receive
thread). A real socket transport (UDS/UDP) is **M5** behind the same interface.
```

## What it does

A transport accepts a complete frame's bytes (a TLV, usually ROUTER-wrapped) and
emits them; inbound frames arrive on the registered receiver (which may fire on an
internal transport thread). The reference catalog defines a poll-based
`transport_vtable`; the C++ seam is callback + recv-thread — an implementation
choice that matches how a real socket's receive loop feeds the bridge.

`loopback_channel_t` wires two endpoints: a frame sent on one is delivered to the
*other's* receiver on that endpoint's thread (modeling async cross-"wire"
delivery, so a bridge cycle terminates by `hop_count` rather than stack recursion).
`shutdown()` joins the receive threads before the receivers are destroyed.

## Interface

```cpp
using peer_id_t = std::array<std::byte, 16>;       // ROUTER origin_peer_id

class transport_t {
    virtual void send(std::span<const std::byte> frame) = 0;
    using receiver_t = std::function<void(std::span<const std::byte>)>;
    virtual void set_receiver(receiver_t) = 0;
};

class loopback_channel_t {                          // dev/test transport
    loopback_endpoint_t& a();  loopback_endpoint_t& b();  // each a transport_t
    void shutdown();                                // join recv threads
};
```

## Two nodes over a wire

```{mermaid}
flowchart LR
    BA["bridge A"] -->|send| EA["endpoint a"]
    EA -->|enqueue| QB[("inbox B")]
    QB -->|recv thread| EB["endpoint b"]
    EB -->|receiver| BB["bridge B"]
    BB -->|send| EB2["endpoint b"]
    EB2 -->|enqueue| QA[("inbox A")]
    QA -->|recv thread| EA2["endpoint a"]
    EA2 -->|receiver| BA
    classDef m fill:#fef9c3,stroke:#92400e; class EA,EB,EA2,EB2 m;
```

## Benefits

- **One seam, many wires** — the bridge and the whole stack above are transport-
  agnostic; M5's socket transport plugs in with no changes upstream.
- **Deterministic testing** — the loopback exercises the full encode→ROUTER→decode
  path with no sockets, so bridge/dedup/cycle behavior is unit-testable (and the
  [benchmark](../../bench/README.md) measures it).
- **Bytes only** — a transport can't accidentally depend on graph semantics.

See: [router](router.md), [bridge](bridge.md).
