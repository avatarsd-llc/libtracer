# Retirement — leaving the graph empties a vertex, it does not erase it (L4 graph)

`retire` marks a vertex and its whole subtree **logically absent**
([RFC-0009](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0009-vertex-removal-and-subscriber-eviction.md)
§A.1 / §B): the path then reads `NOT_FOUND`, identical to never-existed. There is **no
distinct `retired` status** — no such code is allocated, and the collapse is an accepted cost
rather than an oversight ([reference 02](../reference/02-graph-model.md) §Retirement
notification).

## What to notice

- **The object is not freed.** The vertex map is pinned and insert-only
  ([ADR-0057](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0057-graph-composite-vertex-tree.md)),
  so an outstanding `vertex_handle_t` stays dereferenceable after the retirement — the
  example keeps using it. What tells a holder its cached resolution went stale is
  `retire_generation`, bumped by the retirement
  ([ADR-0062](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0062-resolve-once-label-bindings-hold-resolutions-not-names.md)).
  A generation match says the vertex is the same one; it never says the caller may still act
  on it, so an authorization decision must not be cached this way.
- **Retirement takes the subtree.** `/zone/air/humidity` goes with `/zone/air`.
- **It is idempotent and silent.** Retiring an already-retired vertex succeeds and does
  nothing; the retirement delivers along no edge and wakes no `await` (§B.5), which is why
  disappearance is observable only by polling.
- **Revival inherits nothing.** A later local write-creates revives the address, and the
  revived vertex takes its *live ancestor's* ACL policy, never the retired owner's (§B.6) — a
  stale grant cannot outlive the retirement.
- **`collect()` is the embedder's, and is not needed here.** A retired vertex parks its
  detached value seam iff a handler was installed at registration; these vertices carry none,
  so nothing is parked. A bus node with peer churn must call `graph_t::collect()`, and
  `graph_t::parked_seam_count()` makes an uncollected park observable
  ([reference 02](../reference/02-graph-model.md) §Vertex lifecycle).

## Source

```{literalinclude} /core/examples/graph_retire.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[write-creates](graph-write-creates.md) (the other half of the lifecycle) ·
[graph model reference](../reference/02-graph-model.md).
