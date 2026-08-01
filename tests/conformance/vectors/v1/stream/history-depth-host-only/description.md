# stream/history-depth-host-only

[RFC-0022](../../../../../../docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
§5.7 — the STREAM ring depth is **owner-side state with no wire surface at all**. The bytes
below are the answer to a read OR a write of the name it used to live under:

```
ERROR (PL=1) {
  VALUE u16 = 0x0031                          ; tr::schema::not_found (RFC-0002 registry)
}
```

These are the bytes the **golden core actually builds** (`assemble_error` in
`core/src/op_resolve_walk.hpp`): the registered-code identity child is the whole error, and no
code path anywhere in the core emits a `DESCRIPTION` detail child. So this vector is
byte-identical to [`settings/removed-knob`](../../settings/removed-knob/description.md) — the
ring depth is refused exactly as every other withdrawn knob is, and the reply gives a peer no
way to tell the two names apart.

**Behavioural expectations pinned by this vector** (§3.C):

- `graph_t::set_history_depth(vertex, keep)` changes how many entries the STREAM ring
  retains — the next append trims to the new depth;
- **no wire operation can read or write it.** A `:settings.history_keep_last` write answers
  the ERROR above; the same read answers the same code; and a bare `:settings` read does not
  carry the value either (see `settings/read-container-shape`);
- the answer is **caller-independent** — a peer holding the vertex WRITE right gets exactly
  the same `tr::schema::not_found`, because the name resolves to nothing before any gate, on
  the read door as on the write door.

This is the distinction the RFC-0022 removal rests on: what is withdrawn is the **remote
write surface**, not owner-side configuration. The ring depth encodes an application's
retention intent, which no peer and no injected resource can supply — so it is declared
host-side, in the shape of `set_delivery_mode` and `set_app_fields`, and it costs a STREAM
vertex zero additional bytes because a STREAM identity already allocates the extension block.

**Behavioural binding** (see [`../../../HARNESS.md`](../../../HARNESS.md) § *What a vector
gates*): `core/tests/qos_policy_test.cpp` — `test_history_depth_is_host_only` drives
`set_history_depth` and both wire halves, and `test_removed_knob_reply_bytes` byte-compares the
reply the resolver builds for this name against these bytes.

```
08400600010002003100
```
