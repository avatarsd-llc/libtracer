# subscriber/target-as-value-refused

[05-protocol-tlvs.md](../../../../../../docs/reference/05-protocol-tlvs.md) §"Note on string
form vs PATH-TLV form", as corrected by the erratum of
[#1987](https://github.com/avatarsd-llc/libtracer/issues/1987): **a path has one wire form,
the PATH TLV.** The string form `"/client"` is an API spelling only. A `VALUE` child where a
SUBSCRIBER expects its `target_path` is not read as a path.

```
SUBSCRIBER (PL=1) {
  VALUE "/client"                    ; the target as a string, NOT a PATH
}
```

```
04400b00010007002f636c69656e74
```

The record names no `target_path`. Written to `<vertex>:subscribers[]` or
`<vertex>:subscribers[N]`, it is refused `ERROR{tr::schema::type_mismatch}` (`0x0030`): no
edge is admitted, and at `[N]` the slot keeps the subscriber it held.

A routed subscribe (a `FWD{WRITE}` to `:subscribers[]`) never needed a target: its delivery
rides the frame's return route. There the `VALUE` child is skipped like any unknown child, and
it does not move the delivery anywhere.

## Why this is an erratum, not a change

The text used to say "Implementations MUST accept either form where a path is expected". No
shipped reader ever did: `parse_subscriber_tlv` (`core/src/graph_fields.cpp`) takes only a
`PATH` child as the target, and the Rust and TypeScript bindings read and build only the PATH
form. This vector pins what already ships; no wire byte moves.

## What this vector gates, and where the behaviour is bound

Codec only, per [HARNESS.md](../../../../HARNESS.md): these 15 bytes are a well-formed TLV,
so every core round-trips them. The refusal is bound in `core/tests/qos_policy_test.cpp`,
`test_value_target_is_refused`: the append and the replace both answer `TYPE_MISMATCH`, a
later producer write delivers nothing new, and the positive control
`subscriber/policy-absent` (the same `/client` as a PATH) is admitted through the same door.
The bindings check that their SUBSCRIBER target reader finds no target in these bytes.
