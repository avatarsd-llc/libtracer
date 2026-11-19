# fwd/fwd-routed-mount-residual

FWD{ op=READ, dst=/net/board/can0/ow/sensor, src=/reply-ep } — ONE strip-K mount hop: the mount `net/board/can0`, plus the residual `ow/sensor` it forwards.

Under the shipped per-module mount routing (RFC-0014 §S2a / ADR-0061) a connection registers under the qualified key `net/<module>/<name>`, and `resolve_mount_by` matches the **longest registered prefix** of `dst` in one pass. The leading three segments of this `dst` are therefore not three hops — they are the single mount key `net/board/can0` (module `board`, connection `can0`), consumed as one run:

| Segment | 0 | 1 | 2 | 3 | 4 |
| --- | --- | --- | --- | --- | --- |
| Name | `net` | `board` | `can0` | `ow` | `sensor` |
| Role | mount key `net/board/can0` (`strip_k = 3`) | ← | ← | residual `/ow/sensor` | ← |

The forwarding node strips 3 segments, forwards `dst=/ow/sensor` over that link, and prepends its own inbound link's full mount run to `src`. This is a **one-forwarder** case; the genuine two-mount route is `fwd/fwd-routed-two-mount`.

The bytes are unchanged from when this vector was authored as `fwd-routed-multihop`. That name, and its "un-stripped multi-hop dst" prose, predate S2a and read the same `dst` string as two successive bare-name hops (`board`, then `can0`). The bytes were always consistent with the mount reading; only the name and the prose were stale (#419, maintainer ruling (a), 2026-08-02).

```
0f402f00010001000006001900036e657405626f6172640463616e30026f770673656e736f7206000900087265706c792d6570
```
