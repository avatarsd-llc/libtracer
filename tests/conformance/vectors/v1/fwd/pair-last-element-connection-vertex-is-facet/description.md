# fwd/pair-last-element-connection-vertex-is-facet

A PAIR that dereferences to a **connection vertex** as the **last** element
([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) §6 step 3):

```
0F 40 21 00                          FWD, opt = 0x40 (PL = 1), length 33
   01 00 01 00 00                       VALUE op = READ
   06 00 0B 00                          PATH dst, length 11
      00 16 08 07 00 00 00 01 00 00 00     element 0: PAIR 7:1 — a connection vertex
   06 00 09 00 08 72 65 70 6C 79 2D 65 70  PATH src = /reply-ep
```

With no tail there is **no hop**. The element addresses the connection vertex **itself** — its
value and its `:`-facets — and this node is the terminus. That is RFC-0004 §A's dual nature of a
connection vertex, unchanged: a NAME `dst` that names a mount *exactly* terminates at the mount's
connection vertex too, and only a `dst` with something **below** the mount is a forward.

The bytes are identical in shape to `fwd/pair-terminus`: whether `7:1` names a connection vertex
or an ordinary one is a fact about the owner's index, not about these bytes, and only the
**presence of a tail** decides hop versus terminus.

## What this vector gates

The codec. That the router answers here — nothing egresses over the link the vertex names — with
the verdict the NAME spelling of the mount itself gets is bound by `core/tests/path_pair_test.cpp`
— `terminus` (section 4).
