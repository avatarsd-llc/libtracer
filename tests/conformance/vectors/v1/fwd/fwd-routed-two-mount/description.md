# fwd/fwd-routed-two-mount

FWD{ op=READ, dst=/net/uplink/b/net/uplink/c/sensor/temp, src=/reply-ep } — the genuine two-hop route: a `dst` that crosses **two** mounts before it reaches a terminus.

Every other `fwd/` vector carries a `dst` that at most one forwarder ever touches. This one names two mount keys back to back, so the address is consumed by two different nodes in sequence and only the tail survives to resolve locally (#419).

## The topology this `dst` addresses

```
client ──▶ A ──(A's mount net/uplink/b)──▶ B ──(B's mount net/uplink/c)──▶ C
                                                                          /sensor/temp
```

Both hops route by the RFC-0014 §S2a / ADR-0061 qualified mount key `net/<module>/<name>`, matched longest-prefix-first in one registry pass (`resolve_mount_by`). Two hops therefore mean **two mount keys, six segments** — not two segments.

## Hop-by-hop consumption

| Hop | Node | Registry match | `strip_k` | `dst` it forwards | `src` it prepends |
| --- | --- | --- | --- | --- | --- |
| 1 | A | `net/uplink/b` | 3 | `/net/uplink/c/sensor/temp` | A's mount for the inbound client link |
| 2 | B | `net/uplink/c` | 3 | `/sensor/temp` | B's mount for the inbound link from A |
| — | C | no prefix of `sensor/temp` is registered | — | (terminates) | — |

`src` grows by the forwarder's **full** inbound mount run — the whole `net/<module>/<name>`, never a bare NAME — which is what keeps the accumulated reverse route resolvable through the same strip-K descent on the way back (the ADR-0061 erratum).

## Byte breakdown

`0x0F FWD`, `opt.PL=1`, `length=0x003C` (60), 64 bytes total. The `PATH`s carry packed `[u8 len][utf8]` segment records
([RFC-0018](../../../../../docs/spec/rfcs/0018-packed-path-segments.md)), so each is `opt=0x00` and each
segment costs `1 + len` rather than `4 + len`.

| Offset | Bytes | Meaning |
| --- | --- | --- |
| 0 | `0F 40 3C 00` | FWD, PL=1, body length 60 |
| 4 | `01 00 01 00 00` | VALUE u8 `0x00` — `op = READ` |
| 9 | `06 00 26 00` | PATH `dst`, PL=0, body length 38 |
| 13 | `03 "net"` | dst[0] — mount 1, segment 1 |
| 17 | `06 "uplink"` | dst[1] — mount 1, segment 2 (module) |
| 24 | `01 "b"` | dst[2] — mount 1, segment 3 (connection name) |
| 26 | `03 "net"` | dst[3] — mount 2, segment 1 |
| 30 | `06 "uplink"` | dst[4] — mount 2, segment 2 (module) |
| 37 | `01 "c"` | dst[5] — mount 2, segment 3 (connection name) |
| 39 | `06 "sensor"` | dst[6] — residual, resolved locally at C |
| 46 | `04 "temp"` | dst[7] — residual, resolved locally at C |
| 51 | `06 00 09 00` | PATH `src`, PL=0, body length 9 |
| 55 | `08 "reply-ep"` | src[0] — the originator's reply endpoint |

## What this vector does and does not gate

Per [HARNESS.md](../../../../HARNESS.md), a vector gates the **codec** only: the harness contract is `encode(decode(input.bin)) == input.bin`, and a static fixture routes nothing. The hop-by-hop consumption above is a *behavioural* claim, so it is bound where it can be false — `core/tests/fwd_two_mount_test.cpp`, which drives three in-process `fwd_router_t` nodes chained by loopback link pairs through the production `transport_vertex_t` wiring and asserts both `strip_k` values and the terminus delivery.

```
0f405700010001000006403e00020003006e65740200060075706c696e6b0200010062020003006e65740200060075706c696e6b02000100630200060073656e736f720200040074656d7006400c00020008007265706c792d6570
```
