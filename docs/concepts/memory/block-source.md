# The block source

## In one paragraph

`tr::mem::block_source_t` is the seam every failable allocation in the core draws from. It has
two operations. `try_alloc(bytes, align)` returns a block or `nullptr`, and is `noexcept`: a
refusal is a value, never a throw, an abort or a quiet fall back to the global heap.
`release(p, bytes, align)` gives a block back and must repeat the size and alignment it was asked
for, so a source keeps no header per block. Each source has a name and a census of what it holds
and what it refused. It is a type of its own, not a `std::pmr::memory_resource`, because that
type's `allocate` can only fail by throwing, and its null check is deleted by the compiler at the
optimization level an MCU ships.

## The picture

```{mermaid}
:caption: "Legend: green = vertex, sky blue = record, grey dashed = not the library."
flowchart LR
    accTitle: A failable draw and its two outcomes
    accDescr: The graph asks its block source for a block. The source either returns a block, which the graph fills and later gives back with a sized release, or returns null. A null is counted in the source's census and the graph answers the operation BACKPRESSURE. Nothing falls through to the platform heap.
    G("graph_t"):::vertex -->|"try_alloc(bytes, align)"| S{"block_source_t"}
    S -->|"a block"| U["value block, filled"]
    U -->|"release(p, bytes, align)"| S
    S -->|"nullptr"| C["census: refused, largest_refused"]:::record
    C -->|"answer by value"| B["BACKPRESSURE"]
    S -.-x|"never"| H[global heap]:::notlib
```

## When you need it

When you inject a bound, write a source of your own, or read why a write was refused. Most
applications only pass a source to `graph_t`'s constructor and read its census; the seam itself
matters when you build an adapter, such as a source over a region a driver owns.

## How it works

**The contract.** A source returns storage of at least `bytes`, aligned to `align`, or
`nullptr`. Which status the caller answers is the caller's, and follows the operation: a refused
write answers `BACKPRESSURE` and the vertex keeps its previous value; a refused reply at a
terminus is an addressed `BACKPRESSURE`, or a counted drop when even that cannot be built; a
branch write whose decode is refused answers `TYPE_MISMATCH`.

**Sized release.** `release` takes the same `bytes` and `align` as the `try_alloc` that served
the block. A source can find the block's class from those two numbers, so it stores nothing
beside the block.

**No deleting destructor.** The destructor is protected and not virtual. A source is destroyed
as the class it is, never through the base, so no object that emits the vtable references
`operator delete`, which the zero-heap check would count.

**The census.** `stats()` returns a `source_stats_t`: `capacity` (0 when unbounded), `in_use`,
`peak`, `refused` and `largest_refused`. Refusals are counted on the refusal arm alone, so a
successful draw pays nothing for them. `largest_refused` is the number a deployment grows its
source to.

**The sources in the box.**

| Source | Serves | On exhaustion |
| --- | --- | --- |
| `heap_source()` | the platform heap, nothrow | `nullptr` |
| `null_source()` | nothing | always `nullptr` |
| `bump_source_t` | a caller span, front to back, never reclaimed | spills to an upstream source |
| `pool_source_t` | exact-size free lists over a caller slab | `nullptr` |
| `default_root()` | the build's default: [slab pool](pools.md) or [arena](arena.md) | `nullptr` |

**Adapters.** `source_resource_t` presents a source as a `std::pmr::memory_resource` for code that
needs one, and throws `std::bad_alloc` there because that contract requires it.
`source_backend_t` presents one as a `mem_backend_t`, where a refusal is a null segment, which
the write path already answers `BACKPRESSURE`. Neither falls back to the heap.

## Cost

### Wire bytes

None. A peer sees a refusal only as the status the operation answers.

### Allocations

The seam adds none of its own. A publish is one `try_alloc`, for the value block that holds the
reference count, the segment header and the payload. A bump source is for one operation's scope:
an 8 KiB one wired as a router's receive source decoded 6 frames of 53 bytes and refused the next
194, a capacity result and not a rate.

### Latency

One virtual call per draw. Where a draw sits on the hottest path the library makes a direct call
instead: moving the LKV hazard-slot nodes onto the virtual seam cost 22.7% and was reverted.

### Flash and RAM per target

| Target | Flash | RAM |
| --- | --- | --- |
| NARROW | the vtable of each source the build links | the source object; no per-block header |
| MID | `pool_source_t`: 340 B of text plus a 28 B vtable, riscv32 at `-Os` | same |
| WIDE | not measured | same |

## What the library does not do

- It does not retry a refused draw against another source.
- It does not throw from `try_alloc`, or abort on a refusal a peer can provoke.
- It does not record which call site was refused; the census counts, the status tells the caller.

## Pitfalls

- **Symptom:** a bump source works in a test and refuses everything in service. **Cause:** a bump
  block is never reclaimed, so a long-lived one fills. **Fix:** construct it per operation, or use
  a `pool_source_t`, which recycles.
- **Symptom:** a custom source corrupts its free list. **Cause:** `release` was given a different
  size or alignment than the draw. **Fix:** pass back exactly what `try_alloc` was asked for.
- **Symptom:** an exhaustion check vanishes in a size-optimized build. **Cause:** the code drew
  through `std::pmr::memory_resource::allocate`. **Fix:** draw through `try_alloc`.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope) leaves allocation to the implementation,
and [v1 §3.1](../../spec/v1.md#31-static-path-handle-conformance) asks only that addressing a
vertex on the hot path not allocate. The seam is not normative.

**Exercised by:** `mem_source_test`, `mem_source_pmr_test`, `mem_source_backend_test`,
`graph_oom_softfail_test`, `terminus_flatten_backend_test`.

## History

:::{dropdown} How the seam got here
- [ADR-0065](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md) (2026-07-27)
  gave failable allocation its own type, after the `std::pmr` null check was measured deleted at
  `-Os` and `-Oz`.
- [ADR-0083](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md) (2026-10-03) made it the one seam every core
  allocation goes through.
:::

Next concept: [Pools](pools.md). Go deeper:
[reference 09 §The second L0 seam](../../reference/09-memory-substrate.md#the-second-l0-seam-block_source_t-failable-blocks),
then [the status legs](../../design/allocation-and-backpressure.md#status-legs), then the
[backends module](../../modules/backends.md#the-failable-block-seam--block_source_t).
