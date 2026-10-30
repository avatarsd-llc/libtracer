# Stream links

## In one paragraph

A stream link carries frames over a reliable, ordered byte stream, which has no frame
boundaries of its own. So each frame gets one: TCP and QUIC put a 4-byte little-endian length
in front of it, and WebSocket sends it as one binary message. The receiver reads the boundary,
draws one segment of exactly the frame's size from the link's own memory, and delivers it. A
stream link is the one to use for frames larger than a datagram, for a browser, or wherever
loss is not acceptable. A TCP or WebSocket server can carry many peers and name each one.

## The picture

```{mermaid}
:caption: "Legend: green = vertex, blue = frame, grey dashed = not the library, vermillion = peer."
flowchart LR
    accTitle: A frame on a stream link
    accDescr: The stream link vertex prefixes the frame with its length or a WebSocket header and writes both to the socket in one scatter-gather call. The peer reads the boundary and receives the frame in one segment of its exact size.
    L("/net/tcp/gw"):::vertex -->|"length or WS header, then the frame"| S[TCP or QUIC stream]:::notlib
    S ==>|bytes| R[[peer stream link]]:::peer
    R -->|"one exactly-sized segment"| F[/"the frame"/]:::frame
```

## When you need it

- Frames above one MTU: up to 16 MiB by default, where a datagram link stops at 1,472 bytes.
- A browser peer: WebSocket, or WebTransport over QUIC.
- A gateway serving many clients: the TCP and WebSocket servers, peer-named.
- Encryption with certificates on a host: QUIC, which is TLS 1.3.

On a lossy LAN where only the newest value matters, a datagram link avoids head-of-line waits;
see [datagram links](datagram.md).

## How it works

**The boundary.**

| Kind | Boundary per frame | Notes |
| --- | --- | --- |
| `tcp_transport_t`, `tcp_server_transport_t` | `u32` LE length | the prefix is transport framing, not part of the TLV |
| `quic_transport_t` | `u32` LE length on one bidirectional stream | optional `libtracer_quic` module |
| `webtransport_transport_t` | `u32` LE length on one WebTransport stream | the browser-reachable QUIC |
| `ws_server_transport_t`, `ws_client_transport_t` | one RFC 6455 binary message | the client masks its frames |

**Send.** On TCP and the WebSocket server, the prefix or header goes out as the first entry of
a scatter-gather list and the frame as the rest, so the frame is never flattened. A WebSocket
client masks, which needs a copy, drawn from the link's egress store. QUIC and WebTransport copy
the frame once, because msquic holds the send buffer until the send completes.

**Receive.** One shared framer (`length_prefix_framer_t`, and its WebSocket counterpart) reads
the boundary, checks it against the link's cap, and copies the body into one segment of exactly
that size. Two failures get two answers. A frame the node cannot store right now is drained and
counted, and the connection lives. A length beyond the agreed cap means the stream has lost its
framing, so the connection is closed. The cap, `max_frame`, can only be tightened.

**Many peers.** A server configured peer-named is a bus: each session is a peer named from the
wire, and the departure of one evicts only that one. A flat server has one name for all of its
sessions and reports the link down only when the last one closes.

## Cost

### Wire bytes

Per frame, on top of TCP's or QUIC's own headers:

| frame bytes | TCP, QUIC | WebSocket, server to client | WebSocket, client to server |
| ---: | ---: | ---: | ---: |
| 64 B | 4 B | 2 B | 6 B |
| 1 KiB | 4 B | 4 B | 8 B |
| 4 KiB | 4 B | 4 B | 8 B |
| 16 KiB | 4 B | 4 B | 8 B |

Above 64 KiB the WebSocket header is 10 bytes (14 masked). The overhead stays a few bytes at
every size; the kernel's segmentation is not the library's.

### Allocations

None per send on TCP and the WebSocket server for a frame of up to 16 scatter-gather entries; a
larger gather spills once, about 288 bytes (measured by `bench_transport_iov`). One copy per send
on QUIC, WebTransport and the WebSocket client. One exactly-sized segment per received frame,
drawn from the link's receive memory, so a peer that sends big frames spends the receiving
link's budget and no one else's.

### Latency

Not benched on this page. `bench_tcp_baseline`, `bench_transports` and `bench_tcp_fanin` measure
TCP; their figures are on the generated Performance page. No bench yet isolates WebSocket or
QUIC; a WebSocket arm in `bench_transports` would.

### Flash and RAM per target

| Target | Flash | RAM per link (host) |
| --- | --- | --- |
| NARROW | not measured | not measured |
| MID | not measured | not measured |
| WIDE | not measured | TCP server 600 B plus 290 B per connection; WebSocket server 600 B plus 434 B per connection |

The WIDE figures are the `bench_conn_ram` pins. A receive also holds one frame-sized segment
while a frame is being read, up to `max_frame`. A per-transport row in `tools/esp_size_gate.py`
would measure flash on MID.

## What the library does not do

- It does not split a frame across streams or reorder frames. One stream per link, in order.
- It buffers nothing for a slow reader beyond the link's send queue, which drops and counts
  when full.
- It does not read certificates from a creation write. TLS material is the application's,
  handed over at registration.

## Pitfalls

- **Symptom:** the first message after a dial is lost. **Cause:** the sinks were installed
  after receiving began. **Fix:** install them, then call `start_receiving()`.
- **Symptom:** a peer is disconnected for one large frame. **Cause:** its length exceeds the
  link's `max_frame`, which ends framing sync. **Fix:** agree the cap on both ends; a frame
  merely too large to store is shed, not disconnected.
- **Symptom:** one slow consumer delays everyone's newest value. **Cause:** a byte stream
  delivers in order, so a lost segment holds up what follows. **Fix:** use QUIC, or a datagram
  link for values where only the newest matters.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope) leaves a transport's framing below the
TLV to its binding; the frames a stream link carries are
[v1 §3](../../spec/v1.md#3-wire-format).

**Exercised by:** `tcp_test`, `length_prefix_framer_test`, `ws_test`, `ws_transport_test`,
`ws_rx_bound_test`, `quic_test`, `webtransport_test`.

Next concept: [datagram links](datagram.md). Go deeper: the
[transport module page](../../modules/transport.md), then
[reference 16](../../reference/16-websocket-session-auth.md) for WebSocket sessions, then
[v1 §1](../../spec/v1.md#1-scope).
