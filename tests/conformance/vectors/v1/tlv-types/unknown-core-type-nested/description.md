# tlv-types/unknown-core-type-nested

The unassigned core-range code `0x1F` nested inside a structured TLV, with a known sibling after
it.

```
07 40 0C 00 1F 00 02 00 AA BB 02 00 02 00 6F 6B
```

```
07 40 0C 00         POINT, opt = 0x40 (PL = 1), length = 12
   1F 00 02 00 AA BB    type = 0x1F (unassigned), opaque body AA BB
   02 00 02 00 6F 6B    NAME "ok"
```

16 bytes total.

annex 01 §Handling unknown type codes: an unknown TLV nested inside a structured parent is
treated as opaque bytes, and parsing continues. A decoder passes this vector only if it steps
over the six bytes of the `0x1F` child by its declared length and reads the `NAME` that follows.
A decoder that stops at the first unknown child, or that refuses the whole parent, fails the
round trip.

Bound by C++ `core/tests/fwd_fanout_test.cpp`, `test_unknown_type_vectors_skip_by_length`, which
checks that both children decode and that the second is the `NAME`.
