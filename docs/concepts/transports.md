# Transports

## In one paragraph

A transport moves whole Tracer frames between two nodes over one wire technology, and it is a
vertex in the graph like any other. Every link a node holds sits at `/net/<module>/<name>`:
created by a write, addressed by path, gated by the same ACL, retired by the same retire. Below
the frame, each transport frames bytes its own way, and the protocol leaves that framing to the
transport. libtracer ships three families: CAN, which elides the TLV header into the bus ID;
stream links (TCP, WebSocket, QUIC), which add a length or message boundary; and datagram links
(UDP, optionally under Noise), which send one frame per datagram. The router above never sees
the difference: it hands a transport a frame and gets frames back.

```{toctree}
:hidden:
:maxdepth: 1

CAN links <transports/can>
Stream links <transports/stream>
Datagram links <transports/datagram>
```

## The picture

```{mermaid}
:caption: "Legend: green = vertex, grey dashed = not the library, vermillion = peer."
flowchart LR
    accTitle: Three transport families under /net
    accDescr: A vertex's frame is routed to one of three transport vertices under /net. The CAN link sends header-elided slices to the CAN driver, the stream link sends a length prefix and the frame to a socket, and the datagram link sends one datagram per frame. Each reaches a peer node.
    V("vertex /sensor"):::vertex -->|FWD frame| C("/net/can/can0"):::vertex
    V -->|FWD frame| T("/net/tcp/gw"):::vertex
    V -->|FWD frame| U("/net/udp/lan"):::vertex
    C -->|"header-elided slices"| K[CAN driver]:::notlib
    T -->|"u32 length, frame"| S[TCP socket]:::notlib
    U -->|"one datagram per frame"| D[UDP socket]:::notlib
    K ==>|CAN bus| P1[[peer node]]:::peer
    S ==>|TCP| P2[[peer node]]:::peer
    D ==>|UDP| P2
```

## When you need it

Every node that talks to another node needs one. Pick the family by the wire you have and the
frames you send:

| You have | And you send | Read |
| --- | --- | --- |
| a CAN or CAN-FD bus | small values at a fixed priority | [CAN links](transports/can.md) |
| a reliable byte stream, a browser, or frames above one MTU | anything up to the link's cap | [Stream links](transports/stream.md) |
| a LAN or an MCU on lwIP, frames that fit one datagram | low-latency values, loss tolerated | [Datagram links](transports/datagram.md) |

## How it works

**The seam.** `tr::net::transport_t` takes a complete frame, as one buffer or a scatter-gather
list, and emits it; inbound frames arrive on a sink the router installs. It sees bytes, never
TLV meaning. A transport declares one of two sink tiers: a borrowed span, valid only for the
callback, or an owning rope the router may keep and forward without a copy. Every socket and
CAN transport in the tree delivers owning ropes. The details are on the
[transport module page](../modules/transport.md#two-delivery-tiers).

**A transport is a vertex.** Creating, addressing, gating and retiring a link are ordinary
graph operations, and its liveness is a value a caller can read or await. What that buys and
costs, and the lean rule that keeps a kind's private settings out of the shared record, are in
[reference 19](../reference/19-transports-are-vertices.md).

**Point-to-point and bus.** A point-to-point link carries one peer, so its child name addresses
the far side. A bus link reaches many peers over one wire and names each one from the wire's own
traffic, with no vertex and no stored state per peer. CAN is always a bus; the TCP and WebSocket
servers are one when configured peer-named.

**Framing below the frame** is the family's own:

| Family | Adds to each frame | Splits a frame? |
| --- | --- | --- |
| CAN | an advertise manifest on the control ID; the TLV path rides in the bus ID | yes, into 8 or 64-byte slices |
| stream | a 4-byte length (TCP, QUIC) or a WebSocket header of 2 to 14 bytes | no; the byte stream carries it whole |
| datagram | nothing, or 25 bytes under Noise | never; a frame fits one datagram or is dropped |

## Cost

Each child page carries its own numbers. In summary:

### Wire bytes

From 0 bytes per frame (UDP) through 4 (TCP, QUIC) and 2 to 14 (WebSocket) to 25 (Noise). CAN
spends whole bus frames: three or more classic frames of manifest per send, then one data frame
per 8 or 64 bytes.

### Allocations

A send on the UDP, TCP and WebSocket server links goes out as the scatter-gather list the frame
already is; QUIC, WebTransport and the masking WebSocket client copy it once. A receive draws
one segment per inbound frame (per CAN group) from the link's own memory seam, so a peer's
traffic is paid for by the link that receives it.

### Latency

Not benched on this page. `bench_transports` and `bench_tcp_baseline` measure the socket links
and `bench_can_reassembly` the CAN receive path; their figures are on the generated Performance
page.

### Flash and RAM per target

| Target | Flash | RAM per link (measured on a host) |
| --- | --- | --- |
| NARROW | not measured | not measured |
| MID | not measured | not measured |
| WIDE | not measured | UDP 216 B; TCP server 600 B plus 290 B per connection; WebSocket server 600 B plus 434 B per connection |

The WIDE figures are the `bench_conn_ram` pins in `bench/ram_census_pins.json`. The size gates
measure the core without transports, so flash is not measured anywhere; a per-transport row in
`tools/esp_size_gate.py` would measure it on MID, and in `tools/cortexm0_footprint.py` on NARROW.

## What the library does not do

- It does not specify a transport's framing on the wire. v1 leaves that to each binding, and
  only the Noise binding is normative.
- It does not keep state per peer on a bus, or a vertex per peer.
- It does not queue without bound for a slow link. A full send queue drops and counts.
- It runs no timer for liveness. A frozen peer is closed by a check the application drives with
  its own `now`.

## Pitfalls

- **Symptom:** a frame kept from a receive callback reads garbage later. **Cause:** the
  link delivers borrowed spans. **Fix:** keep frames only from a link whose `delivers_ropes()`
  is true, on the rope sink.
- **Symptom:** the first message after a dial is lost and no counter moves. **Cause:** the
  sinks were installed after receiving began. **Fix:** install every sink, then call
  `start_receiving()` last.
- **Symptom:** a custom transport is slower than its wire. **Cause:** it did not override the
  scatter-gather `send`, so the base class gathers into a temporary copy. **Fix:** override it
  with the wire's own `sendmsg` or `writev`.

## Where the rules live

**Normative source:** [v1 §1](../spec/v1.md#1-scope) leaves a transport's framing below the
TLV to its own binding, except the Noise binding,
[reference 23](../reference/23-noise-link-binding.md); the frames a transport carries are
[v1 §3](../spec/v1.md#3-wire-format).

**Exercised by:** `transport_conformance_test`, `transport_vertex_test`,
`transport_alloc_softfail_test`.

Next concept: [CAN links](transports/can.md), then [stream links](transports/stream.md) and
[datagram links](transports/datagram.md). Go deeper:
[reference 19](../reference/19-transports-are-vertices.md), then the
[transport module page](../modules/transport.md), then [v1 §1](../spec/v1.md#1-scope).
