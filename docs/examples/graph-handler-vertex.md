# A HANDLER vertex executes its write (L4 graph)

The vertex's **role** decides what a write means at that address. `STORED_VALUE` assigns;
`HANDLER` — roles 3–7 of
[reference 11](../reference/11-vertex-roles-and-aggregation.md) — runs the owner's `on_write`
instead, and its `on_read` supplies a value the graph never held: an MMIO register, a
computation, a device command. This example registers a relay whose write toggles the device
and whose read reports the device's state.

## What to notice

- **The role is invisible on the wire.** A peer sees one address with `read` and `write`,
  exactly as for a stored value; no role code is ever transmitted
  ([CONTEXT.md](../../CONTEXT.md) §Structural vertex, on why a role is never on the wire).
- **The seam allocation is keyed on presence, not on role.** A vertex bears a value-seam block
  iff a handler was installed at registration, so a `HANDLER` vertex registered with an empty
  `handlers_t` allocates none, and a `STORED_VALUE` vertex given an `on_children` does
  ([reference 02](../reference/02-graph-model.md) §Vertex lifecycle;
  `value_handlers_t` in `core/include/libtracer/vertex.hpp`).
- **`on_write` is where the device acts.** It receives the written rope and the writer's
  `write_ctx_t`; both are borrowed for the call only, so anything retained must be copied.
  The handler returns a `result_t<void>` — a refusal is the device's, not the graph's.
- **This is the sink shape a subscription delivers into.** Delivery *is* a write
  ([CONTEXT.md](../../CONTEXT.md) §SUBSCRIBER direction), so a handler vertex is what a
  spec-faithful `subscribe(src, target)` re-dispatches to — see
  [in-process pub/sub](in-process-pubsub.md), which wires exactly that.

## Source

```{literalinclude} /core/examples/graph_handler_vertex.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[vertex roles and aggregation](../reference/11-vertex-roles-and-aggregation.md) ·
[in-process pub/sub](in-process-pubsub.md).
