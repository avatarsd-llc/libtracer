# subscriber/policy-reserved-bits

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.3 — **reserved bits are ignored, not an error**:

```
SUBSCRIBER (PL=1) {
  PATH (PL=0) { 06 "client" }
  SETTINGS (PL=1) {
    NAME "delivery_policy" VALUE u16=0xFFC1   ; bits 6-15 all set, plus reliability=1
  }
}
```

`0xFFC1` decodes (§3.A) as `reliability = 1` (bits 0–1), `priority = 0` (bits 2–4),
`durability_request = 0` (bit 5), and **every** reserved bit 6–15 set.

A conforming receiver MUST accept the subscribe and MUST ignore bits 6–15 — it may not
reject the frame, and it may not let those bits leak into the honoured fields. The bits are
carried verbatim, so the subscription reads back from `:subscribers[]` unchanged: a node that
predates a future bit's meaning round-trips it rather than erasing it.

```
04402b0006400a0002000600636c69656e740b40190002000f0064656c69766572795f706f6c69637901000200c1ff
```
