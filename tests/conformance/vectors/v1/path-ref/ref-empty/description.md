# path-ref/ref-empty

A `PATH_REF` with a **zero-length body**: `H = 0`, no elements at all, 4 bytes total — the whole
frame is its envelope. `decode` MUST succeed and re-encode to the same four bytes.

```
14 00 00 00     <- type=PATH_REF(0x14), opt=0x00, length=0   <- 0 % 8 == 0, 0 <= 2040
```

## Why this is an `input.bin` and not a reject

The §4.3 bound is an **upper** one. The two structural length rules are `length % 8 == 0` and
`length <= 2040`, and zero satisfies both: an empty body is a whole number of elements, and it
is under the cap. There is no third clause, and inventing one — "at least one element" — would
be a codec rejecting a frame on what the bytes *mean* rather than on how they are shaped.

What an empty bound path means is a routing question, and §5 answers it there: a host that
derefs a `PATH_REF` walks one element per hop, so a zero-element one names no vertex and the
router refuses it with the same NACK any other failed deref gets. That refusal is a **router**
obligation, gated by the implementation's own host suite (HARNESS.md §"What a vector gates"),
and it is not weakened by the codec carrying the bytes: a frame has to decode before anything
can decide it is unroutable.

Together with `path-ref/ref-255-elements` and `path-ref/ref-256-elements` this pins both ends of
the accepted range, so a core that reads the bound as "1 ≤ H ≤ 255" fails here and one that
reads it as "H ≤ 256" fails there.
