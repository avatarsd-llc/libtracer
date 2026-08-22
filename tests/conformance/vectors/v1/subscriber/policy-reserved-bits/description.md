# subscriber/policy-reserved-bits

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.3 — **reserved bits are ignored, not an error**:

```
SUBSCRIBER (PL=1) {
  PATH (PL=0) { 06 "client" }
  SETTINGS (PL=1) {
    NAME "delivery_policy" VALUE u16=0xFFC1   ; bits 6-15 all set, plus reliability=1
                                              ;   = delivery_class 3 + reserved 8-15
  }
}
```

`0xFFC1` decodes (§3.A, as narrowed by
[RFC-0025](../../../../../../docs/spec/rfcs/0025-stream-class-values.md) §4.1) as
`reliability = 1` (bits 0–1), `priority = 0` (bits 2–4), `durability_request = 0` (bit 5),
`delivery_class = 3` **stream** (bits 6–7), and **every** reserved bit 8–15 set.

**The bytes are unchanged.** RFC-0025 §4.1 took bits 6–7 out of the reserved range for
`delivery_class`, whose `0` = conflate is today's behaviour byte-identically — so a subscriber
that predates the class wrote `0` there when the bits were reserved and is conflate-class **by
construction**. This vector's word already carried `11` in those bits, and it still does:
Amendment 3 (§4.1.2 clause 7) repairs the vector **in place, same bytes**, narrowing only this
description and the three-language gates from "bits 6–15 reserved" to **bits 8–15 reserved**.

A conforming receiver MUST accept the subscribe and MUST ignore bits 8–15 — it may not
reject the frame, and it may not let those bits leak into the honoured fields or into the
class. The bits are carried verbatim, so the subscription reads back from `:subscribers[]`
unchanged: a node that predates a future bit's meaning round-trips it rather than erasing it.
Reading the class is not honouring it — every class beyond `conflate` lands with the
fan-out-edge mechanics and the receiving vertex's ring.

```
04402b0006400a0002000600636c69656e740b40190002000f0064656c69766572795f706f6c69637901000200c1ff
```
