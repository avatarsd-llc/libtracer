# settings/removed-knob

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.4 — writing a **removed** knob:

```
ERROR (PL=1) {
  VALUE u16 = 0x0031                       ; tr::schema::not_found (RFC-0002 registry)
  DESCRIPTION "settings.deadline_ns"       ; optional detail: the knob that resolved to nothing
}
```

RFC-0022 §4 removes `reliability`, `priority`, `deadline_ns` and `queue_max_bytes` from the
`:settings` knob grammar, and §3.B removes `durability` with them by reducing `settings_t` to
the two storage magnitudes. A write to **any** of the five now answers
`ERROR{tr::schema::not_found}` (status `SCHEMA_NOT_FOUND`) — the honest answer, and the one an
unsupported field already gives. There is no deprecation window: the protocol is DRAFT and
none of the five ever functioned.

The two survivors — `history_keep_last` and `store_ref_min_bytes` — still accept writes; see
`settings/schema-enumerates-storage` for the shape that enumerates them.

```
08401e000100020031000300140073657474696e67732e646561646c696e655f6e73
```
