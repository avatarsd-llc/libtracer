# settings/delivery-mode-not-found

[RFC-0008](../../../../../../docs/spec/rfcs/0008-vertex-operations-assign-propagate.md) §C, as
corrected by the 2026-10-08 erratum ([#1981](https://github.com/avatarsd-llc/libtracer/issues/1981)):
the per-vertex `delivery_mode` is **owner-side state with no wire spelling**. The bytes below
are the answer to a write OR a read of `:settings.delivery_mode`:

```
ERROR (PL=1) {
  VALUE u16 = 0x0031                          ; tr::schema::not_found (RFC-0002 registry)
}
```

These are the bytes the **golden core actually builds** (`assemble_error` in
`core/src/op_resolve_walk.hpp`), so this vector is byte-identical to
[`settings/removed-knob`](../removed-knob/description.md). RFC-0008 §C once called a
`delivery_mode` NAME/VALUE under the vertex `SETTINGS` a **deferred** wire spelling. No
implementation ever shipped it, and
[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§3.B then emptied the vertex's `:settings` core namespace, so the name resolves to nothing,
exactly as every withdrawn knob does.

**Behavioural expectations pinned by this vector:**

- `graph_t::set_policy(vertex, {.delivery_mode = ...})` sets the mode host-side;
- **no wire operation can read or write it.** A `:settings.delivery_mode` write answers the
  ERROR above and leaves the mode unchanged; the same read answers the same code; and a bare
  `:settings` read does not carry the value (see `settings/read-container-shape`).

**Behavioural binding** (see [`../../../HARNESS.md`](../../../HARNESS.md) § *What a vector
gates*): `core/tests/qos_policy_test.cpp` — `test_delivery_mode_has_no_wire_spelling` drives
`vertex_policy_t::delivery_mode` and both wire halves, and `test_removed_knob_reply_bytes`
byte-compares the reply the resolver builds for this name against these bytes.

```
08400600010002003100
```
