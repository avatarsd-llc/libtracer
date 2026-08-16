# path-label/label-wrong-length

A label element whose declared payload is **three** bytes, not four:

```
06 00 0D 00                       PATH, opt = 0x00, body 13 bytes
   06 73 65 6E 73 6F 72              record: len 6, "sensor"
   00 16 03 01 00 02                 escape, kind 0x16, len 3 -- NOT a label
```

A path label is exactly **4 bytes** — a `u16` slot index and a `u16` generation
([RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md) §4.1) — and
§12.5 requires the case where the payload is not. This is one of the **two** structural clauses
of the ruled element, and it has its own vector for RFC-0024 §9.4's reason: a core that enforces
only one of two clauses sharing a vector passes that vector anyway. The other clause is
`path-label/label-foreign-kind`.

## What a core must do

| role | required behaviour |
| --- | --- |
| a codec relaying the frame | carry the bytes; the record is self-delimiting, so `p += 3 + body[p + 2]` steps over it |
| a hop that implements `kind = 0x16` | **refuse the address** — `ERROR{tr::path::invalid}` (`0x0021`). It MUST NOT read three bytes as a label, MUST NOT zero-extend them, and MUST NOT skip the element and resolve what is left |
| any key context | refuse, as for every escape record (`path/path-escape-in-key-context`) |

The refusal matters because the two halves of a label are **not** independently meaningful: read
short, `01 00 02` would be slot 1 at some generation assembled out of a byte that is not there,
and a generation that is not the minting host's is exactly #603's mis-delivery class — the class
this RFC closes by construction.

## Why this is an `input.bin` case and not a `reject.bin` one

`reject.bin` means **decode** must fail (HARNESS.md §negative cases). A packed `PATH` body is
`opt.PL = 0` — opaque bytes to the codec — so there is no framing here for a codec to refuse, and
the TypeScript and Rust cores are codecs with no resolver. Every core decodes and round-trips
these bytes correctly. The requirement is a **resolver** requirement, exactly as
`path/path-escape-in-key-context` records one.

## Note on §12.5's retired sibling vectors

§12.5 was written against §5.3's *candidate* 8-byte `PATH_LABEL` TLV-child spelling and asks for
`label-pl-set` and `label-ll-set` — an `opt.PL = 1` / `opt.LL = 1` **on the label element's own
TLV header**. Amendment 5 rules that spelling is **never built**: the element is RFC-0018 §8's
escape record, which is not a TLV and has **no option byte**, so both cases are unrepresentable —
the same fate, for the same reason, as the retired `path/path-value-children-illegal`. What
survives is the clause-per-vector discipline they existed to serve, and it is carried by this
vector and by `path-label/label-foreign-kind`. Recorded as RFC-0027 §12.5 erratum 1.
