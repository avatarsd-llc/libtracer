# path-ref/ref-ll-set

A `PATH_REF` with **`opt.LL = 1`** — the same single element as `path-ref/ref-1host`, carried
under the 6-byte header and the u32 length field. `decode` MUST fail with `tr::frame::invalid`.

```
14 08 08 00 00 00     <- opt=0x08 sets LL, length is u32   <- LL MUST be 0 (RFC-0024 §4.2)
   07 00 00 00 03 00 00 00      <- element 0: index=7, generation=3
```

Every other rule holds: `PL` is 0, the body is one whole element, the count is inside the §4.3
bound. The frame is well-formed under all of them and invalid under exactly one, which is what
makes it the discriminating case for that clause.

## Why the wide length is forbidden rather than merely unused

A `PATH_REF` body is at most 2040 bytes (§4.3, 255 elements × 8), so the u32 length can never
carry a value the u16 one could not. Leaving `LL` free would make two spellings of every bound
path — the same elements, four header bytes apart — and an address form that is a **mint key**
downstream cannot have two byte spellings of one route without the deriving end having to
normalise first. So §4.2 forbids the bit instead of ignoring it, exactly as it forbids `PL`.

This vector is the LL half of that pair; `path-ref/ref-pl-set` is the PL half. The two clauses
are independent, so a core that implements one and drops the other passes every other vector in
the category — which is the drift this case exists to catch.
