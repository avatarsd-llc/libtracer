# path-ref/ref-1host

The **minimal** bound path (RFC-0024 §4): one 8-byte element, so `H = 1` and the single
element is the terminus host's reference to the target vertex itself. 12 bytes total —
the 4-byte envelope plus `8 x 1`.

```
14 00 08 00     <- type=PATH_REF(0x14), opt=0x00 (PL=0 AND LL=0, both MUSTs), length=8
   07 00 00 00 03 00 00 00      <- element 0: index=7, generation=3 (u32 LE each)
```

The envelope is 4 bytes and there is no fifth (RFC-0024 §4.2): no element-count field —
the count **is** `length / 8` — and no per-element TLV header, because element *i* is
`body[8i .. 8i+8)`, computed rather than parsed.

## What this vector gates

The codec, and only the codec (HARNESS.md §"What a vector gates"). A core scoring `ok`
here carries the bytes across its decoder and encoder without losing a bit. It says
nothing about routing: an element is **node-scoped** — meaningless anywhere but on the
host that minted it — so no codec anywhere can check that `index=7` names a live vertex.
That validation (bounds check, generation compare, ACL re-check at the deref'd vertex,
RFC-0024 §5-§6) is a router obligation and lands with the routing car.
