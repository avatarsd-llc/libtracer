# stream/batch-packed-unaware

[RFC-0025](../../../../../../docs/spec/rfcs/0025-stream-class-values.md) §7 vector 18
(Amendment 5, §4.1.4) — **a consumer that does not implement the packed block treats the value
as opaque**:

```
BATCH (PL=1, type=0x80) {
  TIME  (PL=0) { u64 LE = 1700000000000000000 }   ; the batch BASE
  VALUE (PL=0) { i32 LE [0, 250] }                ; the offset run — NON-UNIFORM stream
  VALUE (PL=0) { FFFF0000 FF00FF00 }              ; the BLOCK — n = 8 / 4 = 2 samples
}
```

The descriptor declares `width = 4` and `dt_ns = 0`: two samples, at `base + 0` and
`base + 250` ns. A consumer that implements Amendment 5 reads exactly that.

A consumer that does not — one that knows the framed batch but has never heard of `width` —
**MUST treat the value as opaque** once the descriptor says `width != 0`. It may store it,
forward it and hand it to its application as bytes; it must not split it into sample frames.
The reason is in these bytes: read as framed, this record is an offset run of two entries over
one sample child, which a framed reader refuses; and on a uniform stream the same reader would
not refuse at all, but read the whole block as **one** sample. The opaque rule is what keeps that
misreading from happening silently.

A node that moves the value — a relay, a store, the durability latch — is unaffected: it never
interpreted a batch, so the bytes pass through unchanged.

Bound in C++ by `core/tests/batch_test.cpp` §`packed_block_unaware` — the packed reader reads
two samples with their offsets, the framed reader refuses the same bytes, and a decode and
re-encode reproduces them byte for byte.
