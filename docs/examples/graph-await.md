# `await` — the readiness plane (L4 graph)

The third data call. `await` blocks until the vertex's value is assigned again, or until the
deadline expires. It is **single-shot** and lives wholly in the state plane: it observes
assigns *at its own vertex* and takes no part in propagation
([reference 02](../reference/02-graph-model.md) §Assign, propagate, and the coalescing
sweep). The mental model is `epoll` on one identity — `read`/`write` are the data plane,
`:field` writes the control plane, `await` the readiness plane, all on one vertex
([CONTEXT.md](../../CONTEXT.md) §Field-write).

## What to notice

- **`await` is not subtree-scoped.** A subscription observes its vertex *and every
  descendant* (vertical bubbling,
  [RFC-0005](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md));
  `await` does not. The example writes a descendant first and the waiter stays parked — the
  write to the vertex itself is what wakes it.
- **A timeout is a distinct answer.** The second `await` expires and returns `TIMEOUT`, which
  is what lets a consumer distinguish a quiet vertex from a delivered value. An `await`
  carrying a `:field` selector is refused outright (`SCHEMA_NOT_FOUND`) rather than silently
  giving a whole-vertex wakeup — not exercised here, see
  [reference 02](../reference/02-graph-model.md) §Owner-declared application fields.
- **Ordering is the caller's.** The example sleeps 50 ms to let the waiter park, then
  `join()`s after the write, so every wake is complete and visible when the checks run. There
  is no stated ordering between a registration and a concurrent operation on the same address
  ([reference 02](../reference/02-graph-model.md) §Registration racing a concurrent operation)
  — a caller that needs one orders it itself.

## Source

```{literalinclude} /core/examples/graph_await.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[in-process pub/sub](in-process-pubsub.md) (the same primitive alongside subscriptions) ·
[concurrency and scaling](../reference/15-concurrency-and-scaling.md).
