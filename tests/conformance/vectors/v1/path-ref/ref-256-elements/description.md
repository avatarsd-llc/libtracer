# path-ref/ref-256-elements

**One element over the normative bound** (RFC-0024 §4.3): 256 elements, a 2048-byte body,
2052 bytes total. `decode` MUST fail with `tr::frame::invalid`.

```
14 00 00 08     <- type=PATH_REF(0x14), opt=0x00, length=2048   <- 2048 / 8 = 256 > 255
   00 00 00 00 00 00 00 00 ...  <- 256 elements
```

The companion `path-ref/ref-255-elements` is the largest accepted body, so the two vectors
straddle the bound: a core that computes the cap off by one fails exactly one of them.

The bound is not a magic number and not a buffer size. It is the largest count for which
every per-element quantity fits a `u8`, and it sits above both reachable ceilings a
canonical route can mint from (`H <= 69` today, `H <= 171` packed) — an encoding-independent
ceiling, in the discipline RFC-0023 set. A host with more hops than this has no bound
spelling and falls back to the canonical `PATH`, which every bound path is minted from and
which therefore always works.
