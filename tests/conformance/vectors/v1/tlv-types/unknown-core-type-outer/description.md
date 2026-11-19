# tlv-types/unknown-core-type-outer

An outer TLV whose type is `0x1F`, a core-range code that annex 05 §Reserved range leaves
unassigned. Its body is three opaque bytes.

```
1F 00 03 00 AA BB CC
```

```
1F 00 03 00     type = 0x1F (unassigned), opt = 0x00, length = 3
   AA BB CC     opaque body
```

7 bytes total.

## The rule this pins

annex 01 §Handling unknown type codes: a receiver meeting a core-range code it does not implement
MUST NOT crash, MUST respect `length` when it skips the TLV, and MUST go on parsing the stream.
That is the forward extension path. A later revision may assign `0x1F`, and a receiver deployed
today steps over it.

This vector claims only that skip and that the link keeps working. A bare outer TLV carries no
`src`. Only the retired codes `0x11` to `0x13` have a rule that answers them on their arrival
link (`tlv-types/retired-route-handle-*`), so this vector claims no answer and claims no silence
either.

## Bound by

C++ `core/tests/fwd_fanout_test.cpp`: `test_unknown_type_vectors_skip_by_length` decodes these
bytes, steps over them by the header's length and parses the frame after them.
`test_unknown_outer_type_keeps_the_link` injects them into a router link, checks that nothing goes
out on any other link, and checks that a `FWD` after them on the same link is still served.
