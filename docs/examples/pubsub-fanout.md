# Pub/sub fan-out & dispatch cost (L4 graph)

One publisher, a growing set of subscribers, and a question: **what does each extra
subscriber cost?** This example registers one `/sensor/temp`
[vertex](../modules/graph.md), then for each fan-out width in {1, 8, 64} attaches
that many callbacks, writes 5,000 values, and reports the per-delivery latency and
delivery throughput. Every `write` fans out to every subscriber as a refcount-bump
clone of the *same* [rope](../modules/views.md) value — **no byte copy per subscriber**.

## What to notice

- **Fan-out is amortized, not multiplied** — per-*delivery* cost *drops* as fan-out grows
  (the per-write overhead is shared across more subscribers); the `RESULT` lines make the
  trend visible.
- **The value is cloned by refcount, not by bytes** — 64 subscribers means 64 refcount
  bumps on one segment, not 64 copies.
- **`:schema` discovery** — after a `:settings.deadline_ns` field-write, the vertex's shape
  is read back structurally as a 2-child `POINT`.
- **The `RESULT` line is informational** — timing never fails CI; the self-checks assert
  every subscriber saw every write.

```{note}
The absolute nanoseconds come from whatever build ran (CI builds the examples in a debug
configuration); treat them as a *shape*, not a spec number. The canonical, release-build,
CI-published figures live on the [performance page](../performance.md).
```

## Source

```{literalinclude} /core/examples/pubsub_fanout.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) · [in-process pub/sub](in-process-pubsub.md) ·
[performance](../performance.md).
