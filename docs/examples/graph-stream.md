# A STREAM vertex and its bounded history ring (L4 graph)

The `STORED_VALUE` contract is "the latest": *k* writes leave one value. A `STREAM` vertex
additionally appends each write to a bounded ring, and its contract is "observe every
buffered entry" — which is why its propagation is a **drain** of the entries appended since
the previous flush, not a coalesced last-writer-wins flush
([reference 02](../reference/02-graph-model.md) §Stream drain semantics).

## What to notice

- **The depth has no wire surface at all.** It is a *retention intent* only the application
  can supply, so it is declared owner-side with `set_history_depth` — not a `:settings` knob.
  The withdrawn `history_keep_last` answers `SCHEMA_NOT_FOUND` on read and on write,
  caller-independently
  ([RFC-0022](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0022-delivery-policy-is-per-subscription-vertex-keeps-storage.md)
  §3.B / §3.C).
- **Nothing is inherited.** A declaration reaches exactly the vertex it names — no ancestor
  walk, no subtree push, no propagation question when a parent is reconfigured after its
  children exist (§3.F).
- **The ring is drain-only, and indexing never reaches it.** `[n]` counts children of the
  *stored value*, never entries of the history ring ([CONTEXT.md](../../CONTEXT.md) §Element
  addressing). The example reads the ring through `history()` and separately checks that a
  plain `read` still serves the latest value.
- **Overflow is not silent, in general.** Shedding under pressure carries the flow-gap signal
  and is accounted for
  ([RFC-0025](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0025-stream-class-values.md)
  §4.5). What this example shows is the ordinary *trim* to the declared depth on a local
  producer, which is the retention intent doing its job — not a receiver-visible gap.

## Source

```{literalinclude} /core/examples/graph_stream.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[vertex roles and aggregation](../reference/11-vertex-roles-and-aggregation.md) ·
[user data packing](../reference/06-user-data-packing.md).
