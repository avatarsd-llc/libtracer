# settings/schema-enumerates-nothing

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.6 — the synthesized `:schema` protocol part, for a vertex named `temp` with no owner
field descriptor table:

```
POINT (PL=1) {
  NAME "temp"
  SETTINGS (PL=1) { }                ; the implemented protocol knobs: NONE
}
```

RFC-0010 §B.2 defines the synthesized part as "the implemented `settings.*` knobs". After
RFC-0022 §3.B deleted `settings_t` there are **none** — so the enumeration is empty, and
therefore for the first time **complete**. That is the condition
[#706](https://github.com/avatarsd-llc/libtracer/issues/706) was filed about (the schema
advertised `deadline_ns`, which nothing consumed, and omitted `store_ref_min_bytes`, which
everything did), dissolved by removing the inputs rather than by extending the view.

The empty `SETTINGS` is **emitted, not omitted**: the record keeps its shape, so a generic
renderer walks `POINT{ NAME, SETTINGS, [NAME "app" SETTINGS{…}] }` whatever the vertex
declares. The owner part (RFC-0010 §B.2) is appended after it when a descriptor table is
installed, exactly as before.

The two survivors of RFC-0022 — the STREAM ring depth and the store-by-reference threshold
— are absent here because they are **owner-side declarations with no wire surface at all**
(§3.C), not because they are undocumented.

```
07400c000200040074656d700b400000
```
