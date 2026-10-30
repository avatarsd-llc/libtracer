# Datagram links

## In one paragraph

A datagram link sends each Tracer frame as exactly one UDP datagram. The datagram is the
boundary, so nothing is added in front of the frame, and nothing is reassembled: a frame either
fits one datagram or is refused by the receiver. A lost datagram is a lost frame, and the next
value replaces it, which is what a sensor stream on a LAN or an MCU on lwIP usually wants. Under
the Noise binding the same link encrypts each frame into one transport message, for 25 bytes of
overhead, after a two-message handshake.

## The picture

```{mermaid}
:caption: "Legend: green = vertex, blue = frame, grey dashed = not the library, vermillion = peer."
flowchart LR
    accTitle: A frame on a datagram link
    accDescr: The datagram link vertex sends the frame as one datagram through the UDP socket in one scatter-gather call. The peer's datagram link receives the datagram into one segment and delivers it as the frame.
    L("/net/udp/lan"):::vertex -->|"the frame, as one datagram"| S[UDP socket]:::notlib
    S ==>|UDP| R[[peer datagram link]]:::peer
    R -->|"one segment"| F[/"the frame"/]:::frame
```

## When you need it

- Low-latency values on a LAN, where a retransmitted old value is worse than a lost one.
- An MCU on lwIP, where a datagram link has no per-peer state and no stream buffers.
- An encrypted link without certificates: Noise with a pre-shared key, on the same socket.

For frames above one MTU, or where loss is not acceptable, use a
[stream link](stream.md).

## How it works

**One frame, one datagram.** `udp_transport_t::send` hands the frame to `sendmsg` as the
scatter-gather list it already is; a datagram the socket refuses is dropped and counted on
`dropped_tx`. The receiver's cap, `max_frame`, defaults to 1,472 bytes, one Ethernet MTU of
payload; a longer datagram is refused and counted on `malformed_rx`. The cap can be raised up
to 65,536 bytes, but a datagram above the path MTU then relies on IP fragmentation, which lwIP
does not reassemble by default.

**Receive.** The link reads each datagram into one segment from its receive memory, sized to the
smaller of its receive budget and one byte more than `max_frame`, so an oversized datagram is
seen as oversized and dropped rather than truncated. A datagram is delivered as one frame or not
at all.

**Under Noise.** The link runs the handshake of
[reference 23](../../reference/23-noise-link-binding.md) once per peer, then sends each frame as
one transport message: a type byte, a nonce, the ciphertext and a tag. Several frames are never
packed into one message, and a frame is never split.

## Cost

### Wire bytes

| frame bytes | UDP | UDP under Noise |
| ---: | ---: | ---: |
| 64 B | 0 B | 25 B |
| 1 KiB | 0 B | 25 B |
| 4 KiB | 0 B, past the default cap | 25 B, past the default cap |
| 16 KiB | 0 B, past the default cap | 25 B, past the default cap |

Above 1,472 bytes a frame needs a raised `max_frame` on the receiver and IP fragmentation on the
way; the library's overhead stays 0 or 25 bytes. The Noise handshake is one 58-byte and one
50-byte datagram per peer.

### Allocations

None per send: `sendmsg` takes a stack scatter-gather list. A gather of more than 16 entries
spills once, about 288 bytes (measured by `bench_transport_iov`). One segment per received
datagram, drawn from the link's receive memory.

### Latency

Not benched on this page. `bench_transports` and the UDP arm of `bench_compose_net` measure the
link; their figures are on the generated Performance page. No bench isolates the Noise
encryption; a Noise arm in `bench_transports` would.

### Flash and RAM per target

| Target | Flash | RAM per link |
| --- | --- | --- |
| NARROW | not measured | not measured |
| MID | not measured | not measured |
| WIDE | not measured | 216 B, nothing per peer |

The WIDE figure is the `bench_conn_ram` pin; its rope-heap arm, which counts the receive
segment, is 1,752 B. A per-transport row in `tools/esp_size_gate.py` would measure flash on
MID.

## What the library does not do

- It does not fragment, reassemble, retransmit or reorder. A lost datagram is a lost frame.
- It keeps no stream state and no vertex per peer.
- It does not raise `max_frame` on its own to fit a large frame, and the sender does not check
  the receiver's cap.

## Pitfalls

- **Symptom:** large values never arrive and the receiver's `malformed_rx` rises. **Cause:**
  the frame exceeds its `max_frame`, 1,472 bytes by default. **Fix:** send smaller values, or
  use a stream link.
- **Symptom:** after raising `max_frame`, large frames vanish on an MCU. **Cause:** the
  datagram was fragmented, and lwIP does not reassemble by default. **Fix:** keep frames
  within the path MTU.
- **Symptom:** Noise datagrams are dropped and `malformed_rx` rises. **Cause:** the peers
  disagree on the pre-shared key or the binding version. **Fix:** provision both ends from one
  source.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope) leaves plain UDP framing to its binding;
under Noise the datagram layout is
[reference 23 §3](../../reference/23-noise-link-binding.md#3-datagram-framing-and-the-size-bound).
The frames it carries are [v1 §3](../../spec/v1.md#3-wire-format).

**Exercised by:** `udp_test`, `noise_link_test`, `security_noise_test`.

Next concept: back to [transports](../transports.md). Go deeper:
[reference 23](../../reference/23-noise-link-binding.md), then the
[transport module page](../../modules/transport.md), then [v1 §1](../../spec/v1.md#1-scope).
