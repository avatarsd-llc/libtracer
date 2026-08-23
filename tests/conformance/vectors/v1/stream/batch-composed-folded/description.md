# stream/batch-composed-folded

[RFC-0025](../../../../../../docs/spec/rfcs/0025-stream-class-values.md) §7 vector 13
(Amendment 4, §4.1.3, on the 2026-08-23 erratum) — **the same composed batch as
`stream/batch-composed-standalone`, in the FOLDED carriage**:

```
VALUE (PL=1) {                                    ; the branch-write node's ONE structured value
  TIME  (PL=0) { u64 LE = 1700000000000000000 }   ; the batch BASE — sample time of frame 0
  VALUE (PL=0) { i32 LE [0, 1500, -250] }         ; the packed offset run — NON-UNIFORM stream
  VALUE (PL=0) { u16 LE = 0x0064 }                ; sample 0
  VALUE (PL=0) { u16 LE = 0x0065 }                ; sample 1
  VALUE (PL=0) { u16 LE = 0x0066 }                ; sample 2
}
```

**One byte differs from `stream/batch-composed-standalone`, and it is the first one.** `0x80`
becomes `0x01` (`VALUE`); the `TIME` base, the packed offset run and the sample frames are
byte-for-byte the same. That is the carriage rule of §4.1.2 clause 6 as scoped by the 2026-08-23
erratum, stated as a checkable pair rather than as prose. Both spellings come out of one layout
implementation (`tr::wire::store_batch_head`, `core/include/libtracer/batch.hpp`), so they cannot
drift.

**Why the folded seat is a `VALUE` and not a `0x80`.** A flush folded into a
`propagate(v, FOLD)` branch write rides a node whose grammar is held byte-for-byte at
[RFC-0005](../../../../../../docs/spec/rfcs/0005-subtree-subscriptions.md) §B: a leading `NAME`,
**at most one** `VALUE`, and recursive `POINT` children. A `0x80` child is "any other child
type", which every conforming decomposer rejects. Seating the batch in the node's one admitted
`VALUE` needs no new grammar, no new admitted child, no terminus change and no new vector on the
receiving side — and it means a folded batch presents **no user-range byte to the graph at all**,
so claim 5 survives the fold untouched.

**How a consumer tells the carriages apart: it does not need to.** The §4.3 stream descriptor at
the vertex is the discriminator in both, so the type byte is not load-bearing for
interpretation — which is exactly why it is free to differ.

`stream/fold-carries-composed-batch` is these 50 bytes seated in the frame a real folded sweep
emits.

```
01402e000c00080000002a36fe9c971701000c0000000000dc05000006ffffff010002006400010002006500010002006600
```
