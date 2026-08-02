# fwd/fwd-src-accumulated

FWD{ op=READ, dst=/net/uplink/d/sensor/temp, src=/net/downlink/a/net/downlink/cli/reply-ep } — the **accumulated `src`**: the same op seen mid-route, after two forwarders have each prepended their inbound link's FULL mount run.

Where `fwd/fwd-routed-two-mount` carries the *origin* frame of a multi-mount route, this one carries a frame *in flight*, two hops in. It is the vector for the `src` half of the strip-K invariant: `dst` shrinks by whole `net/<module>/<name>` runs, `src` grows by whole `net/<module>/<name>` runs, in reverse-route order.

## The topology this frame is in flight on

The same mount-key discipline as `fwd/fwd-routed-two-mount`, one hop longer — the frame below is the one **B puts on the wire toward C**:

```
client ──▶ A ──(net/uplink/b)──▶ B ──(net/uplink/c)──▶ C ──(net/uplink/d)──▶ D
                                      │                                     /sensor/temp
                                      └── this vector: the frame B sends to C
```

Each node's mount for its *inbound* link is the reverse-direction key: A's is `net/downlink/cli`, B's is `net/downlink/a`, C's is `net/downlink/b`.

| Hop | Node | Registry match | `strip_k` | `dst` after | `src` after |
| --- | --- | --- | --- | --- | --- |
| origin | client | — | — | `/net/uplink/b/net/uplink/c/net/uplink/d/sensor/temp` | `/reply-ep` |
| 1 | A | `net/uplink/b` | 3 | `/net/uplink/c/net/uplink/d/sensor/temp` | `/net/downlink/cli/reply-ep` |
| 2 | B | `net/uplink/c` | 3 | **`/net/uplink/d/sensor/temp`** | **`/net/downlink/a/net/downlink/cli/reply-ep`** |
| 3 | C | `net/uplink/d` | 3 | `/sensor/temp` | `/net/downlink/b/net/downlink/a/net/downlink/cli/reply-ep` |
| — | D | no prefix of `sensor/temp` is registered | — | (terminates) | — |

Two hops of accumulation therefore mean **two mount runs, six segments** of `src` growth — not two segments. The forwarder prepends the *qualified* key composed by `transport_vertex.cpp` (`net/<module>/<name>`), never a bare connection NAME, which is exactly what keeps the accumulated reverse route resolvable through the same strip-K descent on the way home (the ADR-0061 erratum). The most recent hop's run sits at the **front**, so reading `src` left to right walks the route back toward the originator.

## Byte breakdown

`0x0F FWD`, `opt.PL=1`, `length=0x0073` (115), 119 bytes total.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | `0F 40 73 00` | FWD, PL=1, body length 115 |
| 4 | `01 00 01 00 00` | VALUE u8 `0x00` — `op = READ` |
| 9 | `06 40 28 00` | PATH `dst`, body length 40 |
| 13 | `02 00 03 00 "net"` | dst[0] — C's mount, segment 1 |
| 20 | `02 00 06 00 "uplink"` | dst[1] — C's mount, segment 2 (module) |
| 30 | `02 00 01 00 "d"` | dst[2] — C's mount, segment 3 (connection NAME) |
| 35 | `02 00 06 00 "sensor"` | dst[3] — residual, resolved locally at D |
| 45 | `02 00 04 00 "temp"` | dst[4] — residual, resolved locally at D |
| 53 | `06 40 3E 00` | PATH `src`, body length 62 |
| 57 | `02 00 03 00 "net"` | src[0] — B's inbound mount run, segment 1 |
| 64 | `02 00 08 00 "downlink"` | src[1] — B's inbound mount run, segment 2 (module) |
| 76 | `02 00 01 00 "a"` | src[2] — B's inbound mount run, segment 3 (NAME) |
| 81 | `02 00 03 00 "net"` | src[3] — A's inbound mount run, segment 1 |
| 88 | `02 00 08 00 "downlink"` | src[4] — A's inbound mount run, segment 2 (module) |
| 100 | `02 00 03 00 "cli"` | src[5] — A's inbound mount run, segment 3 (NAME) |
| 107 | `02 00 08 00 "reply-ep"` | src[6] — the originator's reply endpoint |

## What this vector does and does not gate

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only — the harness contract is `encode(decode(input.bin)) == input.bin`, and a static fixture routes nothing. The hop-by-hop accumulation above is a *behavioural* claim, and it is bound where it can be false: `core/tests/fwd_two_mount_test.cpp` drives the production `fwd_router_t` / `transport_vertex_t` wiring and asserts byte-exactly that after two hops `src = /net/downlink/a/net/downlink/cli/reply-ep` — the identical two-run shape this vector's `src` carries.

## Provenance — the bytes CHANGED on 2026-08-02

This is the only conformance vector whose bytes have ever changed. It previously read
`FWD{ op=READ, dst=/can0/ow/sensor, src=/via_board/via_net/reply-ep }` (77 bytes, `0f4049…`),
described as "the multihop op after two hops: dst shrunk by two, src grown by two".

Those bytes are **pre-S2a and do not compose**. Under the shipped per-module mount routing
(RFC-0014 §S2a / ADR-0061) a forwarder consumes and prepends a whole three-segment
`net/<module>/<name>` run, so a `src` grown by two hops can never be two bare segments
(`via_board`, `via_net`) — and neither of those names is a mount key at all. The old frame
described a routing model the implementation has never had.

Changed under maintainer ruling (b) on [#419](https://github.com/avatarsd-llc/libtracer/issues/419),
2026-08-02, on the DRAFT protocol (`v1.md`: adding a vector is free, changing existing bytes is a
spec change — this one was authorised explicitly). The vector keeps its name: it still shows
exactly what it always claimed to show, `src` accumulating mid-route.

```
0f407300010001000006402800020003006e65740200060075706c696e6b02000100640200060073656e736f720200040074656d7006403e00020003006e657402000800646f776e6c696e6b0200010061020003006e657402000800646f776e6c696e6b02000300636c69020008007265706c792d6570
```
