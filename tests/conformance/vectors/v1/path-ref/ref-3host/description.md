# path-ref/ref-3host

A **one-forwarder** bound path (RFC-0024 §4.1): `H = 3` — origin, forwarder, terminus.
Element 1 is forwarder 1's own reference to its connection vertex for the next hop.

```
14 00 18 00     <- type=PATH_REF(0x14), opt=0x00, length=24 (u16 LE)
   07 00 00 00 03 00 00 00      <- element 0: index=7,  generation=3   (origin -> hop 1)
   13 00 00 00 0C 00 00 00      <- element 1: index=19, generation=12  (forwarder -> hop 2)
   2A 00 00 00 01 00 00 00      <- element 2: index=42, generation=1   (terminus target)
```

**28 bytes total** — RFC-0024 §3.2's one-forwarder row. The shape worth noticing is that a
canonical `dst` grows with **segments** while a bound path grows with **hosts**: each host
costs one mount run canonically (at least 3 segments) but exactly 8 B bound.

A forwarder consumes element 0 and forwards the remainder, the same monotone shrink the
canonical `dst` performs — which is why a bound path is loop-free by construction and
needs no visited set. **The hop structure is explicit on the wire**: H elements is H hosts,
with no reading of names required.
