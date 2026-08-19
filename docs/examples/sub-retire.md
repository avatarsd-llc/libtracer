# Retire drops a producer's subscriptions (L4 graph)

`retire(vh)` marks a vertex and its whole subtree **logically absent** and re-virginizes
each one: the previous owner's `:acl`, value seam, stored value, history, app-field table,
`:subscribers[]`, owner-side storage declarations and delivery mode are all cleared, so a
later revive of the same address inherits nothing of the retired owner
([RFC-0009 §B](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md)).
This example subscribes both *on* a leaf and on its parent, retires the leaf, revives it,
and shows which edge survived.

## What to notice

- **Retirement delivers nothing and wakes no `await` (§B.5).** A subscriber learns a
  producer went away from the absence of writes, not from a retirement event — the example
  keeps an ancestor subscriber in place specifically to catch a spurious delivery.
- **Edges on an ancestor are untouched.** They belong to a vertex that was not retired, so
  the parent's subscription still observes the revived leaf's writes. The leaf's own edge
  does not come back.
- **The allocation is not freed.** A `vertex_handle_t` never dangles — the vertex map is
  pinned and insert-only
  ([ADR-0057](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0057-graph-composite-vertex-tree.md))
  — so the vertex is *emptied*, not erased. `retire_generation` is the stamp a cached
  resolution carries and re-reads: a mismatch means the path was retired, and possibly
  re-created for a different owner, since the resolution.
- **There is no wire operation that reaches here.** Retirement is owner-side; a peer goes
  through the device's own logic, which is what calls it. It is idempotent, and the root
  cannot be retired.
- **This is the only owner-side eviction that exists today.** ⚠️ The per-subscriber
  heartbeat described in
  [04 §Liveness loss](../reference/04-communication-flows.md) is a statement of *intent*:
  no `:liveness.*` field is implemented in either direction and no wire spelling for the
  refresh is ratified ([#586](https://github.com/avatarsd-llc/libtracer/issues/586)). The
  transport-level counterpart — evicting every edge that fanned out over a departed link,
  in one sweep, without retiring the target vertices (RFC-0009 §D) — is
  `graph_t::evict_link_edges`, which needs a live link and so is not shown here.

## Source

```{literalinclude} /core/examples/sub_retire.cpp
:language: cpp
:linenos:
```

See also: [graph model](../reference/02-graph-model.md) ·
[communication flows](../reference/04-communication-flows.md) ·
[graph module](../modules/graph.md).
