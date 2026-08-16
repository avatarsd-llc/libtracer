# path-label/label-mixed

A packed `PATH` carrying name and label elements **in both orders** — a name before a label, and
a label before a name:

```
06 00 1A 00                       PATH, opt = 0x00, body 26 bytes
   06 73 65 6E 73 6F 72              record: len 6, "sensor"
   00 16 04 01 00 02 00              LABEL: kind 0x16, u32 LE 0x00020001 -> slot 1, gen 2
   04 74 65 6D 70                    record: len 4, "temp"
   00 16 04 02 01 FF FF              LABEL: kind 0x16, u32 LE 0xFFFF0102 -> slot 258, gen 65535
```

This is the vector [RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md)
§5.2's ruling *lives or dies by*. **Normative:** a `PATH` MAY carry any mixture of name and label
elements, in any order. There is no "fully minted" state a path must reach and no all-or-nothing
rule.

## Why mixing has to be legal

It follows directly from the ruling that **hosts that don't mint don't change their part**. A hop
that does not implement RFC-0027, or that refuses to mint — §8.3's exhaustion, §7.1's saturation,
a policy choice — relays the path with its own part still spelled as strings. Every *other* hop's
part still compacts, and every hop still reads its own part in whatever spelling it arrives. That
per-element degradation is the property §2.2 names as this design's distinguishing one, and it is
what a NARROW node buys by declining to mint at all: it pays none of the per-hop table state and
still benefits from every other hop's labels.

## Why this is safe here and unsafe for a bound path

A reader who knows [RFC-0024](../../../../../docs/spec/rfcs/0024-bound-paths-node-scoped-vertex-ref-source-routing.md)
§7.1 erratum 1 will notice it rules the *opposite* way for `PATH_REF`, and the difference is
framing. A `PATH_REF` is a bare fixed-stride array with **no per-element framing**, so its
elements are identified **by position**: a list that skips a host is not a shorter route but a
wrong one, because the next reader consumes an element another host minted. A packed path element
is **self-delimiting and self-describing** — a literal record by its length byte, a label by its
kind — so each element is read by the hop whose part it is, in the same order the strings were.
**Skipping is not expressible**, and erratum 1's mis-route class does not arise.

## The saturated generation is a legal value

The second label sits at generation `0xFFFF`, the **saturation point** of §4.3.1. It is a
perfectly usable label and a core must carry it: what saturation forbids is the *next* mint into
that slot, which retires the slot **permanently** — never reused, never wrapped, not after
reclamation, not after the peer departs, not after the table empties. That rule is invisible on
the wire, which is why it is bound by a host test (`core/tests/path_label_test.cpp`,
`saturate_and_retire`) and not by a vector.

## What a core must do

Round-trip the body bit-for-bit, walking `p += 1 + body[p]` for a literal record and
`p += 3 + body[p + 2]` for an escape. A core MUST NOT require labels to be leading, trailing,
contiguous, or present at all.
