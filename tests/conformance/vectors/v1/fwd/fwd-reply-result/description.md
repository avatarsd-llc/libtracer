# fwd/fwd-reply-result

FWD{ op=REPLY, dst=/net/downlink/a/net/downlink/cli/reply-ep, src=<pair 3:1>, kind=RESULT, payload=VALUE u32=1234 } — the **successful reply exactly as the terminus emits it**, source-routed home over the accumulated return route, its `src` seeded with the PAIR of the vertex that served it ([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) §6.2).

## The point this frame depicts

The terminus C of `fwd/fwd-routed-two-mount`'s route, the instant it has served the request and put the reply on the wire back toward B — **before any forwarder has consumed a mount run from it**. `fwd/fwd-reply-error` depicts the *same* point of the *same* route; the two vectors differ only in `kind` and payload.

```
client ◀── A ◀──(net/downlink/cli)── B ◀──(net/downlink/a)── C  [/sensor/temp]
                                                             │
                                                             └── this vector: the frame C emits
```

The request that reaches C is the one `fwd/fwd-routed-two-mount` originates:
`dst=/sensor/temp`, `src=/net/downlink/a/net/downlink/cli/reply-ep` — the `src` two forwarders have grown by their FULL `net/<module>/<name>` inbound mount runs (RFC-0014 §S2a / ADR-0061).

The resolver assembles the reply's `dst` from the request's `src` verbatim, and seeds its `src` with
the PAIR of the vertex it served (RFC-0029 §6.2 — the target's slot and generation, here `3:1`):

| | request, as it arrives at C | reply, as C emits it |
| --- | --- | --- |
| `dst` | `/sensor/temp` | `/net/downlink/a/net/downlink/cli/reply-ep` |
| `src` | `/net/downlink/a/net/downlink/cli/reply-ep` | `<pair 3:1>` |

So this vector's `dst` is byte-identical to `fwd/fwd-src-accumulated`'s `src`. That identity is the whole point: the accumulated `src` a forward hop builds **is** the reply's routable `dst`. The `src` is the start of the PAIR chain the origin learns: each relaying hop prepends its own element to it (`fwd/reply-src-accumulates-forward-route-pair`, `-name`).

## How that `dst` gets home

The terminus's own first hop is implicit — C answers over the link the request arrived on, it does not route on `dst`. Every hop after that is the ordinary strip-K descent, in reverse:

| Hop | Node | Registry match | `strip_k` | `dst` after | `src` after |
| --- | --- | --- | --- | --- | --- |
| — | C emits | (answers over the inbound link) | — | **`/net/downlink/a/net/downlink/cli/reply-ep`** | **`<pair 3:1>`** |
| 1 | B | `net/downlink/a` (B's link to A) | 3 | `/net/downlink/cli/reply-ep` | `<B's pair for C><pair 3:1>` |
| 2 | A | `net/downlink/cli` (A's link to the client) | 3 | `/reply-ep` | `<A's pair for B><B's pair for C><pair 3:1>` |
| — | client | no prefix of `reply-ep` is registered | — | (terminates — the REPLY sink) | — |

A REPLY routes through the *same* `route_fwd_forward` a request does: `dst` shrinks by whole mount runs and `src` grows by whole mount runs, op-agnostically. Two consequences are visible above — the reply's `dst` is fully consumed to `/reply-ep` at the originator, and its `src` arrives as a PAIR chain spelling the outbound route back out again (a hop that holds no PAIR contributes its NAME run instead).

## Byte breakdown

`0x0F FWD`, `opt.PL=1`, `length=0x004E` (78), 82 bytes total. The two `PATH`s carry packed
`[u8 len][utf8]` segment records ([RFC-0018](../../../../../docs/spec/rfcs/0018-packed-path-segments.md)),
so each is `opt=0x00` and each segment costs `1 + len` rather than `4 + len`; a PAIR element is the
`00 16 08` escape followed by its `u32` index and `u32` generation (RFC-0029 §4.1).

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | `0F 40 4E 00` | FWD, PL=1, body length 78 |
| 4 | `01 00 01 00 03` | VALUE u8 `0x03` — `op = REPLY` |
| 9 | `06 00 29 00` | PATH `dst`, PL=0, body length 41 |
| 13 | `03 "net"` | dst[0] — B's mount run for its link to A, segment 1 |
| 17 | `08 "downlink"` | dst[1] — segment 2 (module) |
| 26 | `01 "a"` | dst[2] — segment 3 (connection name) |
| 28 | `03 "net"` | dst[3] — A's mount run for its link to the client, segment 1 |
| 32 | `08 "downlink"` | dst[4] — segment 2 (module) |
| 41 | `03 "cli"` | dst[5] — segment 3 (connection name) |
| 45 | `08 "reply-ep"` | dst[6] — the originator's reply endpoint |
| 54 | `06 00 0B 00` | PATH `src`, PL=0, body length 11 |
| 58 | `00 16 08 03 00 00 00 01 00 00 00` | src[0] — PAIR 3:1, the served vertex at C |
| 69 | `01 00 01 00 00` | VALUE u8 `0x00` — `kind = RESULT` |
| 74 | `01 00 04 00 D2 04 00 00` | VALUE u32 LE `1234` — the served value |

## What this vector does and does not gate

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only — the harness contract is `encode(decode(input.bin)) == input.bin`, and a static fixture routes nothing. The route swap and the homeward descent above are *behavioural* claims, bound where they can be false: `core/tests/fwd_two_mount_test.cpp` drives the production `fwd_router_t` / `transport_vertex_t` wiring, asserts byte-exactly that the request reaching C carries `src=/net/downlink/a/net/downlink/cli/reply-ep` (this vector's `dst`), and asserts that the REPLY reaches the client with `dst` fully consumed to `/reply-ep`. The whole frame, seeded `src` included, is pinned byte-exactly against the production router by `core/tests/path_pair_test.cpp`'s `learning_vectors()`.

## Provenance — the bytes CHANGED on 2026-10-23

The `src` was the request's `dst`, `/sensor/temp` (83 bytes, `0F404F…`). Under RFC-0029 §6.2 the
terminus seeds a success reply's `src` with the PAIR of the vertex it served, so the origin learns
a PAIR chain with no flag asking for one ([#2245](https://github.com/avatarsd-llc/libtracer/issues/2245));
RFC-0029 §13.4 lists the `fwd/fwd-reply-*` vectors as keeping their `dst` and gaining that `src`.
The bytes are pinned against the production router by `core/tests/path_pair_test.cpp`'s
`learning_vectors()`.

## Provenance — the bytes CHANGED on 2026-08-02

This vector previously read
`FWD{ op=REPLY, dst=/via_board/via_net/reply-ep, src=/sensor, kind=RESULT, payload=VALUE u32=1234 }`
(76 bytes, `0f4048…`), described as "source-routed back via the accumulated route".

Those bytes are **pre-S2a and do not compose**. Under the shipped per-module mount routing
(RFC-0014 §S2a / ADR-0061) an accumulated return route is built out of whole three-segment
`net/<module>/<name>` runs, so it can never be the two bare names `via_board`/`via_net` —
neither of which is a mount key at all, and neither of which any node could match. The old
frame described a routing model the implementation has never had.

Changed under maintainer ruling (c) on [#419](https://github.com/avatarsd-llc/libtracer/issues/419),
2026-08-02, on the DRAFT protocol (`v1.md`: adding a vector is free, changing existing bytes is a
spec change — this one was authorised explicitly). Ruling (b) had already corrected the third
route-bearing vector, `fwd/fwd-src-accumulated`; ruling (c) applies the identical fix to both
reply vectors, so no conformance vector now carries the pre-S2a form. The `kind` and the
payload are untouched — only the route was wrong.

```
0f406a00010001000306403e00020003006e657402000800646f776e6c696e6b0200010061020003006e657402000800646f776e6c696e6b02000300636c69020008007265706c792d6570064012000200060073656e736f720200040074656d70010001000001000400d2040000
```
