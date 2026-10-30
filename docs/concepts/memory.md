# Memory

## In one paragraph

Every byte libtracer's core allocates comes through one seam, `tr::mem::block_source_t`: a call
that returns a block or a null pointer, and never throws, aborts or falls back to the heap. The
application chooses what stands behind it. A host build's default is a slab pool that asks the
platform heap only for whole slabs. An MCU build's default is a static arena sized at compile
time, with no heap behind it at all. A bounded node injects a pool whose size is its bound. When
a source runs out, the operation that asked answers by value, most often `BACKPRESSURE`, and the
node keeps running. A link-time check holds the MCU archive to that promise on the Cortex-M0
build.

```{toctree}
:hidden:
:maxdepth: 1

The block source <memory/block-source>
Pools <memory/pools>
The static arena <memory/arena>
The zero-heap profile <memory/zero-heap>
```

## The picture

```{mermaid}
:caption: "Legend: purple = application, green = vertex, grey dashed = not the library."
flowchart LR
    accTitle: One seam, three things that can stand behind it
    accDescr: The application injects one block source into the graph, which draws values, tables and rings from it. Behind the seam stands either the host slab pool, which asks the platform heap for whole slabs, or a bounded pool the application sized, or the MCU static arena. An exhausted source returns null, and the operation answers BACKPRESSURE.
    A([application]):::app -->|"inject"| S{"block_source_t"}
    S -->|"host default"| P["slab pool"] -->|"whole slabs"| H[platform heap]:::notlib
    S -->|"bounded node"| B["pool sized by the app"]
    S -->|"MCU default"| R["static arena, kArenaBytes"]
    G("graph_t"):::vertex -->|"try_alloc: block or null"| S
    G -->|"null: BACKPRESSURE"| A
```

## When you need it

On a host you can ignore this area until you want a bound: the default grows with demand. On an
MCU, or wherever a peer must not be able to exhaust the node, read it before you size anything:
the source you inject is the node's memory budget, and the census it keeps is how you measure it.

## How it works

| Piece | What it is | Page |
| --- | --- | --- |
| `block_source_t` | the one failable seam: `try_alloc`, sized `release`, a census | [The block source](memory/block-source.md) |
| pools | recycling sources: the bounded `pool_source_t`, the host `slab_pool_t` | [Pools](memory/pools.md) |
| static arena | the MCU default: one `.bss` array, size-classed, never trimmed | [The static arena](memory/arena.md) |
| zero-heap profile | the MCU build that references no heap, and the check that holds it | [The zero-heap profile](memory/zero-heap.md) |

**One injection at the graph.** `graph_t`'s constructor takes one source. Values, the graph's
tables, folded reads, read-back encoders and the default receiver ring all draw from it, so a
deployer sizes one region and reads one census.

**Receiver pays.** A cost a peer provokes is drawn from the receiving side's own source: a link's
receive memory, a STREAM vertex's own ring source. One receiver's exhaustion does not starve
another's.

**Which source, not which knob.** NARROW versus WIDE is the source you inject, decided at build
time; there is no runtime allocator switch.

## Cost

Each child page carries its own numbers. In summary:

### Wire bytes

None. Memory policy never reaches the wire; a peer sees only the `BACKPRESSURE` an exhausted
source causes, and the `:stats.mem.*` census when it reads one.

### Allocations

One `try_alloc` per publish, for the value block, with the segment header and payload in the
same block. On a host a 1 KiB value is one 1,152-byte size-class block; a payload past the top
class, a 64 KiB segment, is drawn on its own from the root, about 8 ns more.

### Latency

The host default puts a per-thread cache in front of the value classes. A shared free-list pool
on a per-frame path is measured to collapse under contention, so give each receiver its own
source rather than guarding a shared one.

### Flash and RAM per target

| Target | Default source | Static RAM | Flash |
| --- | --- | --- | --- |
| NARROW | static arena | `kArenaBytes`, 32 KiB by default | `pool_source_t` is 340 B of text plus a 28 B vtable on riscv32 |
| MID | static arena (ESP-IDF, `CONFIG_LIBTRACER_ARENA_BYTES`) | 32 KiB by default; 0 with the heap root | same |
| WIDE | host slab pool | none: slabs on demand | not measured |

## What the library does not do

- It does not fall back to the global heap when an injected source refuses.
- It does not abort or throw on exhaustion a peer can provoke.
- It does not choose a node's bound. The bound is the size of the source you inject.
- It does not share one pool across receivers unless you wire it that way.

## Pitfalls

- **Symptom:** a node rejects writes with `BACKPRESSURE` under load but never at idle. **Cause:**
  the source was sized from idle free memory. **Fix:** size it from the saturating run's peak, read
  from the census.
- **Symptom:** throughput falls as threads are added. **Cause:** one shared pool on a per-frame
  path. **Fix:** a source per receiver.

## Where the rules live

**Normative source:** [v1 §1](../spec/v1.md#1-scope) leaves memory to the implementation; nothing
here is normative. What a peer can observe, an exhausted receiver's `BACKPRESSURE` and the
`:stats` census record, is in [v1 §3](../spec/v1.md#3-wire-format) via
[reference 05](../reference/05-protocol-tlvs.md#the-seam-census-record--stats).

**Exercised by:** `graph_oom_softfail_test`, `transport_alloc_softfail_test`, `slab_pool_test`,
`mem_arena_test`.

Next concept: [The block source](memory/block-source.md). Go deeper:
[failable allocation and backpressure](../design/allocation-and-backpressure.md), then
[reference 09](../reference/09-memory-substrate.md#one-injection-at-the-graph), then the
[backends module](../modules/backends.md).
