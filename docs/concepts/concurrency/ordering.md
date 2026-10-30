# Ordering

## In one paragraph

What one vertex promises is narrow and exact. A reader sees the whole previous value or the
whole new one, never a mix. A value it holds stays valid however often the vertex is written
after. Every assign advances the vertex's write sequence, a 32-bit counter that `await` tests
only for a change, and each subscriber of that vertex receives its values in the order they were
assigned. Across vertices nothing is promised: two threads writing two vertices may be seen in
either order, and a registration racing an operation on the same address may go either way. A
caller that needs an order across vertices builds it, by writing one vertex, or by putting a
sequence number of its own in the value.

## The picture

```{mermaid}
:caption: "Legend: purple = application, green = vertex, sky blue = record."
flowchart LR
    accTitle: Order within one vertex, none across two
    accDescr: Writer one assigns value a then value b to vertex x; the sequence goes from n to n plus 1 to n plus 2, and x's subscriber receives a then b. Writer two assigns c to vertex y at about the same time; nothing orders c against a or b.
    W1([writer 1]):::app -->|"assign a, then b"| X("vertex x"):::vertex
    X --- Q["write sequence: n, n+1, n+2"]:::record
    X -->|"a, then b"| S("subscriber of x"):::vertex
    W2([writer 2]):::app -->|"assign c"| Y("vertex y"):::vertex
    Y -.->|"no order against a or b"| S
```

## When you need it

When a reader seems to see a stale value, when `await` wakes and finds no new bytes, or when two
values written together must be read together.

## How it works

**A write is atomic for its vertex.** A vertex holds one value, a rope of views. A write swaps
the whole rope; a reader observes one rope or the other.

**A held value stays valid.** A read hands back a refcounted handle. A later write replaces the
vertex's value, not the one the reader holds; the refcount, adjusted with at least acquire and
release ordering, frees it when the last holder lets go. Holding a value is itself a write to
its refcount, so many threads holding one hot value contend on its cache line.

**The sequence is for change, not for order.** Each assign bumps a 32-bit sequence, including
assigns that store nothing, such as a handler vertex's. `await` samples it and wakes when it
differs. It wraps at 2^32 without event, so never compare two samples for less or greater.

**Per subscriber, per vertex, in assign order.** Delivery runs on the writer's thread, so one
writer's assigns reach each subscriber in that writer's order.

**Across vertices, nothing.** The order of concurrent writes to different vertices is left open.
To publish two values that must be read together, write them as one value, a branch write or a
batch, to one vertex.

**Registration races.** A read that races the registration of its own address may resolve or
answer `PATH_NOT_FOUND`. Order the two in the application if it matters.

## Cost

### Wire bytes

None. Ordering here is a property of one node.

### Allocations

None. The sequence lives in the vertex; the refcount in the value's block.

### Latency

The sequence bump is one atomic add, taken without a lock. Copying a shared handle at 24 threads
cost 32.5 ns per operation on the machine of reference 15 §3, against 0.10 ns for a read of a
word nobody writes: an owning read of a hot value is a write.

### Flash and RAM per target

| Target | Write sequence |
| --- | --- |
| NARROW | 4 B per vertex; a guarded load and store on a core with no atomic add (Cortex-M0) |
| MID | 4 B per vertex; the same guarded pair on ESP32-C3 |
| WIDE | 4 B per vertex, one hardware atomic add |

## What the library does not do

- It does not order writes to different vertices.
- It does not give the write sequence an order; it answers only "changed or not".
- It does not order a registration against an operation on the same address.
- It does not diff values: an assign of the same bytes is still an assign.

## Pitfalls

- **Symptom:** a reader pairs a new `x` with an old `y`. **Cause:** they are two vertices.
  **Fix:** write them as one value.
- **Symptom:** a check `seq > last` fails after a long run. **Cause:** the sequence wrapped.
  **Fix:** test only for inequality.
- **Symptom:** threads reading one value get slower as threads are added. **Cause:** every
  owning read writes the refcount. **Fix:** shard the value, or read it once per batch of work.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope). The protocol defines what one operation
does; what concurrent ones may observe is not normative.

**Exercised by:** `write_seq_test`, `seq_latch_test`, `stream_admit_one_lock_test`,
`lkv_slot_test`, `lkv_slot_inversion_test`.

[Reference 15 §2.1](../../reference/15-concurrency-and-scaling.md#21-obligations) derives the
obligations, and [§2.3](../../reference/15-concurrency-and-scaling.md#23-where-the-protocol-is-silent-and-arguably-should-not-be)
names the registration race; `bench_contention` and `bench_lkv_slot` measure the costs.

No conformance vector covers ordering yet. A two-frame vector, two `FWD{WRITE}` to one vertex
and the subscriber's deliveries in assign order, would.

## History

:::{dropdown} How the ordering rules got here
- [ADR-0064](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md)
  made the last-known-value publish waiterless and the slot lock-free.
- [RFC-0028](../../spec/rfcs/0028-lean-value-path.md) fixed the write sequence at 32 bits.
:::

Next concept: back to [concurrency](../concurrency.md). Go deeper:
[reference 15 §4, why an owning read is a write](../../reference/15-concurrency-and-scaling.md#4-why-an-owning-read-is-a-write),
then [§5, which shapes scale](../../reference/15-concurrency-and-scaling.md#5-which-shapes-scale).
