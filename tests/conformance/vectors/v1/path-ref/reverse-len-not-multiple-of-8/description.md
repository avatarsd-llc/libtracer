# path-ref/reverse-len-not-multiple-of-8

A `PATH_REF_REVERSE` (`0x15`) whose `length` is **not** a whole number of elements: 12 bytes,
one and a half elements.

```
15 00 0C 00     <- type=PATH_REF_REVERSE(0x15), opt=0x00, length=12   <- 12 % 8 = 4, invalid
   07 00 00 00 03 00 00 00      <- element 0
   AA BB CC DD                  <- four bytes that are not an element
```

`decode` MUST fail with `tr::frame::invalid`.

## Why this vector exists next to `ref-len-not-multiple-of-8`

The reverse list got its own type code in RFC-0024 §7.1 amendment 2, and a new code is a new
place for the structural rule to be forgotten. Its body grammar is `0x14`'s **exactly** —
`opt.PL` and `opt.LL` MUST both be 0, `length` MUST be a multiple of 8, the element count MUST
be ≤ 255 — because the two codes differ in *role*, not in shape: one is an address, the other
is the list a mint-flagged request accumulates on its way to the responder.

A core that applies the shape rule to `0x14` alone accepts this frame, and then the two codes
have disagreed about what a bound-path body is. The four trailing bytes are deliberately not
zero, for `ref-len-not-multiple-of-8`'s reason: a decoder that silently truncated to the
nearest whole element re-encodes to different bytes and fails the round-trip rather than
passing quietly.
