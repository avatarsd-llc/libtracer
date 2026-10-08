# fwd/pair-hop-egress

A two-element chain ([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) §4.2):

```
0F 40 2C 00                          FWD, opt = 0x40 (PL = 1), length 44
   01 00 01 00 00                       VALUE op = READ
   06 00 16 00                          PATH dst, length 22
      00 16 08 07 00 00 00 01 00 00 00     element 0: PAIR 7:1 (this node's)
      00 16 08 03 00 00 00 01 00 00 00     element 1: PAIR 3:1 (the next node's)
   06 00 09 00 08 72 65 70 6C 79 2D 65 70  PATH src = /reply-ep
```

The receiving node reads **only its head element**. `7:1` dereferences (bounds, generation,
registered) to the connection vertex of one of its point-to-point children, so this node is a
**hop** (§6 step 3):

1. **§6.4** — the operation's right (READ) is evaluated at that connection vertex for the
   caller's subject — the same gate, and so the same verdict, the NAME spelling of the same hop
   reaches;
2. it egresses over that child with `dst` shrunk by **exactly the consumed element** — the next
   node receives `PATH{ 00 16 08 03 00 00 00 01 00 00 00 }`;
3. `src` grows **canonically**, by the inbound link's NAME run (§6.1) — never by a PAIR, so the
   reply's `dst` is a canonical route.

**The hop holds nothing.** The pair *is* its own vertex-index entry: no table, no descent, no
per-request state, zero heap.

## What this vector gates

The codec. That the hop's egress is **byte-identical** to the NAME spelling's egress for the same
hop and tail, and that the granted and ungranted rights cross (or do not) in both spellings alike,
is bound by `core/tests/path_pair_test.cpp` — `hop` and `authorization` — against the production
`fwd_router_t`.
