# Concurrency

## In one paragraph

The wire protocol says nothing about threads, and a node may run on one. When a node does run on
several, five obligations fall out of the operation semantics: a write is atomic with respect to
reads of that vertex, a value handed to a reader stays valid while it is held, every assign
advances the vertex's write sequence and reaches each subscriber in assign order, registration
may happen at any time, and refcounts use at least acquire and release ordering. Everything else
is left to the implementation. This one starts no threads of its own: delivery runs on the
writer's thread, a socket link runs its own receive thread, and the context a subscription holds
is freed only after every delivery that could still name it has ended.

```{toctree}
:hidden:
:maxdepth: 1

Who runs where <concurrency/threads>
Reclamation <concurrency/reclamation>
Ordering <concurrency/ordering>
```

## The picture

```{mermaid}
:caption: "Legend: purple = application, vermillion = peer, green = vertex, grey = not the library."
flowchart LR
    accTitle: The threads that touch a graph
    accDescr: An application thread writes a vertex and the delivery to its subscriber runs on that same thread. A peer's frame arrives over a link on the link's receive thread, which writes into the graph and so delivers on the receive thread. The graph and the router start no threads of their own; the threads belong to the application and to the links.
    A([application thread]):::app -->|"write"| V("vertex"):::vertex
    P[[peer]]:::peer ==>|"link"| R["link receive thread"]:::notlib
    R -->|"write"| V
    V -->|"delivery, on the writer's thread"| S("subscriber target"):::vertex
```

## When you need it

As soon as two threads touch one graph: a receive thread and the application, two links, or a
host that shards work across cores. A single-threaded node on bare metal needs only the
reclamation choice on [Reclamation](concurrency/reclamation.md).

## How it works

| Piece | What it answers | Page |
| --- | --- | --- |
| threads | which thread runs a delivery, a link, a control-plane change | [Who runs where](concurrency/threads.md) |
| reclamation | when a subscription's context or a parked value may be freed | [Reclamation](concurrency/reclamation.md) |
| ordering | what a reader or subscriber sees, and in which order | [Ordering](concurrency/ordering.md) |

**Disjoint work scales; a hot vertex does not.** Threads writing different vertices scale close
to the core count. Every thread reading or writing one vertex serializes on its cache line, and
an owning read is itself a write to the refcount. Design hot vertices out, or shard them.

**No timers.** Nothing here waits on a clock. A grace period ends when the last delivery that
could name a pair ends, never after a delay.

## Cost

### Wire bytes

None. Concurrency is a property of one node; nothing about it travels.

### Allocations

None per operation. A deferred release waits in one shared table of `kDeferredReleaseSlots`
entries, 16 by default, fixed at build time.

### Latency

On the machine of reference 15 §3, disjoint work reached about 9.6 times one thread at 24
threads, and one contended `fetch_add` plateaued at 14.25 ns per operation. The reclamation
bracket adds about 10 ns to a WIDE publish; see [Reclamation](concurrency/reclamation.md).

### Flash and RAM per target

| Target | Reclamation | Per-thread state |
| --- | --- | --- |
| NARROW | `reclaim_strict_t` on one core without preemption, else QSBR | none under strict |
| MID | `reclaim_qsbr_t` | 64 B per FreeRTOS task |
| WIDE | `reclaim_qsbr_t` | 16 B per thread |

## What the library does not do

- It does not start threads in the graph or the router.
- It does not order concurrent writes to different vertices.
- It does not order a registration against a racing operation on the same address.
- It does not read a clock to end a grace period.

## Pitfalls

- **Symptom:** adding threads makes one counter slower. **Cause:** every thread writes the same
  vertex. **Fix:** give each thread its own vertex and combine them on read.
- **Symptom:** an unsubscribe answers `BACKPRESSURE`. **Cause:** the deferred-release table is
  full. **Fix:** raise `kDeferredReleaseSlots`, or shard subscriptions per receive thread.

## Where the rules live

**Normative source:** [v1 §1](../spec/v1.md#1-scope). The protocol defines frames and operations
only; how a node runs threads is not normative.

**Exercised by:** `write_seq_test`, `seq_latch_test`, `reclaim_test`, `net_lock_order_test`,
`delivery_mode_race_test`.

[Reference 15 §2](../reference/15-concurrency-and-scaling.md#2-what-the-protocol-requires)
derives the five obligations from the operation semantics; `bench_contention` measures the four
scaling regimes.

## History

:::{dropdown} How the concurrency model got here
- [ADR-0075](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0075-a-vertexs-edges-are-published-and-read-under-an-edge-pin.md)
  made a write snapshot its edges under a pin and dispatch outside every lock.
- [ADR-0080](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0080-reclamation-policy-is-a-build-time-closed-per-target-seam.md)
  made reclamation a build-time policy; Amendment 1, 2026-10-12, made QSBR the one grace
  mechanism and the default.
:::

Next concept: [Who runs where](concurrency/threads.md). Go deeper:
[reference 15](../reference/15-concurrency-and-scaling.md), then
[reference 17](../reference/17-reclamation-policy.md).
