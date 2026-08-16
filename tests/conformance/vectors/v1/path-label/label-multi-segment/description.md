# path-label/label-multi-segment

One label standing for a **three-segment** mount run, with the residual still spelled as strings:

```
06 00 13 00                       PATH, opt = 0x00, body 19 bytes
   00 16 04 03 00 01 00              LABEL: kind 0x16, u32 LE 0x00010003 -> slot 3, gen 1
                                       stands for the whole run /net/downlink/a
   06 73 65 6E 73 6F 72              record: len 6, "sensor"
   04 74 65 6D 70                    record: len 4, "temp"
```

[RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) **amendment 6**
rules §5.3 sub-question 2: a label covers a hop's **whole local part** — its entire mount run,
however many segments that is — and **not** one segment. §12.5 requires this case alongside the
one-segment `path-label/label-roundtrip` *"so the two cannot silently diverge"*.

## The arithmetic

The compacted run `/net/downlink/a` packs as `03 "net"` + `08 "downlink"` + `01 "a"` = **15 B**.
One label element is **7 B**. Per-*segment* labelling would have spent **21 B** on the same run —
worse than the strings it replaces.

| spelling | bytes for the run |
| --- | ---: |
| packed strings (`path/path-sensor-temp`'s encoding) | 15 |
| one label for the whole local part (**ruled**) | 7 |
| one label per segment (rejected) | 21 |

## Why per-segment was rejected

Not on bytes alone. A per-segment label would replace each name with a label but still leave a
**multi-element walk and a per-element table lookup** where a single deref belongs — turning an
O(1) local resolution back into O(segments), and preserving the very shape the RFC exists to
delete. The measured prize is a *per-segment* term: §3.2's canonical terminus resolution at
**+21.3 ns per address segment**, replaced by a deref **flat at 11 ns at every depth**. On NARROW
it is worse still: per-segment makes the per-hop mint table scale with **address depth**, which is
the one term a small node cannot bound, where the ruled form bounds it by peers × hops.

## Two consequences an implementation will hit

1. **Invalidation scope is the whole run.** Re-mounting *anywhere* inside a hop's local part
   retires that hop's label — the slot's generation bumps and the stale label lands in the
   `NOT_FOUND`-class fallback to the string spelling (§4, §7). A mint is never load-bearing
   (§6.3), so this is a re-resolution, not an error.
2. **Mixed granularity across hops stays legal**, because the rule is per-*hop*-local-part and not
   per-route: a one-segment run mints a label covering one segment (that is
   `path-label/label-roundtrip`), a three-segment run mints one covering three. Both are on the
   wire here and in `path-label/label-mixed`.

## What a core must do

Carry the bytes bit-for-bit. Nothing in the **element** distinguishes a one-segment label from a
three-segment one — the record is 7 B either way — so a core cannot infer the run's width from
the wire, and MUST NOT try: the width is known only to the minting host's table.
