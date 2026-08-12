# acl/denied-caller-undeclared-app-field

[RFC-0010](../../../../../../docs/spec/rfcs/0010-owner-app-fields-and-schema.md) §Erratum
(2026-08-12, [#435](https://github.com/avatarsd-llc/libtracer/issues/435)) — a caller the
vertex ACL denies, reading **or** writing a `settings.app.` spelling that the owner never
declared:

```
ERROR (PL=1) {
  VALUE u16 = 0x0050                       ; tr::access::denied (RFC-0002 registry)
}
```

These are the bytes the **golden core actually builds** (`assemble_error` in
`core/src/op_resolve_walk.hpp`) — the registered-code identity child is the whole error, and
the reply does not echo the field name.

The point of the vector is **which** of the two registered codes answers, per namespace,
identically on read and on write:

| namespace | is the name set a secret? | denied caller, nonexistent name | denied caller, existent facet |
| --- | --- | --- | --- |
| protocol-owned (`{subscribers, acl, children, settings, schema, identity}`, withdrawn flat knobs) | no — published spec text | `ERROR{tr::schema::not_found}` (`0x0031`, byte-identical to [`settings/removed-knob`](../../settings/removed-knob/description.md)) | `ERROR{tr::access::denied}` (`0x0050`) |
| owner-defined (`settings.app.*`) | **yes** — owner-declared, per-node | **`ERROR{tr::access::denied}` — these bytes** | `ERROR{tr::access::denied}` — the same bytes |

For the owner namespace the ACL gate is evaluated **before** any name resolution
(gate-before-resolve), so a denied caller's reply is uniform over declared, undeclared,
`ro` and `wo` spellings: the error channel discloses neither the owner's field names nor
which spellings exist. `SCHEMA_NOT_FOUND` for an undeclared app field is the answer a
caller the ACL **admits** receives (RFC-0010 §A.2/§A.3 as corrected). This generalizes the
anti-leak stance [reference/05](../../../../../../docs/reference/05-protocol-tlvs.md)
§Gating `:identity` already records — a caller-dependent `PERMISSION_DENIED` /
`SCHEMA_NOT_FOUND` split on one spelling leaks through the error code — and walks back the
[#430](https://github.com/avatarsd-llc/libtracer/issues/430) write-side hoist exactly where
its own justification ("knob names are a fixed, published constant of the protocol") does
not reach.

**Behavioural binding** (see [`../../../HARNESS.md`](../../../HARNESS.md) § *What a vector
gates*): `core/tests/acl_test.cpp` — `test_denied_caller_disclosure_parity` drives both
doors at three callers over both namespaces, plus the existent-surface controls
(`:schema`, bare `:settings`, a declared field — all still `PERMISSION_DENIED` for the
denied caller; `:identity` still pre-auth).

```
08400600010002005000
```
