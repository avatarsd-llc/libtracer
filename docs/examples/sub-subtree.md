# One edge, a whole subtree (L4 graph)

There is no separate "wildcard subscribe" verb — and no textual path wildcard, since `*`
may not appear in a NAME. **Every** subscription is a subtree subscription
([RFC-0005 — subtree subscriptions](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0005-subtree-subscriptions.md)):
an edge on `/dev` observes writes to `/dev` and to every descendant of it. This example
registers `/dev/a/temp` and `/dev/b/temp`, subscribes once on `/dev`, and writes both
leaves.

## What to notice

- **One `SUBSCRIBER` covers a composite.** Subscribing to a parent replaces one edge per
  leaf, which is the point on a node whose subscriber arena is measured in kilobytes.
- **Propagation is one hop plus upward bubbling.** A write fans out to the written vertex's
  own subscriptions plus each *ancestor's*, so the walk is strictly rootward and bounded by
  tree height — it never descends.
- **The delivered value is the descendant's write as-is.** The producer's frame arrives at
  its produced granularity; the subscription carries no "which leaf" field. Any provenance a
  consumer needs must travel **in the delivered data**
  ([RFC-0003](https://github.com/avatarsd-llc/libtracer/blob/main/docs/spec/rfcs/0003-bridged-wildcard-delivery-path.md)),
  never be inferred from the edge. Remote delivery is where that becomes a wire question;
  in-process it is the application's own encoding.
- **`own_subs` at the leaf is zero, and that is not "nobody is listening".** A subtree
  subscriber is counted by the ancestor bookkeeping (`listeners_above`), not by the leaf's
  own slot count — the example asserts both, because a producer that gated its publish on
  `own_subs` would silently drop every subtree subscriber. `has_subscribers` is the
  question to ask.

## Source

```{literalinclude} /core/examples/sub_subtree.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[graph model](../reference/02-graph-model.md) ·
[communication flows](../reference/04-communication-flows.md) ·
[subscribe to one vertex](sub-callback.md).
