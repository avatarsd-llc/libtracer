# In-process pub/sub (L4 graph)

The smallest complete libtracer node: a single-process [graph](../modules/graph.md) with
no transport and no wire bytes. A publisher writes `/sensor/temp`; three subscribers each
receive the value a different way:

1. a **direct in-process callback** — the `subscribe(src, callback)` sugar;
2. a **spec-faithful target vertex** — `subscribe(src, target)` re-dispatches to a
   handler-backed sink vertex;
3. a **thread blocking in `await()`** — the single-shot readiness primitive.

Delivery to (1) and (2) is a refcount-bump clone of the *same* [rope](../modules/views.md)
value — no byte copy. The example finishes by reading the last-known-value back and
field-writing a QoS setting, then discovering it via the `:schema` control read.

## What to notice

- **Handles, not strings, on the hot path** — the path is encoded once via the parse-once
  `path_t("…")` constructor; `register_vertex` returns a `vertex_t*` reused for every op.
- **`await` is join-safe** — the publisher `join()`s the waiter *after* the write, so every
  delivery is complete and visible when the checks run.
- **It self-checks** — each delivery path is asserted, so the ctest smoke test guards
  *behavior*, not just a clean exit.

## Source

```{literalinclude} /core/examples/in_process_pubsub.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) · [views](../modules/views.md) ·
[path](../modules/path.md).
