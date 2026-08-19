# Write-creates — a local write materializes its target (L4 graph)

A **local** data write to an address that does not resolve creates the vertex, and every
missing intermediate along the way, `mkdir -p` style
([RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md)
§D; `graph_t::ensure_vertex`). It is gated by the `CREATE` access bit on the nearest existing
ancestor's effective ACL — a graph holding no ancestor at all is open, which is why this
example needs no ACL setup.

## What to notice

- **The asymmetry is the ruling, and it is not shown here.** A **remote** fieldless
  `FWD{WRITE}` whose `dst` resolves to nothing answers `tr::path::not_found` and creates
  nothing (RFC-0005 §D amendment 1); a peer creates through the creator endpoint
  ([ADR-0059](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0059-creator-endpoint-creation-and-removal-are-writes-to-a-vertex.md)).
  Demonstrating that arm needs the wire resolver, so this example asserts only the local
  half; the remote half is pinned in `core/tests/op_resolve_test.cpp`, not here.
- **The `:` control plane does not create.** A field write to a nonexistent vertex is
  `NOT_FOUND`, because there is no vertex to control. The example writes `/zone/b:acl` and
  then confirms `/zone/b` still does not exist.
- **The created intermediate is real, and empty.** `/zone/a` resolves after the write but was
  never written itself; reading it serves the **composed branch read** of its registered
  subtree — the folded `POINT` tree, not a `NOT_FOUND`
  ([RFC-0016](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0016-composed-branch-read.md)).
- **Appearance is the first write.** Every way a vertex comes into being is itself a write, so
  a subtree subscriber above it sees the child appear without any dedicated event type
  ([reference 02](../reference/02-graph-model.md) §Observing structural change).

## Source

```{literalinclude} /core/examples/graph_write_creates.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[graph model reference](../reference/02-graph-model.md) §Vertex lifecycle ·
[retirement](graph-retire.md) (the other half of the lifecycle).
