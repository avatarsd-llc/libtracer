# settings/removed-knob

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.4 — writing a **removed** knob:

```
ERROR (PL=1) {
  VALUE u16 = 0x0031                       ; tr::schema::not_found (RFC-0002 registry)
}
```

These are the bytes the **golden core actually builds** (`assemble_error` in
`core/src/op_resolve_walk.hpp`), not a declared shape: the registered-code identity child is the
whole error. The reply does **not** echo the unresolved knob name — `type_t::DESCRIPTION` has no
producer anywhere in the core — so this vector is byte-identical to
[`stream/history-depth-host-only`](../../stream/history-depth-host-only/description.md). That is
the point: the answer carries no per-knob detail, so a peer cannot tell from the reply *which*
withdrawn name it asked for.

RFC-0022 §3.B deletes `settings_t` **outright**, so the vertex's `:settings` core namespace is
empty and a write to **any** of the seven historical names answers
`ERROR{tr::schema::not_found}` (status `SCHEMA_NOT_FOUND`) — the honest answer, and the one an
unsupported field already gives:

| name | why it is gone |
| --- | --- |
| `reliability`, `priority`, `durability` | they describe one producer→subscriber **relationship**, and moved to the subscription's packed `delivery_policy_t` (§3.A) |
| `deadline_ns`, `queue_max_bytes` | inert *and* without a coherent per-vertex meaning — deleted, not moved (§3.E) |
| `history_keep_last` | an application **retention intent**: owner-side vertex state now (`graph_t::set_history_depth`, §3.C) |
| `store_ref_min_bytes` | a deployment **copy/pin trade**: owner-side too, and §3.D replaces the predicate itself once §6's dual-target measurement lands |

There is no deprecation window: the protocol is DRAFT, and of the seven only three ever
functioned — none of them as remotely-writable QoS.

The answer is **caller-independent**. An unknown core-namespace NAME resolves to nothing
before any ACL gate (docs/reference/05 §`0x0B` validation, a rule stated with no caller
qualifier), so a caller the vertex `:acl` would deny still sees `tr::schema::not_found` and
never `tr::access::denied`. Both the write door and the read door resolve the name first, so
one name never answers two different codes depending on who asked.

**Behavioural binding** (see [`../../../HARNESS.md`](../../../HARNESS.md) § *What a vector
gates*): `core/tests/qos_policy_test.cpp` — `test_removed_knob_reply_bytes` resolves a real
`FWD{WRITE, :settings.deadline_ns}` through `op_resolver_t` and byte-compares the reply's
`ERROR` child against these bytes; `core/tests/acl_test.cpp` —
`test_flat_knob_surface_is_withdrawn` pins the caller-independence, on read and on write.

```
08400600010002003100
```
