# Subscribe to one vertex (L4 graph)

The smallest subscription there is: one producer, one in-process callback, and the
delivery contract that follows from it. `subscribe(src, callable)` returns a
`subscription_t` handle; every write to `src` then invokes the callback with the written
[rope](../modules/views.md) value.

## What to notice

- **`subscribe` is host-SDK sugar, not a wire verb.** The wire data API stays
  read/write/await
  ([ADR-0006](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0006-read-write-await-api-no-connect.md));
  a subscription is a consumer-initiated `SUBSCRIBER` field-write into the producer's
  `:subscribers[]`
  ([ADR-0026](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0026-consumer-initiated-subscription-client-write.md)).
  The callback overload enters the same single admission door as that field-write —
  SUBSCRIBE gate, append, durability latch
  ([ADR-0049](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0049-field-write-single-subscriber-admission-door.md))
  — it merely skips the parse a callback cannot ride.
- **Delivery is inline, on the writing thread.** The callback runs inside `write()`, once
  per write; nothing is queued and no delivery thread exists. A slow callback is a slow
  `write`.
- **The callable is bound by address, and it *is* the edge's `ctx`.** The templated
  overload takes an lvalue reference (a temporary would dangle) and forwards
  `{fn, &callable}` to the `subscriber_fn_t` form — a plain function-pointer pair, so the
  per-publish edge snapshot is a trivial copy rather than a `std::function` clone. How long
  that address must stay alive is the subject of
  [unsubscribe & the release hook](sub-unsubscribe.md).
- **`own_subs` counts the vertex's OWN slots.** It is a sizing figure, not an
  "is anyone listening" test — subscribers on an ancestor are not in it (see
  [one edge, a whole subtree](sub-subtree.md)), and `has_subscribers` is the question a
  producer should ask.

## Source

```{literalinclude} /core/examples/sub_callback.cpp
:language: cpp
:linenos:
```

See also: [graph module](../modules/graph.md) ·
[communication flows](../reference/04-communication-flows.md) ·
[in-process pub/sub](in-process-pubsub.md).
