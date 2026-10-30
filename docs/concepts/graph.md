# The graph

## In one paragraph

A node's state is a tree of vertices addressed by path, and the whole data API is three calls:
`read`, `write` and `await`. A write is two operations in one: `assign` replaces the vertex's
value, and `propagate` delivers the stored value along the vertex's subscription edges. An edge is
a `SUBSCRIBER` record held in the producer's `:subscribers[]` field and naming one target, so
subscribing is itself a write. A delivery lands at its target like a direct write and stops
there; it never relays onward. A queue exists only where a consumer asks for one, as its own
target vertex's STREAM ring. The same model holds whether the target is on this node or across a
link, because a remote delivery is a `FWD` write with the same bytes ([the wire format](wire.md)).

```{toctree}
:hidden:
:maxdepth: 1

Vertices <graph/vertex>
Propagate and FOLD <graph/propagate>
STREAM rings <graph/stream-ring>
Subscriptions <graph/subscribe>
```

## The picture

```{mermaid}
:caption: "Legend: purple = application, green = vertex, orange = ring."
flowchart LR
    accTitle: A write, its delivery, and where a queue lives
    accDescr: The producing application writes to vertex /sensor/temp, which assigns the value and propagates it along the edges held in its subscribers field. One edge delivers to a stored vertex on a consumer, which keeps only the latest value. Another delivers to a STREAM vertex whose ring the consumer sized. Each consumer application reads or awaits its own vertex.
    P([producer]):::app -->|"write = assign, propagate"| V("/sensor/temp"):::vertex
    V -->|"edge: deliver"| S("stored /ui/temp"):::vertex
    V -->|"edge: deliver"| Q("STREAM /log/temp"):::vertex
    Q --- R[("the consumer's ring")]:::ring
    S -->|"read, await"| C1([dashboard]):::app
    R -->|"history"| C2([logger]):::app
```

## When you need it

Always: every libtracer program registers vertices and writes to them. Read the children when you
choose a vertex's role, decide whether to coalesce or keep every sample, wire subscriptions
between nodes, or chase a value that arrived where you did not expect it.

## How it works

| Piece | What it does | Page |
| --- | --- | --- |
| vertex | a named position with a role, an optional value and `:` fields | [Vertices](graph/vertex.md) |
| `assign`, `propagate` | the two halves of a write; FOLD sends a subtree as one frame | [Propagate and FOLD](graph/propagate.md) |
| STREAM ring | the consumer's own bounded queue, sized in bytes by its own source | [STREAM rings](graph/stream-ring.md) |
| subscription | a `SUBSCRIBER` record written into `:subscribers[]` | [Subscriptions](graph/subscribe.md) |

**Data, readiness and control are three planes of one vertex.** `read` and `write` are the data
plane, `await` is readiness, and the `:` fields (`:subscribers`, `:acl`, `:settings`, `:schema`)
are the control plane, the vertex's `ioctl`. There is no `connect` or `subscribe` verb on the
wire.

**Every subscription is a subtree subscription.** An edge on a vertex sees writes to it and to
every descendant, carried as written. One edge on a parent covers a whole device.

**Delivery is terminal.** A delivery runs the target's store, its write ACL and its `await` wake,
and stops. Whatever the target's owner does next is a new write. That is why subscription cycles
cannot amplify and why a chain of edges does not relay.

**The producer never queues.** A write is the lock-free path at every depth, and the cost of a
slow consumer lands in that consumer's ring, not in the producer.

## Cost

Each child page carries its own numbers. In summary:

### Wire bytes

None of its own on one node. A remote delivery is one `FWD{WRITE}` per selected vertex, or one
branch-write frame per subtree with FOLD; a subscribe is one `FWD{WRITE}` of a `SUBSCRIBER`.

### Allocations

None on an idle write: with no listener, the delivery decision is two atomic loads and takes no
vertex lock. A local delivery clones a reference to the stored value, never its bytes. A publish
wider than `8 × kInlineFanout` subscribers draws one edge table of 48 bytes per subscriber.

### Latency

Not benched on this page. `bench_fanout_clone_storm` and `bench_subscribe_index` measure fan-out
and subscribe; their figures are on the generated performance page.

### Flash and RAM per target

| Target | Per vertex | Per edge |
| --- | --- | --- |
| NARROW | 64 B on 32-bit (`kMaxVertexBytes32`) | inline, `kInlineFanout` slots per vertex |
| MID | 64 B, the same 32-bit ratchet | same |
| WIDE | 88 B on 64-bit (`kMaxVertexBytes64`) | same; 48 B each past the inline width |

The vertex sizes are ratchets asserted in `vertex.hpp` on every build, pinned to the measured
size. A STREAM ring, a handler, app fields or an `:acl` add the vertex's cold extension block.

## What the library does not do

- It does not relay a delivery past its target, cap subscription depth, or deduplicate.
- It does not queue at the producer, pace a stream, or run a timer for any delivery.
- It does not tell a subscriber that a vertex went away. Disappearance is seen by re-reading.
- It does not compare stored bytes to decide whether to deliver.

## Pitfalls

- **Symptom:** a write to A never reaches C although A feeds B and B feeds C. **Cause:** delivery
  is terminal. **Fix:** subscribe C to A, or make B a handler that re-emits.
- **Symptom:** a subscriber sees fewer values than were written. **Cause:** its target is a stored
  vertex, which keeps the latest. **Fix:** make the target a STREAM vertex sized for the burst.

## Where the rules live

**Normative source:** [v1 §3](../spec/v1.md#3-wire-format), which incorporates
[reference 05](../reference/05-protocol-tlvs.md#0x04--subscriber) in full, including the data-API
semantics [v1 §1](../spec/v1.md#1-scope) promises: subscription, delivery, and the `:` field
surface.

**Exercised by:** `fwd-write-subscriber-field`, `fold-carries-composed-batch`, `create-child`,
`graph_test`, `subscribe_toward_test`.

Next concept: [Vertices](graph/vertex.md). Go deeper:
[reference 02](../reference/02-graph-model.md#assign-propagate-and-the-coalescing-sweep), then the
[graph module](../modules/graph.md), then
[reference 05 §0x04 SUBSCRIBER](../reference/05-protocol-tlvs.md#0x04--subscriber).
