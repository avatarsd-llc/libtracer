# STREAM rings

## In one paragraph

A STREAM vertex is a queue: its contract is that a reader observes every buffered entry, not only
the latest. The queue belongs to whoever consumes it. A subscriber that wants history makes its
own target vertex a STREAM, declares how many entries it means to keep, and injects the block
source that bounds the ring in bytes; the producer is never asked to retain anything and keeps
writing on its lock-free path. When the ring cannot admit an entry it answers in one of two ways,
chosen when it is wired: best effort sheds the oldest entry and records a gap in order, reliable
refuses the write and answers `BACKPRESSURE`. Neither the depth nor the arm crosses the wire.

## The picture

```{mermaid}
:caption: "Legend: purple = application, green = vertex, orange = ring."
flowchart LR
    accTitle: The consumer's ring and its two pressure arms
    accDescr: A producer's vertex delivers to the consumer's STREAM target. The target's ring, sized by the consumer's own block source, admits each entry. When full, best effort sheds the oldest entry and raises a gap marker; reliable refuses and the local producer sees BACKPRESSURE. The consumer drains the ring in order.
    P("producer /imu"):::vertex -->|"deliver"| T("consumer STREAM /rx/imu"):::vertex
    T --> R[("ring: depth intent, bytes from its own source")]:::ring
    R --> F{"full?"}
    F -->|"best effort: shed oldest"| G["gap marker, in order"]
    F -->|"reliable: refuse"| B["BACKPRESSURE to a local writer"]
    R -->|"history, drain in order"| C([consumer]):::app
```

## When you need it

When every sample matters to the reader: a log, a sensor trace, a command sequence. If only the
newest value matters, a stored vertex is cheaper and coalesces for free. If the producer can
compose many samples into one value, a [batch](../batching.md) in a ring of a few entries holds the
same samples for far less overhead.

## How it works

**Declaring the ring.** The consumer registers its target as `STREAM` and declares depth with
`graph_t::set_policy(v, {.retention = retention_t::N, .depth = depth})`. Depth is the intent; the
block source injected at that vertex is the bound, in bytes. A depth the source cannot fund shows
up as a shed or a refusal, never as a quietly smaller ring. Each ring has its own source, never a
pool shared with another vertex.

**Admission.** Each delivery reserves one entry. The declared depth retires the oldest entry
before the byte bound is charged, so a ring at its depth pays for the new entry with the one it
was about to drop, and a source sized for exactly N entries does not shed on the N+1th.

**The two arms.**

| | best effort (the default) | reliable |
| --- | --- | --- |
| when the reservation fails | the oldest entry is shed, once, and the write retried | the write is refused, the ring unchanged |
| the producer sees | success | `BACKPRESSURE`, for a local producer |
| the consumer sees | `tr::flow::address_shift_gap`, in order, at the shed point | nothing missing |
| counted by | `stream_gaps()` | nothing to count |

The shed is singular, one entry per refused admission, so a source that has gone to zero empties
the ring one write at a time rather than in one stroke. The reliable arm reaches only a local
producer in v1: there is no wire carrier for backpressure, so a remote producer learns of drops
after the fact, or from a watermark the consumer writes back.

**Draining.** `graph_t::history(v, out)` hands the ring's entries to the caller's span as
references to the stored values, without allocating. A STREAM vertex's own propagate drains,
in order, the entries appended since its last flush, where a stored vertex sends only the latest.

**Order.** Entries keep arrival order at that ring. Order across writers comes from each
producer's own monotonic stamps, never from a sequence number the ring mints.

## Worked example

The ring's depth has no wire surface. A peer that names it, as if it were a field, gets the
reply below: an `ERROR` holding the status `tr::schema::not_found`.

```{hex-walk} stream/history-depth-host-only
:caption: What a peer gets for asking about a ring's depth.
u16: the status, 0x0031 = 49, tr::schema::not_found
```

## Cost

### Wire bytes

None of its own. A ring is local state at the receiver; what reaches it is ordinary deliveries,
one entry per delivery. A batch of N samples is one entry, so a ring of 4 entries holding 1 KiB
batches holds 4 KiB of samples.

### Allocations

One reservation per admitted entry, from the ring's own injected source; none for a shed beyond
releasing the oldest. Reading history allocates nothing. The producer's write allocates nothing on
the ring's behalf.

### Latency

The ring is on the consumer's side, so it adds nothing to the producer's write. The producer-side
ring it replaced cost a fixed 29 ns, 54%, per write regardless of depth, and four writers on one
vertex fell from 4.59 M writes a second lock-free to 1.73 M through it. Batch folding is the
cheapest stream carrier, about 2.54 ns per sample. `ring_pressure_test` pins both arms.

### Flash and RAM per target

| Target | RAM | Note |
| --- | --- | --- |
| NARROW | the ring's bytes, from the source you inject at that vertex | plus the vertex's extension block |
| MID | same | same |
| WIDE | same | same |

The graph adds no depth constant and no shared pool. On a tens-of-kilobytes MCU, size each ring
against the batch it holds, per
[reference 22 §3.1](../../reference/22-backpressure-and-sizing.md#31-bounded-mcu-node-esp32-class-tens-of-kb-heap).

## What the library does not do

- It does not queue at the producer, at any depth or delivery class.
- It does not carry backpressure across a link in v1.
- It does not let a peer read or set a ring's depth.
- It does not shrink a ring silently when its source runs short.

## Pitfalls

- **Symptom:** a remote producer keeps sending into a reliable ring that is refusing. **Cause:**
  backpressure reaches only a local producer. **Fix:** have the consumer write back a
  received-up-to watermark per batch, and let the producer pace itself to it.
- **Symptom:** gaps appear at a rate the ring handled before. **Cause:** entries grew, as when
  samples became batches. **Fix:** size the ring's source in bytes against the entry it now holds.
- **Symptom:** STREAM writes slow sharply as threads are added. **Cause:** many rings share one
  block source, which collapsed to 0.01× its single-thread rate at 24 threads where separate
  sources kept 0.46×. **Fix:** inject a source per ring.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates the
subscription record and its delivery in
[reference 05 §0x04 SUBSCRIBER](../../reference/05-protocol-tlvs.md#0x04--subscriber); a ring's
depth and arm are the implementation's, with no wire surface.

**Exercised by:** `history-depth-host-only`, `ring_pressure_test`, `stream_admit_one_lock_test`.

Next concept: [Subscriptions](subscribe.md). Go deeper:
[reference 22 §2](../../reference/22-backpressure-and-sizing.md#2-the-two-pressure-arms-stated-once),
then [reference 02 §Stream drain semantics](../../reference/02-graph-model.md#stream-drain-semantics),
then the [graph module](../../modules/graph.md).
