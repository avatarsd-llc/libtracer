# path-ref/ref-2host

A **direct-link** bound path (RFC-0024 §4.1): `H = 2`, so element 0 is the origin's own
reference to the connection vertex for its first hop, and element 1 is the terminus
host's reference to the **target vertex itself**.

```
14 00 10 00     <- type=PATH_REF(0x14), opt=0x00, length=16 (u16 LE)
   07 00 00 00 03 00 00 00      <- element 0: index=7,  generation=3
   2A 00 00 00 01 00 00 00      <- element 1: index=42, generation=1
```

**20 bytes total**, which is the `4 + 8H` of RFC-0024 §3.2's direct-link row, byte for
byte. The canonical spelling of the same route is 54 B there; that saving is arithmetic
over the shipped encoding rule, not a capture.

Each element is *that host's own* reference and means nothing on any other host — a bound
path is a stack of node-scoped references, not a global name. The canonical `PATH` is
untouched by this form and remains both the mint key and the fallback.
