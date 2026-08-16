# subscriber/policy-absent

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.1 — the **absent** delivery policy. The `SUBSCRIBER`'s `SETTINGS` child exists and
carries only the pre-existing `delivery_compact` key, so no `delivery_policy` is named:

```
SUBSCRIBER (PL=1) {
  PATH (PL=0) { 06 "client" }        ; the consumer's delivery target
  SETTINGS (PL=1) {
    NAME "delivery_compact" VALUE u8=0 ; the only key a pre-RFC-0022 sender emitted
  }
}
```

Absent ⇒ **all-zero** ⇒ best-effort, default priority, **no durability request** — today's
behaviour, byte for byte. These are exactly the bytes a sender that predates RFC-0022
produces, which is the compatibility claim §3.A makes: the policy rides in the *existing*
child, so an old sender is a conforming sender.

A receiver MUST NOT read a neighbouring key as the policy: this vector fails any parser that
positionally assumes the SETTINGS child's members.

```
04402b0006400a0002000600636c69656e740b4019000200100064656c69766572795f636f6d706163740100010000
```
