# `:children[]` — enumerate a parent's members (L4 graph)

The `:` control plane, read. `:children[]` is addressed **whole**: the read serves a
structured (`PL=1`) reply whose children are the parent's registered **members** — members,
not creation SPECs (the write-spec / read-members asymmetry,
[reference 05](../reference/05-protocol-tlvs.md) §SPEC). It is the enumeration half of
"observing structural change"; the other half is an ordinary subscription to the parent
([reference 02](../reference/02-graph-model.md) §Observing structural change).

## What to notice

- **One level, and each vertex answers for itself.** `/zone:children[]` lists `soil` and
  `air`; the grandchild `/zone/air/humidity` is enumerated from `/zone/air`, because a child
  is its own identity carrying its own facets ([CONTEXT.md](../../CONTEXT.md) §Schema).
- **A leaf enumerates an empty list, not an error.** An empty member list is a legitimate
  answer, in the same spirit as the bare `:settings` read serving an empty container.
- **A composed reply is a rope.** `:children[]` is synthesized rather than stored, so the
  reply may arrive as several links; the example calls `materialize()` before decoding —
  zero copy when single-link, one flatten copy otherwise
  ([ADR-0053](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0053-lazy-rope-backed-decode-view-partial-path-routing.md)
  §6).
- **This is not a notification.** The `:` plane is silent by design: no field read or write
  wakes `await` or propagates ([CONTEXT.md](../../CONTEXT.md) §Announce write). A consumer
  watching for structural change subscribes to the parent and re-enumerates — and a retired
  child simply stops appearing, with no tombstone
  ([reference 02](../reference/02-graph-model.md) §Retirement notification).

## Source

```{literalinclude} /core/examples/graph_children.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[addressing reference](../reference/03-addressing.md) ·
[vertex roles and aggregation](../reference/11-vertex-roles-and-aggregation.md).
