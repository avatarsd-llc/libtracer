# tlv-types/settings-reliability

SETTINGS{NAME "reliability", VALUE u8=1} — a named-knob SETTINGS record, as the library encoder emits one.

Since [RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
`reliability` is **not** a vertex `:settings` knob — it is bits 0–1 of the per-subscription
`delivery_policy` (§3.A), and a `:settings.reliability` write answers `SCHEMA_NOT_FOUND`. The
bytes are kept unchanged because what this vector pins is the SETTINGS record *shape* — a
`NAME` key followed by its `VALUE` — not the knob registry. For the vertex's actual knob set
see `settings/schema-enumerates-storage`.

```
0b40140002000b0072656c696162696c6974790100010001
```
