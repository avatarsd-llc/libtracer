# fwd/pair-stale-generation-not-found

The answer a node gives a PAIR it **cannot validate**
([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) §6 step 2, §6.3):

```
0F 40 34 00                          FWD, opt = 0x40 (PL = 1), length 52
   01 00 01 00 03                       VALUE op = REPLY
   06 00 09 00 08 72 65 70 6C 79 2D 65 70  PATH dst = /reply-ep (the request's src)
   06 00 0B 00                          PATH src, length 11 (the request's dst, echoed)
      00 16 08 03 00 00 00 01 00 00 00     PAIR 3:1 — refused
   01 00 01 00 01                       VALUE kind = ERROR
   09 40 0A 00 08 40 06 00 01 00 02 00 20 00   STATUS{ ERROR{ VALUE u16 = 0x0020 } }
```

`tr::path::not_found` (`0x0020`) — nothing forwarded, nothing applied, nothing repaired.

**One answer for every way the dereference fails**: the index is out of range, the generation
moved (the vertex was retired, or its slot re-tenanted), the generation saturated, or the slot
holds a placeholder. RFC-0024 §5.3's drop-never-mis-route and RFC-0027 §7.2's `NOT_FOUND` are one
rule here; a multi-element **forward** refusal now answers too, rather than dropping silently.

The origin's recovery is the canonical string it still holds: it clears its cached chain and
re-sends the string, and the reply to that re-teaches the chain (§6.3, from slice S2). **One
failed operation is the entire cost** — no withdraw frame, no lease, no TTL, no repair.

## What this vector gates

The codec. That the production router answers a stale pair with exactly these bytes (modulo the
echoed pair's value, which is whichever the owner refused) is bound by
`core/tests/path_pair_test.cpp` — `vectors` and `refusals` (moved generation, out-of-range index,
stale hop element, saturated generation, retired vertex).
