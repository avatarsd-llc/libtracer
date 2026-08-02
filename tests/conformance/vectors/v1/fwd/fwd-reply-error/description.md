# fwd/fwd-reply-error

FWD{ op=REPLY, dst=/net/downlink/a/net/downlink/cli/reply-ep, src=/sensor/temp, kind=ERROR, STATUS{ ERROR{ VALUE u16=0x0020 tr::path::not_found } } } — the **failing reply exactly as the terminus emits it**, on the identical route its RESULT twin carries.

## The point this frame depicts

The same point as [`fwd/fwd-reply-result`](../fwd-reply-result/description.md): the terminus C of `fwd/fwd-routed-two-mount`'s route, the instant it has put the reply on the wire back toward B — **before any forwarder has consumed a mount run from it**. The two vectors are deliberately the same frame at the same place, differing only in `kind` and payload, so a reader can diff the success and failure shapes with the route held constant.

The one difference in the depicted world: here C has **no** `/sensor/temp` vertex, so the resolver answers `tr::path::not_found` instead of serving a value.

```
client ◀── A ◀──(net/downlink/cli)── B ◀──(net/downlink/a)── C  [no /sensor/temp]
                                                             │
                                                             └── this vector: the frame C emits
```

The error reply's routes are the request's routes swapped verbatim, exactly as the RESULT side's
are (`op_resolve_walk.hpp`'s `assemble_error` calls the same `assemble`):

| | request, as it arrives at C | reply, as C emits it |
| --- | --- | --- |
| `dst` | `/sensor/temp` | `/net/downlink/a/net/downlink/cli/reply-ep` |
| `src` | `/net/downlink/a/net/downlink/cli/reply-ep` | `/sensor/temp` |

The reply `src` is therefore the **refused spelling** — the request `dst` the terminus could not resolve — which is what lets the originator tell *which* of several outstanding addresses failed. `fwd_router.cpp` names it that way where it builds the bus-NAME rejection reply on the same swap: "reply src = request dst (the refused spelling — what the peer asked for, echoed so it can correlate)".

The homeward descent is identical to the RESULT vector's: B matches `net/downlink/a` (strip-K = 3), A matches `net/downlink/cli` (strip-K = 3), and the frame reaches the client with `dst` fully consumed to `/reply-ep`.

## The ERROR identity

The ERROR is structured (`opt.PL=1`) per RFC-0002 §C: its first child is a VALUE carrying the u16 LE registered code `0x0020` (`tr::path::not_found`).

## Byte breakdown

`0x0F FWD`, `opt.PL=1`, `length=0x0070` (112), 116 bytes total.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | `0F 40 70 00` | FWD, PL=1, body length 112 |
| 4 | `01 00 01 00 03` | VALUE u8 `0x03` — `op = REPLY` |
| 9 | `06 40 3E 00` | PATH `dst`, body length 62 |
| 13 | `02 00 03 00 "net"` | dst[0] — B's mount run for its link to A, segment 1 |
| 20 | `02 00 08 00 "downlink"` | dst[1] — segment 2 (module) |
| 32 | `02 00 01 00 "a"` | dst[2] — segment 3 (connection NAME) |
| 37 | `02 00 03 00 "net"` | dst[3] — A's mount run for its link to the client, segment 1 |
| 44 | `02 00 08 00 "downlink"` | dst[4] — segment 2 (module) |
| 56 | `02 00 03 00 "cli"` | dst[5] — segment 3 (connection NAME) |
| 63 | `02 00 08 00 "reply-ep"` | dst[6] — the originator's reply endpoint |
| 75 | `06 40 12 00` | PATH `src`, body length 18 |
| 79 | `02 00 06 00 "sensor"` | src[0] — the refused spelling, segment 1 |
| 89 | `02 00 04 00 "temp"` | src[1] — the refused spelling, segment 2 |
| 97 | `01 00 01 00 01` | VALUE u8 `0x01` — `kind = ERROR` |
| 102 | `09 40 0A 00` | STATUS, PL=1, body length 10 |
| 106 | `08 40 06 00` | ERROR, PL=1, body length 6 |
| 110 | `01 00 02 00 20 00` | VALUE u16 LE `0x0020` — `tr::path::not_found` |

## What this vector does and does not gate

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only — the harness contract is `encode(decode(input.bin)) == input.bin`, and a static fixture routes nothing. The route swap is a *behavioural* claim, bound in `core/tests/fwd_two_mount_test.cpp` (which pins the request `src` this vector's `dst` mirrors, and the fully-consumed `dst` at the originator) and in `core/tests/op_resolve_test.cpp`, which drives the `assemble_error` path directly.

## Provenance — the bytes CHANGED on 2026-08-02

This vector previously read
`FWD{ op=REPLY, dst=/via_board/via_net/reply-ep, src=/sensor, kind=ERROR, STATUS{ ERROR{ VALUE u16=0x0020 } } }`
(82 bytes, `0f404e…`).

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
STATUS/ERROR identity are untouched — only the route was wrong.

```
0f407000010001000306403e00020003006e657402000800646f776e6c696e6b0200010061020003006e657402000800646f776e6c696e6b02000300636c69020008007265706c792d6570064012000200060073656e736f720200040074656d70010001000109400a0008400600010002002000
```
