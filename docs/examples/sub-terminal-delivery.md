# Delivery terminates at the target (L4 graph)

`subscribe(src, target)` binds a **target vertex**: a write to `src` re-dispatches the
cloned value to `target`. What arrives there is an ordinary write — stored, gated by the
target's own `:acl`, waking `await`, indistinguishable from a direct write — and it
**stops there**
([RFC-0007 — delivery terminates at target](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0007-delivery-terminates-at-target.md),
[ADR-0051](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0051-delivery-terminates-at-target-no-dispatch-limits.md)).
This example wires `/a → /b`, puts an observer on `/b`'s own subscribers, writes `/a`, and
shows the observer never fires.

## What to notice

- **Chains do not relay.** With `A → B` and `B:subscribers → C`, a write to A does not
  reach C. Wire C directly to A — one subtree subscription already covers a whole subtree
  with one edge — or put a controller or handler at B whose *logic* re-emits.
- **A cycle is impossible by construction, not by a hop counter.** Because each delivery is
  store-only, a mutual `X ↔ Y` pair fires exactly one hop and stops; there is no dispatch
  depth limit to tune because there is no transitive dispatch to bound.
- **The target is subscription-unaware.** It does not learn which subscription, or that any
  subscription, produced the write — which is the same fact that makes provenance an
  application-data concern (see [one edge, a whole subtree](sub-subtree.md)).
- **Fan-in is the target's own gate.** Many subscriptions may fan into one target; they
  resolve per the target's role (overwrite for a stored value, append for a stream), and the
  target's `:acl` — not the source — decides who may write to it.

## Source

```{literalinclude} /core/examples/sub_terminal_delivery.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[communication flows](../reference/04-communication-flows.md) ·
[in-process pub/sub](in-process-pubsub.md).
