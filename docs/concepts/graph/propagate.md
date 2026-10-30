# Propagate and FOLD

## In one paragraph

A write is two operations. `assign` replaces a vertex's value, advances its write sequence, wakes
any `await` on it and marks it pending; it reads no edge and sends nothing. `propagate` delivers,
along subscription edges, the vertex's current stored value and every pending descendant in its
subtree; it takes no value and changes nothing. `write(v, x)` is exactly `assign(v, x)` then
`propagate(v)`. Split apart, they let a producer assign at any rate and propagate when it chooses:
k assigns between two sweeps deliver once, with the latest value, and a sweep over a large quiet
subtree touches only the vertices that changed. FOLD is one more choice on the same sweep: send
the whole selection as one branch-write frame instead of one write per vertex.

## The picture

```{mermaid}
:caption: "Legend: purple = application, green = vertex, blue = frame."
flowchart LR
    accTitle: Assign marks, propagate sweeps, FOLD folds
    accDescr: The application assigns new values to /s/a and /s/b, which marks them pending. A propagate on /s sweeps the pending vertices in its subtree. By default it sends one write per selected vertex. With FOLD it sends one branch-write frame holding both.
    A([application]):::app -->|"assign"| VA("/s/a, pending"):::vertex
    A -->|"assign"| VB("/s/b, pending"):::vertex
    A -->|"propagate /s"| SW{"emission?"}
    SW -->|"default: one write each"| W1[/"write /s/a"/]:::frame
    SW -->|"default"| W2[/"write /s/b"/]:::frame
    SW -->|"FOLD: one frame"| F[/"branch write for /s"/]:::frame
```

## When you need it

Use `write` until you have a reason not to. Split it when a producer samples faster than anyone
needs to hear (assign at the sample rate, propagate at the delivery rate), when several leaves
change together and should be flushed together, or when a remote subtree subscriber should receive
one frame per flush rather than one per leaf.

## How it works

**Pending, not queued.** An assign enters the vertex in the graph's pending set, ordered by
canonical path key, so a subtree is a contiguous range of it. A sweep costs time in proportion
to the pending vertices under its root, not the subtree's size. The write sequence is a change
counter compared for equality only; nothing compares the value's bytes.

**The root selects, the edges decide who hears.** `propagate(root)` selects which vertices flush.
Each selected vertex is delivered to every subscriber that observes it, including subtree
subscribers above `root`, and then its pending mark is drained. A second, overlapping sweep
therefore neither repeats a delivery nor misses an observer.

**Delivery mode.** A per-vertex attribute decides whether an ancestor's sweep includes the
vertex: `IF_NEWER`, the default, only when pending; `UNCONDITIONAL` always, which turns the
producer's own propagate cadence into a keepalive; `EXPLICIT` never, only a direct propagate on
the vertex itself. An assign is never gated by it, and a direct propagate always delivers its
argument.

**FOLD.** `propagate(v, emission_mode_t::FOLD)` selects exactly what the default would and emits
one branch-write frame: the folded `POINT` tree of the selection, rooted at `v`. A subscriber at a
covered leaf receives byte-identical bytes either way, because the receiver decomposes the tree.
It is one frame per subtree; two subtrees are two calls. The fold refuses before it delivers or
drains anything when a selected vertex cannot be one node of the tree: a value that is not a
single trailer-less `VALUE` answers `tr::schema::type_mismatch`, and so does a STREAM vertex, whose
ring has no single value to fold. The caller falls back to the default emission and loses nothing.

**A handler keeps nothing to propagate.** `assign` and `propagate` rooted at a handler vertex
answer `SCHEMA_NOT_FOUND`; use `write`, which calls the handler and delivers at once.

**On the wire.** A `FWD{WRITE}` arriving at its terminus means assign the addressed vertex, then
propagate it. The terminus is the propagate's argument, so a directed write always lands.

## Worked example

`/s/adc0` holds a composed batch. `propagate(/s, FOLD)` sweeps `/s` and sends one branch-write
frame: a `POINT` naming the root, a child `POINT` naming `adc0`, and the child's one `VALUE`.

```{hex-walk} stream/fold-carries-composed-batch
:caption: One FOLD sweep of /s, carrying /s/adc0's value.
str: the root's leading NAME
str: the child vertex's NAME
u64: TIME base, the sample time of frame 0 in ns
i32[]: offsets of each sample from the base, in ns
u16: sample 0
u16: sample 1
u16: sample 2
```

The default emission of the same sweep is one `FWD{WRITE}` to `/s/adc0` carrying the 50-byte
batch record. [Batching](../batching.md) walks that record field by field.

## Cost

### Wire bytes

The default sends one `FWD{WRITE}` per selected vertex, each paying its own header and address
bytes ([the illustrated frame](../wire/illustrated-frame.md)). FOLD pays one frame plus, per
vertex, a `POINT` and a `NAME` of `8 + len` bytes. For 64 leaves of 1 KiB each, the payload is
64 KiB either way; the default adds 64 frame envelopes, FOLD one envelope and 64 small nodes, and
the folded frame needs the wide length (`opt.LL`) once it passes 65,535 bytes.

### Allocations

`assign` swaps one value in; it does not enqueue. `propagate` on an idle subtree draws nothing. A
local delivery is a reference-count clone of the stored value. FOLD composes one frame head and
links the stored values behind it, without copying them.

### Latency

Coalescing is the latency trade: a sample waits until the producer's next propagate, and that
wait is the producer's choice. `propagate_fold_test` pins the fold's behaviour; no bench times a
FOLD sweep against the default, and an arm in `bench_fanout_clone_storm` would.

### Flash and RAM per target

| Target | RAM | Flash |
| --- | --- | --- |
| NARROW | the pending set holds one entry per pending vertex; no per-subscriber state | part of the graph in the gated Cortex-M0 node |
| MID | same | inside the reported ESP-IDF core |
| WIDE | same | not measured |

## What the library does not do

- It does not propagate on a timer. The producer's call is the flush.
- It does not compare a new value with the old one. `ON_CHANGE` byte comparison does not exist.
- It does not hold a value in a producer-side queue between assign and propagate; the stored
  value is the single source of truth.
- It does not merge two subtrees into one frame.

## Pitfalls

- **Symptom:** `propagate(v, FOLD)` is refused. **Cause:** a selected vertex is a STREAM, or holds a
  value with a trailer or more than one record. **Fix:** compose one value onto a stored vertex, or
  fall back to the default emission.
- **Symptom:** `propagate` at a handler answers `SCHEMA_NOT_FOUND`. **Cause:** a handler retains
  nothing for a sweep to deliver. **Fix:** call `write`.
- **Symptom:** a vertex never reaches its parent's subscribers. **Cause:** its delivery mode is
  `EXPLICIT`. **Fix:** propagate it directly, or set `IF_NEWER`.

## Where the rules live

**Normative source:** [v1 §3](../../spec/v1.md#3-wire-format), which incorporates the branch
write in [reference 05 §Branch write](../../reference/05-protocol-tlvs.md#branch-write--decomposition)
and subtree delivery in
[reference 05 §Subtree subscription](../../reference/05-protocol-tlvs.md#subtree-subscription--vertical-bubbling).

**Exercised by:** `fold-carries-composed-batch`, `propagate_fold_test`, `delivery_mode_race_test`,
`folded_children_test`.

Next concept: [STREAM rings](stream-ring.md). Go deeper:
[reference 02 §Assign, propagate](../../reference/02-graph-model.md#assign-propagate-and-the-coalescing-sweep),
then the [graph module §Assign and propagate](../../modules/graph.md#assign-and-propagate), then
[reference 05 §Branch write](../../reference/05-protocol-tlvs.md#branch-write--decomposition).
