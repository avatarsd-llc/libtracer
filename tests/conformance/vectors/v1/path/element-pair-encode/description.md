# path/element-pair-encode

A packed `PATH` body that **mixes** NAME and PAIR elements
(RFC-0029 (`docs/spec/rfcs/0029-one-path-primitive.md`, [#1634](https://github.com/avatarsd-llc/libtracer/pull/1634)) §4.1–§4.2):

```
06 00 14 00                          PATH, opt = 0x00, body 20 bytes
   03 6E 65 74                          NAME  "net"
   00 16 08 03 00 00 00 01 00 00 00     PAIR  index = 3, generation = 1
   04 74 65 6D 70                       NAME  "temp"
```

A PAIR is a vertex's **owner-issued** `(u32 index, u32 generation)` — RFC-0024 §4.4's element,
unchanged — spelled as the RFC-0018 escape record `00 <kind = 0x16> <len = 8> <u32 LE index>
<u32 LE generation>`: **11 bytes**. `kind = 0x16` is reused (§16 Q4); the declared length is what
makes it a PAIR.

## What a core must do

| role | required behaviour |
| --- | --- |
| a codec | carry the bytes; every record is self-delimiting, so `p += 3 + body[p + 2]` steps over the PAIR |
| the node the PAIR belongs to | dereference it — bounds, generation, registered — and hop, terminate or refuse by what the vertex is (§6) |
| any key context | refuse: a chain is a **frame** path and never a canonical key (§5.1; `v1.md` §3.1) |

Each element is read by **exactly one** node and self-describes by its kind and length, so any
element may be a NAME or a PAIR, in any order (§4.2). A node that cannot issue a PAIR for its
part leaves that part as NAMEs and every other node's part still compacts.

## What this vector gates

The codec, and only the codec ([HARNESS.md](../../../../HARNESS.md)). The element's spelling is
pinned byte-exact against the reference emitter (`emit_path_pair`) by
`core/tests/path_pair_test.cpp` — `codec` and `vectors`.
