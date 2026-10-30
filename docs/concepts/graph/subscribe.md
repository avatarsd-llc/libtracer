# Subscriptions

## In one paragraph

A subscription is a record, not a call. Subscribing writes a `SUBSCRIBER` into the producer
vertex's `:subscribers[]` field; the record names one target, where every later write to the
producer, or to anything below it, is delivered. The edge lives on the producer and the target
knows nothing about it: a delivery looks exactly like a direct write. Over a link the same
subscribe is an ordinary `FWD{WRITE}`, and the producer keeps the request's accumulated return
route as the target, so later deliveries stream back with no hop holding state for the flow. The
source's ACL gates who may subscribe, the target's ACL gates who may write into it, and a
subscriber may ask, per subscription, for the current value at once when it joins.

## The picture

```{mermaid}
:caption: "Legend: purple = application, green = vertex, sky blue = record."
flowchart LR
    accTitle: A subscription is a SUBSCRIBER record held by the producer
    accDescr: The consumer writes a SUBSCRIBER record into the subscribers field of the producer's vertex /sensor/temp, gated by the source's READ right. Later writes to /sensor/temp are delivered to the record's target, gated by the target's WRITE right, and stop there.
    C([consumer]):::app -->|"write :subscribers[]"| R["SUBSCRIBER: target, policy"]:::record
    R -->|"held in"| V("/sensor/temp"):::vertex
    W([producer]):::app -->|"write"| V
    V -->|"deliver, target's WRITE gate"| T("target /ui/temp"):::vertex
```

## When you need it

Whenever a value should reach someone without them polling: a dashboard following a sensor, a
controller following its inputs, a node mirroring a subtree of another node. Use one edge on a
parent to follow a whole subtree. Ask for the latched value when a late joiner must paint the
current state before the next write.

## How it works

**The record.** A `SUBSCRIBER` is a structured record whose children are, in order, the target
`PATH`, an optional `SETTINGS` of per-subscription policy, an optional `ACL` capability and an
optional `NAME` identifier. A local subscribe names its target; a routed one may leave it out, and
the return route the request accumulated becomes the target.

**The host API.** `graph_t::subscribe(src, target)` writes the edge for a target vertex;
`subscribe(src, fn, ctx)` binds a callback instead and returns a `subscription_t` that
`unsubscribe` deactivates. On the wire both are field writes.

**Delivery policy.** One packed `u16` under the key `delivery_policy` in the record's `SETTINGS`.
Absent means all zero, the default. Bit 5, `durability_request`, is the one honoured today: the
subscribe itself delivers the producer's last value once, before any new write. Reliability,
priority and delivery class are carried and read back; the bit layout is in
[reference 02 §Subscriber delivery policy](../../reference/02-graph-model.md#subscriber-delivery-policy).

**Subtree and terminal.** An edge on V observes writes to V and every descendant, carried as
written, once per subscriber. Each delivery runs the target's store, its write ACL and its
`await` wake, and stops: it is never re-dispatched to the target's own subscribers. Several edges
may fan into one target; the target's role resolves them, overwrite for a stored vertex and
append for a STREAM.

**Remote edges.** For a subscribe that crossed hops, every forwarding hop adds its element to the
request's reverse-route child, and the producer stores that chain with the edge. Each delivery is
then a `FWD{WRITE}` with an empty `src`, addressed by the chain, shrinking one element per hop.
When the link the edge was admitted over goes down, the edges bound to it are evicted, and the
vertices stay.

## Worked example

A remote subscribe: a `FWD{WRITE}` to `/sensor/temp`'s `:subscribers[]`, appending a `SUBSCRIBER`
whose target is the requester's `/reply-ep`.

```{hex-walk} fwd/fwd-write-subscriber-field
:caption: Subscribing is a field write.
u8: op, 1 = WRITE
path: dst, the producer vertex
str: the field, subscribers
u8: index mode, 1 = ELEMENT, an append
path: src, the route back
path: the subscription's target
```

The same record with a `SETTINGS` child asking for the latched value on join, `delivery_policy`
`0x0020`:

```{hex-walk} subscriber/policy-durability
:caption: A SUBSCRIBER that asks for the current value when it joins.
path: target
str: the policy key
u16: delivery_policy, bit 5 = durability_request
```

## Cost

### Wire bytes

A subscribe is one `FWD{WRITE}`, 79 bytes in the example above, plus 29 more bytes of `SETTINGS`
when a policy is given. Each delivery is one `FWD{WRITE}` with an empty `src`: its envelope does
not grow with the value, so a 1 KiB or 16 KiB delivery pays the same addressing bytes as a 4-byte
one, and a PAIR chain saves bytes at every hop it replaces a mount run.

### Allocations

The return route is copied once, at subscribe, into a reference-counted segment the edge holds.
Each local delivery clones a reference to the value, never its bytes. With no subscriber in a
subtree, a write's delivery decision is two atomic loads. A vertex holds `kInlineFanout` edges
inline (8 by default, 2 on the ESP-IDF component); a publish to more than `8 × kInlineFanout`
subscribers draws one table of 48 bytes per subscriber on a 64-bit host.

### Latency

Not benched on this page. `bench_subscribe_index` times subscribe and lookup as edges grow,
`bench_fanout_clone_storm` times fan-out, and `bench_chain_delivery_pair` prices remote delivery
over a PAIR chain at 1 and 3 hops.

### Flash and RAM per target

| Target | RAM per edge | Note |
| --- | --- | --- |
| NARROW | inline slots, `kInlineFanout` per vertex | no per-subscriber buffer or queue anywhere |
| MID | about 430 B per subscription edge, measured on an ESP32 node | [reference 22 §3.1](../../reference/22-backpressure-and-sizing.md#31-bounded-mcu-node-esp32-class-tens-of-kb-heap) |
| WIDE | inline slots, then 48 B per subscriber in a publish snapshot | past `8 × kInlineFanout` subscribers |

## What the library does not do

- It has no `subscribe` verb on the wire and no connection to open first.
- It does not filter by value, throttle, or keep alive per subscription; those belong to the
  producer's cadence or to a filter vertex.
- It does not relay a delivery onward from its target.
- It does not tell a subscriber that a watched vertex was retired.

## Pitfalls

- **Symptom:** a subscribe answers `tr::schema::type_mismatch`. **Cause:** a local subscribe gave
  no target `PATH`. **Fix:** name the target; only a routed subscribe may leave it out.
- **Symptom:** a late joiner shows nothing until the next write. **Cause:** it did not set
  `durability_request`. **Fix:** set bit 5 of `delivery_policy` in its record.
- **Symptom:** subscriptions vanish when a peer reconnects. **Cause:** edges bound to a link are
  evicted when it goes down. **Fix:** resubscribe on link up.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates
[reference 05 §0x04 SUBSCRIBER](../../reference/05-protocol-tlvs.md#0x04--subscriber), its
[producer fan-out](../../reference/05-protocol-tlvs.md#producer-fan-out-to-remote-subscribers)
and [subtree delivery](../../reference/05-protocol-tlvs.md#subtree-subscription--vertical-bubbling).

**Exercised by:** `fwd-write-subscriber-field`, `policy-durability`, `policy-absent`,
`no-target`, `subscribe_toward_test`, `subscription_selector_test`.

Next concept: [Batching](../batching.md). Go deeper:
[reference 04 §Subscribe via field-write](../../reference/04-communication-flows.md#subscribe-via-field-write),
then the [graph module](../../modules/graph.md), then
[reference 05 §0x04 SUBSCRIBER](../../reference/05-protocol-tlvs.md#0x04--subscriber).
