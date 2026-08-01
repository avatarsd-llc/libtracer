# settings/removed-knob

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.4 — writing a **removed** knob:

```
ERROR (PL=1) {
  VALUE u16 = 0x0031                       ; tr::schema::not_found (RFC-0002 registry)
  DESCRIPTION "settings.deadline_ns"       ; optional detail: the knob that resolved to nothing
}
```

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
never `tr::access::denied`.

```
08401e000100020031000300140073657474696e67732e646561646c696e655f6e73
```
