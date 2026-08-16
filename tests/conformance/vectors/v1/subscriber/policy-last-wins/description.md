# subscriber/policy-last-wins

The #995 family disposition on the RFC-0022 §3.A QoS SETTINGS — the
`delivery_policy` key three times, the last occurrence wrong-typed:

```
SUBSCRIBER (PL=1) {
  PATH (PL=0) { 06 "client" }
  SETTINGS (PL=1) {
    NAME "delivery_policy" VALUE u16=0x0001   ; well-formed — superseded below
    NAME "delivery_policy" VALUE u16=0x0021   ; well-formed — the LAST such: wins
    NAME "delivery_policy" NAME "nope"        ; wrong-typed — SKIPPED, not an error
  }
}
```

**Behavioural expectation:** the policy word reads `0x0021` (reliability=1,
durability_request=1). Two rules, each with a pre-#995 Rust divergence of the
opposite sign: on a repeated key the last **well-formed** occurrence wins (the
binding used to return the FIRST match), and a wrongly-typed value is skipped
with the earlier word kept (the binding used to reject the whole read). Both
mirror the C++ `parse_subscriber_tlv`, which reads through
`wire::config_reader_t` (#985).

The codec harness checks only the round-trip; the reader claim is bound per core
(see HARNESS.md): C++ `core/tests/qos_policy_test.cpp`, Rust
`bindings/rust/tests/conformance_vectors.rs`.

```
04405f0006400a0002000600636c69656e740b404d0002000f0064656c69766572795f706f6c69637901000200010002000f0064656c69766572795f706f6c69637901000200210002000f0064656c69766572795f706f6c696379020004006e6f7065
```
