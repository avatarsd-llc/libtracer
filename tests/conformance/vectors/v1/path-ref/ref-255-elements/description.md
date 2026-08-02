# path-ref/ref-255-elements

The **normative maximum** bound path (RFC-0024 §4.3): 255 elements, a **2040-byte** body,
**2044 bytes** total. One element over this is `tr::frame::invalid` —
`path-ref/ref-256-elements`.

```
14 00 F8 07     <- type=PATH_REF(0x14), opt=0x00, length=2040 (u16 LE)
   00 00 00 00 00 00 00 00      <- element 0:   index=0,   generation=0
   01 00 00 00 07 00 00 00      <- element 1:   index=1,   generation=7
   ...                          <- element i:   index=i,   generation=(i*7) mod 65536
   FE 00 00 00 F2 06 00 00      <- element 254: index=254, generation=1778
```

## Why 255, and why it is derived rather than chosen

255 is the largest count for which **every** per-element quantity — the count itself, the
largest index (254), a receiver's per-element table dimension — fits a `u8`, in the
discipline RFC-0023 set for the canonical segment cap. It sits *above* both reachable
ceilings: a bound path is always minted from a canonical route, and a canonical body caps
at 1024 B while a hop costs at least one 3-segment mount run, so `H <= 69` under today's
NAME encoding and `H <= 171` under packed segments. Being above both is the point — the
bound is an **encoding-independent** ceiling rather than an artifact of whichever body
grammar `PATH` currently uses.

At the maximum the body is 2040 B, three orders inside a u16 length. That is the whole
argument for `opt.LL = 0` being a MUST rather than a convention: there is no reachable
`PATH_REF` for which `LL = 1` is anything but two wasted bytes.
