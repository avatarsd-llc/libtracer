# path-label/label-roundtrip

A packed `PATH` whose **one** element is an [RFC-0027](../../../../../docs/spec/rfcs/0027-label-switched-path-compression.md)
path label:

```
06 00 07 00                       PATH, opt = 0x00 (PL MUST be 0), body 7 bytes
   00 16 04 01 00 02 00              LABEL: escape, kind 0x16, len 4, u32 LE 0x00020001
                                       -> slot index 1, generation 2
```

This is the vector §12.5 asks for first: *"a `PATH` with one label element, round-tripped,
pinning the ledger byte for byte."* Those bytes are now settled — RFC-0027 **amendment 5**
rules the element to be [RFC-0018](../../../../../docs/spec/rfcs/0018-packed-path-segments.md)
§8's escape record, `00 <u8 kind = 0x16> <u8 len = 4> <u32 LE label>`, **7 B per element**. The
8-byte `PATH_LABEL` TLV-child spelling §5.3 carried as its leading candidate is **never built**,
so no type code `0x16` is assigned in `tlv.hpp` and there is no per-element option byte.

## The value

The 4-byte payload is one little-endian `u32` split 16/16 (RFC-0027 §4.1):

| half | bytes | value |
| --- | --- | --- |
| slot index | `01 00` | 1 |
| generation | `02 00` | 2 |

The index is an **array slot handed out by the minting host**, never a hash of the path text —
a hash collision is a mis-delivery, and this doc set closes that class by construction rather
than by digest width (§4.2). The generation is the anti-mis-route guard: it is compared on every
use, only ever moves forward, and **saturates and retires its slot rather than wrapping**
(§4.3.1). Generation `0` is reserved "no label" and never reaches the wire, so a zeroed payload
is refused without a table lookup.

## Host scope — what these bytes do NOT mean

A path label means something **only on the host that minted it**. The same 7 bytes presented to
any other host resolve to nothing: the table is keyed by the peer the label was minted for, so a
leaked label buys an attacker one `NOT_FOUND` and no state (§8.4). Nor is a label a capability —
§8.1 mints only post-auth and §8.2 re-checks the ACL at the dereferenced vertex on **every**
labelled operation. A generation match says the part is the same one; it never says the caller
may still act on it.

## What this shape is

A whole address spelled as one label is the **terminus-residual** case: the entire remaining
address is one hop's local part, so the minting hop replaced all of it. It is the axis §3.3 says
decides this RFC — the byte column alone ties `PATH_REF`, and what does not tie is replacing a
canonical terminus resolution measured at **+21.3 ns per address segment** with a table-indexed
deref **flat at 11 ns at every depth**.

## What a core must do

Carry these bytes bit-for-bit. The escape is admissible in a **frame path**; a hop that does not
implement minting steps over the record by its declared length, using the walk it already ships,
and relays the frame. In **key context** — `path_lookup_key`, a vertex-map lookup, any
`key_view_t` ancestor test — a `PATH` carrying this element is **inadmissible** and a resolver
answers `ERROR{tr::path::invalid}` (`0x0021`); `path/path-escape-in-key-context` is that half of
the rule, and the reason is RFC-0018 §5's injectivity: canonical keys stay pure-string.
