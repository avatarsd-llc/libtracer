# tlv-types/settings-reliability

SETTINGS{NAME "reliability", VALUE u8=1} — a named-knob SETTINGS record, as the library encoder emits one.

Since [RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
`reliability` is **not** a vertex `:settings` knob — it is bits 0–1 of the per-subscription
`delivery_policy` (§3.A), and a `:settings.reliability` write answers `SCHEMA_NOT_FOUND`, as does
every other name in the (now empty) core namespace. The bytes are kept unchanged because what
this vector pins is the SETTINGS record *shape* — a `NAME` key followed by its `VALUE` — not the
knob registry. For what a vertex's `:settings` and `:schema` actually serve now, see
`settings/read-container-shape` and `settings/schema-enumerates-nothing`.

```
0b40140002000b0072656c696162696c6974790100010001
```
