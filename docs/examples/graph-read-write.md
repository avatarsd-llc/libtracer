# Read and write a vertex — the data plane (L4 graph)

Two of the three data calls ([CONTEXT.md](../../CONTEXT.md) §read / write / await), on a
`STORED_VALUE` vertex. The example writes twice and reads three times, which is enough to
pin down the storage contract: **one store per vertex, last-writer-wins**, and a read serves
the latest stored value — never behind a notification, legitimately newer
([reference 02](../reference/02-graph-model.md) §Subtree subscriptions, branch writes, and
write-creates).

## What to notice

- **A never-written vertex has no last-known-value.** It answers `NOT_FOUND`, the same status
  an address that does not resolve answers. The collapse is deliberate and documented
  ([reference 02](../reference/02-graph-model.md) §Retirement notification lists the three
  meanings `tr::path::not_found` carries); a consumer that must tell them apart carries its
  own expectation of what should exist.
- **`read` returns a reference, not a copy.** `value_ref_t` names the *published* value; the
  rope is not cloned link-by-link on the way out
  ([RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
  is a different concern — the reference-vs-copy rule and its measurement live on
  `vertex_t::value_ref_t` in `core/include/libtracer/vertex.hpp`). Holding one keeps that
  value alive, which the example checks: the first read still reads `21.5C` after the second
  write replaced the store.
- **The payload is opaque.** L4 never interprets a `VALUE`'s bytes — the example writes ASCII
  because it is legible, not because the graph knows what a temperature is.
- **A write is not a primitive.** It is `assign` (state) followed by `propagate` (edges),
  [RFC-0008](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0008-vertex-operations-assign-propagate.md).
  Nothing subscribes here, so only the `assign` half is observable.

## Source

```{literalinclude} /core/examples/graph_read_write.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) · [views](../modules/views.md) ·
[graph model reference](../reference/02-graph-model.md) ·
[communication flows](../reference/04-communication-flows.md).
