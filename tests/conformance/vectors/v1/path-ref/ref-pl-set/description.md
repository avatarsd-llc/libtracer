# path-ref/ref-pl-set

A `PATH_REF` with **`opt.PL = 1`** — the same 16-byte body as `path-ref/ref-2host`, one
option bit different. `decode` MUST fail with `tr::frame::invalid`.

```
14 40 10 00     <- opt=0x40 sets PL       <- MUST be 0 (RFC-0024 §4.2)
   07 00 00 00 03 00 00 00
   2A 00 00 00 01 00 00 00
```

## Why this is rejected rather than ignored

`PL = 1` asserts "the payload is concatenated child TLVs", and a `PATH_REF` body is not
that: it is a fixed-stride record array. A generic `PL = 1` walker handed these bytes reads
the first four body bytes as a TLV header — `type` = the low byte of an index, `opt` = the
next — and **mis-frames the whole body**. Here the first element's index is 7, so a walker
would see `type = 0x07` (POINT), `opt = 0x00`, `length = 0x0000`, and march off into the
element array producing a tree of phantom children out of index and generation bytes.

So `PL = 0` is a MUST, not a convention, and the reject is what keeps a decoder from
inventing structure the format does not have. `opt.LL = 1` is forbidden for the parallel
reason: under a 2040-byte body cap it can only ever be two wasted bytes.

The `PATH` type makes the same flip for the same reason under RFC-0018's packing.
