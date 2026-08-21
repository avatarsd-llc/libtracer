# stream/batch-time-roundtrip

[RFC-0025](../../../../../../docs/spec/rfcs/0025-stream-class-values.md) §7 vector 8
(restated from `stream/batch-trailer-roundtrip` by Amendment 1, §4.2.1) — **a batch's sample
time is a payload `TIME` child, and a uniform stream spends 0 bytes per sample on it**:

```
BATCH (PL=1, type=0x80) {
  TIME  (PL=0) { u64 LE = 1700000000000000000 }   ; the batch BASE — sample time of frame 0
  VALUE (PL=0) { u16 LE = 0x0064 }                ; sample 0 — no time of its own
  VALUE (PL=0) { u16 LE = 0x0065 }                ; sample 1
  VALUE (PL=0) { u16 LE = 0x0066 }                ; sample 2
}
```

`0x80` is the user-range code **formally assigned to BATCH** by §4.1.2 (Amendment 3, clause 6);
a BATCH is otherwise an ordinary structured TLV, so this vector mints no grammar and every
conforming decoder already decodes it ([05-protocol-tlvs.md](../../../../../../docs/reference/05-protocol-tlvs.md)
§User range).

**Per-sample time is DERIVED.** The stream's §4.3 descriptor — a `SETTINGS` LKV *beside* the
data vertex, negotiated once and never repeated per batch — declares `dt_ns = 1000` here, so
`t(i) = 1700000000000000000 + i × 1000`. Nothing per-sample is transmitted: a 2-byte sample
costs 2 bytes. A **non-uniform** stream is exactly one whose descriptor declares `dt_ns == 0`,
and it carries one packed `i32` LE offset array in a single `VALUE` child between the `TIME`
and the sample frames — 4 bytes per sample, contiguous, no per-child TLV header and no anchor
walk. Which of the two shapes these bytes are is a fact about the **descriptor**, never about
the record: the graph never interprets a BATCH (claim 5).

**The record carries no trailer.** Wire/TX time is `opt.TS` on the **outermost** frame only and
is always `TF=0` (§4.2.1 consequence 1); a written value is an inner TLV, so nothing here is
stamped. The retired §4.2 spelling — a `TF=0` parent plus a `TF=1` trailer on every child —
spent 4 trailer bytes per sample to say what the descriptor already says, and would have made a
folded stream unencodable as a branch write (§4.1.2 clause 5, which rejects trailer-carrying
nodes inside one).

```
80401e000c00080000002a36fe9c9717010002006400010002006500010002006600
```
