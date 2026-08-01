# subscriber/policy-durability

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.2 — a subscription **requesting durability**:

```
SUBSCRIBER (PL=1) {
  PATH (PL=1) { NAME "client" }
  SETTINGS (PL=1) {
    NAME "delivery_policy" VALUE u16=0x0020   ; bit 5 = durability_request
  }
}
```

The packed field (§3.A) decodes as `reliability=0` (bits 0–1), `priority=0` (bits 2–4),
`durability_request=1` (bit 5), reserved (bits 6–15) all zero.

**Behavioural expectation:** a producer holding a last-known value delivers it once,
immediately, to *this* subscriber at admission — and does **not** deliver it to a sibling
subscriber whose policy is absent or has bit 5 clear. Before RFC-0022 the producer's single
`:settings.durability` flag decided this for every subscriber at once, so the pair could not
be expressed. The latch carries no new wire bytes of its own (reference/05 §SUBSCRIBER); it
is observable as delivery *timing*, which is why the request is what this vector pins.

```
04402b0006400a0002000600636c69656e740b40190002000f0064656c69766572795f706f6c696379010002002000
```
