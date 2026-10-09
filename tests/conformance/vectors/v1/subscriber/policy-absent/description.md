# subscriber/policy-absent

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.1 — the **absent** delivery policy. The `SUBSCRIBER`'s `SETTINGS` child exists and
carries only the **retired** `delivery_compact` key
([RFC-0032](../../../../../../docs/spec/rfcs/0032-delete-compact-streams-ride-the-chain.md) §6.2),
which a receiver ignores, so no `delivery_policy` is named:

```
SUBSCRIBER (PL=1) {
  PATH (PL=0) { 06 "client" }        ; the consumer's delivery target
  SETTINGS (PL=1) {
    NAME "delivery_compact" VALUE u8=0 ; RETIRED: a receiver ignores it
  }
}
```

Absent ⇒ **all-zero** ⇒ best-effort, default priority, **no durability request** — today's
behaviour, byte for byte. These are exactly the bytes a sender that predates RFC-0022
produces, which is the compatibility claim §3.A makes: the policy rides in the *existing*
child, so an old sender is a conforming sender.

A receiver MUST NOT read a neighbouring key as the policy: this vector fails any parser that
positionally assumes the SETTINGS child's members. It also pins RFC-0032 §6.2's pair-consuming
read: the retired key is skipped as a NAME/VALUE pair, never surfaced as a compaction opt-in.
[`compact-key-retired`](../compact-key-retired/description.md) is the same record with the key
set to `1`.

```
044028000600070006636c69656e740b4019000200100064656c69766572795f636f6d706163740100010000
```
