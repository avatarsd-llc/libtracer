# fwd/pair-terminus

A **chain of length one** (RFC-0029 (`docs/spec/rfcs/0029-one-path-primitive.md`, [#1634](https://github.com/avatarsd-llc/libtracer/pull/1634))
§4.2):

```
0F 40 21 00                          FWD, opt = 0x40 (PL = 1), length 33
   01 00 01 00 00                       VALUE op = READ
   06 00 0B 00                          PATH dst, length 11   <- the whole address
      00 16 08 03 00 00 00 01 00 00 00     element 0: PAIR index = 3, generation = 1
   06 00 09 00 08 72 65 70 6C 79 2D 65 70  PATH src = /reply-ep
```

The only element names the target on the receiving node. It dereferences to an ordinary vertex
and is the last element, so this node is the **terminus** (§6 step 3) and applies the operation
with the **same `apply_op`** the NAME spelling reaches. A PAIR-spelled terminus never
write-creates (RFC-0005 amendment 1's remote rule).

**37 bytes**, against 38 for the NAME spelling `/sensor/temp` of the same operation — and 4 bytes
a hop more than the retired 7-byte RFC-0027 label (§5.2), bought back by deleting the per-hop
table (§11).

## What a PAIR is still subject to

Everything the NAME spelling is subject to. The operation's gate is the resolver's own
`graph_t::read` / `write` / `await` at the dereferenced vertex, under the same subject — **a
generation match authorizes nothing** (§6.4). A PAIR is an address, not a capability.

## What this vector gates

The codec. That the terminus's verdict is **byte-identical** to the NAME spelling's, and that a
PAIR-spelled WRITE lands on the vertex, is bound by `core/tests/path_pair_test.cpp` — `terminus`.
