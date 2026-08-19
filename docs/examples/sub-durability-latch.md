# The delivery policy is per subscription (L4 graph)

`delivery_policy_t` is a packed 16-bit field describing **one producer→subscriber
relationship**, carried in the `SUBSCRIBER` TLV's `SETTINGS` child
([RFC-0022 §3.A](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)).
The host sugar takes the same bits, so the two doors stay byte-identical. This example puts
two subscribers on one producer that already holds a value, and differs them by one bit.

## What to notice

- **Bit 5, `durability_request`, is the transient-local latch.** Set, it delivers the
  producer's latched last value once at subscribe time; clear (the all-zero default), the
  join delivers nothing. Both edges take the *next* write either way — the example asserts
  that too, so a subscribe that silently failed to register could not pass.
- **The ablation is the point.** Both subscribers sit on the same producer, holding the same
  value, admitted through the same door, and they differ. Before RFC-0022 a single
  `settings.durability` knob on the *vertex* decided this for both, and this pair could not
  be expressed.
- **`reliability` (bits 0–1) and `priority` (bits 2–4) are stored and read back, not
  honoured.** They await the transport work that would consume them — the honest shape
  RFC-0022 §3.E chose over moving dead per-vertex fields. Do not read this example as
  showing a QoS engine; it shows the one bit that is live today.
- **Reserved bits are carried verbatim.** Bits 6–15 must be written zero and ignored on
  read, and a policy differing only there is not the same bytes.
- **Storage stayed on the vertex.** History depth and the pin ratio are declared owner-side
  through the host API and have no wire surface at all (§3.B–§3.D); the vertex's `:settings`
  core namespace answers `SCHEMA_NOT_FOUND` for every flat knob name.

## Source

```{literalinclude} /core/examples/sub_durability_latch.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[protocol TLVs](../reference/05-protocol-tlvs.md) ·
[communication flows](../reference/04-communication-flows.md).
