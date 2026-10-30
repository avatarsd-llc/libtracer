# Who runs where

## In one paragraph

The graph and the router start no threads. Every call runs on the thread that makes it, and a
write delivers to its subscribers on the writer's own thread, before the write returns. A link
brings its own thread: each POSIX socket link owns a receive thread and hands every frame it
reads to the router from there, so a peer's write, and the deliveries it causes, run on that
receive thread. Control-plane work such as opening a connection arrives the same way, and the
transport vertex serializes it on its own mutex. The locks the data path does take are few and
narrow: a path lookup takes the graph's map lock shared, and a delivery reads its edges under a
pin and dispatches outside every lock, so a callback may call back into the graph.

## The picture

```{mermaid}
:caption: "Legend: purple = application, vermillion = peer, green = vertex, grey = not the library."
flowchart LR
    accTitle: A write and the thread its delivery runs on
    accDescr: A thread writes a vertex. The write advances the sequence without a lock, snapshots the published edge array under an edge pin, releases the pin, and then calls each subscriber's delivery on the same thread. A peer's frame arrives on the link's receive thread, which makes the same write and so runs the same deliveries.
    A([application thread]):::app -->|"write"| V("vertex"):::vertex
    P[[peer]]:::peer ==>|"socket link"| T["receive thread"]:::notlib
    T -->|"write, via the router"| V
    V -->|"snapshot edges under a pin"| D{"dispatch, outside every lock"}
    D -->|"same thread"| S1("subscriber target"):::vertex
    D -->|"same thread"| S2("subscriber target"):::vertex
```

## When you need it

When a callback blocks, a lock seems to be held too long, or work lands on a thread you did not
expect. Knowing which thread runs a delivery tells you whose stack and priority it borrows.

## How it works

**Delivery runs on the writer.** A write bumps the vertex's sequence without a lock. With no
subscriber it stops there, after one load. Otherwise it copies the vertex's published edge array
under a bounded edge pin, drops the pin, and dispatches each edge. Nothing queues the delivery
for later and no thread is woken to run it: the write returns once every edge has been served.

**A callback may re-enter.** Because dispatch holds no lock, a delivery callback may read,
write, subscribe or unsubscribe. A write from inside a delivery is a nested delivery on the
same thread; nothing caps its depth, so a cycle is the application's to break.

**Handles skip the map.** A handle-addressed read or write takes no map lock. A path-addressed
one resolves the path under the graph's map lock, taken shared, then proceeds as the handle
call does.

**Stripes guard the control plane.** Adding or clearing an edge, filling a STREAM ring and
setting an ACL take one of `kVertexLockStripes` process-wide mutexes, 16 by default, picked from
the vertex's address. A publish takes one only to wake a parked waiter.

**Links bring threads.** Each POSIX socket link owns a receive thread, joined by its
`shutdown()`. Creation is a write, so a peer's write to a creator endpoint runs on that thread
and may dial a socket; the transport vertex holds its own mutex while it does. The declared lock
order is transport vertex, then router, then graph, then vertex stripe, and no data write waits
behind a dial.

**Registration takes a lock.** Registering a vertex is not safe from an interrupt. Register from
a task or a thread.

## Cost

### Wire bytes

None.

### Allocations

None for the dispatch itself; the edge snapshot lands in one of two buffers the pin owns. A wide
fan-out reuses a thread-local buffer, and a nested re-entrant one may allocate.

### Latency

A write with no subscriber costs one load and no lock. Reference 15 §3 measured a `std::mutex`
at 43.7 ns per operation at 24 threads, which is why the publish path took the stripe mutex out:
on the same-stripe fan-1 write, 18.58 times as fast at 24 threads.

### Flash and RAM per target

| Target | Threads the library adds |
| --- | --- |
| NARROW | none |
| MID | none in the graph or router; a link may run its own task |
| WIDE | one receive thread per POSIX socket link |

## What the library does not do

- It does not hand a delivery to another thread or a queue.
- It does not cap the depth of a re-entrant write.
- It does not make registration interrupt-safe.
- It does not order a write against a racing retire of the same vertex; the calling plane does.

## Pitfalls

- **Symptom:** a slow callback stalls a link. **Cause:** the delivery runs on the link's receive
  thread. **Fix:** keep callbacks short; post heavy work to a thread the application owns.
- **Symptom:** a write from a callback never returns. **Cause:** the callback writes a vertex
  that leads back to itself. **Fix:** break the cycle in the application.
- **Symptom:** a crash registering from a bus interrupt. **Cause:** registration takes a lock.
  **Fix:** register from the discovery task.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope). Which thread runs what is an
implementation concern and not normative.

**Exercised by:** `net_lock_order_test`, `net_control_plane_race_test`,
`delivery_mode_race_test`, `subscribe_retire_race_test`, `retire_sweep_race_test`,
`stream_admit_one_lock_test`.

## History

:::{dropdown} How the threading model got here
- [ADR-0051](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0051-delivery-terminates-at-target-no-dispatch-limits.md)
  made delivery terminate at its target, with no depth cap and no drain queue.
- [ADR-0063](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0063-connection-table-lock-free-reads-trait-serialized-writes.md)
  serialized control-plane writes on the transport vertex's own mutex.
- [ADR-0075](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0075-a-vertexs-edges-are-published-and-read-under-an-edge-pin.md)
  moved the edge read off the stripe mutex and under a pin.
:::

Next concept: [Reclamation](reclamation.md). Go deeper:
[reference 19 §4](../../reference/19-transports-are-vertices.md#4-control-plane-work-runs-on-a-data-thread),
then the [transport module](../../modules/transport.md).
