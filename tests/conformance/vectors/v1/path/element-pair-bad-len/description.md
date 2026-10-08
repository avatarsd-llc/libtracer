# path/element-pair-bad-len

A `kind = 0x16` escape record whose declared payload is **seven** bytes, not eight:

```
06 00 11 00                          PATH, opt = 0x00, body 17 bytes
   06 73 65 6E 73 6F 72                 NAME "sensor"
   00 16 07 03 00 00 00 01 00 00        escape, kind 0x16, len 7 -- NOT a PAIR
```

A PAIR is exactly a `u32` index and a `u32` generation
([RFC-0029](../../../../../docs/spec/rfcs/0029-one-path-primitive.md) §5.1). A `kind = 0x16`
record of any other length is **malformed** and **refuses the address, never the frame**.

## What a core must do

| role | required behaviour |
| --- | --- |
| a codec relaying the frame | carry the bytes; the record is self-delimiting and is stepped over by its declared length |
| a hop that implements `kind = 0x16` | **refuse the address** — `ERROR{tr::path::invalid}` (`0x0021`). It MUST NOT read seven bytes as a pair, MUST NOT zero-extend them and MUST NOT skip the element and resolve what is left |
| any key context | refuse, as for every escape record |

Read short, `03 00 00 00 01 00 00` would be slot 3 at a generation assembled out of a byte that is
not there — a generation the owner never issued, which is exactly the mis-delivery class the
generation compare exists to close.

Until RFC-0029 slice S3 deletes it, `len = 4` is still RFC-0027's label element; that is why this
vector uses 7 and not 4. After S3 a `len = 4` record is malformed too, with no diagnostic
distinguishing it (§16 Q4).

## Why this is an `input.bin` case and not a `reject.bin` one

`reject.bin` means **decode** must fail. A packed `PATH` body is `opt.PL = 0` — opaque bytes to the
codec — so there is nothing here for a codec to refuse; the requirement is a **resolver**
requirement, as for `path-label/label-wrong-length`.

## What this vector gates

The codec. The refusal is bound by `core/tests/path_pair_test.cpp` — `codec` (the element walker
answers `MALFORMED` for every length but 8 and 4, while `packed_record_span` still steps over the
record) and `refusals` (a `dst` headed by such a record is answered `tr::path::invalid` through the
production router).
