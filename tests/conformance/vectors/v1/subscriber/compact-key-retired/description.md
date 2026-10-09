# subscriber/compact-key-retired

[RFC-0032](../../../../../../docs/spec/rfcs/0032-delete-compact-streams-ride-the-chain.md) §6.2
and §12.2: the subscription an older consumer sends when it asks for `COMPACT` delivery. The
`SETTINGS` child carries `delivery_compact = 1` and no `delivery_policy`:

```
SUBSCRIBER (PL=1) {
  PATH (PL=0) { 06 "client" }          ; the consumer's delivery target
  SETTINGS (PL=1) {
    NAME "delivery_compact" VALUE u8=1   ; RETIRED: a receiver ignores it
  }
}
```

```
044028000600070006636C69656E740B4019000200100064656C69766572795F636F6D706163740100010001
```

The key is retired. A receiver admits the subscription, reads the absent `delivery_policy` as
all-zero, and serves it with ordinary `FWD{WRITE}` deliveries over the chain. No reader surfaces
a compaction opt-in. A `:subscribers[N]` read returns the record with the retired key as written.
This vector differs from [`policy-absent`](../policy-absent/description.md) only in the key's
value (`1` against `0`).

**Behavioural binding:**
- C++: `core/tests/qos_policy_test.cpp` (§5 vectors) admits these bytes through the
  `:subscribers[]` door with no latch and an all-zero policy, and reads them back byte-identical
  through `:subscribers[0]`. `core/tests/fwd_fanout_test.cpp`
  (`test_retired_compact_opt_in_is_unknown`) subscribes a REMOTE consumer with the key set and
  asserts chain `FWD{WRITE}` deliveries with an empty `src`, including a write to a child of
  the subscribed vertex.
- Rust: `bindings/rust/tests/conformance_vectors.rs` (`subscriber_compact_key_retired`).
- TypeScript: `bindings/typescript/packages/client/test/vectors.test.mjs`.
