# Pools

## In one paragraph

A pool is a block source that recycles. libtracer ships two. `pool_source_t` is the bounded
one: exact-size free lists over a slab the application hands it, so the slab's size is the
node's bound and exhaustion is a `nullptr`. `slab_pool_t` is the size-classed one: it rounds a
request up to a row of the build's size-class table and asks the source behind it only for
whole slabs, which turns sizes a peer chooses back into a few sizes a pool can recycle. The
host default root is a slab pool over the platform heap, split into value, table and net
sub-pools, so the platform allocator never sees a request per value.

## The picture

```{mermaid}
:caption: "Legend: green = vertex, grey dashed = not the library."
flowchart LR
    accTitle: A request through the host slab pool
    accDescr: The graph asks for 1,072 bytes. The slab pool rounds the request up to its 1,152-byte class and takes a block from that class's slab. A class with no free block asks the root for one whole slab. A request past the last row is drawn from the root at its own size, outside the census's in_use.
    G("graph_t"):::vertex -->|"try_alloc 1,072 B"| K{"class row?"}
    K -->|"rounds up to 1,152 B"| C["class: free list in its slabs"]
    C -->|"empty: one whole slab"| R[root: platform heap]:::notlib
    K -->|"past the last row"| O["own block from the root"]
    O --> R
```

## When you need it

When a host node should have a bound, inject a `pool_source_t` sized for it. When peers choose
the sizes, as with received payloads, put a `slab_pool_t` in front of it. When a host fans out
wider than about 1,365 subscribers per vertex, or holds payloads past 64 KiB, read the size-class
table before you rely on the default.

## How it works

**`pool_source_t`.** Each `size_class_t` the application supplies holds one exact
`(bytes, align)` pair and a free list threaded through its blocks, so the number of classes is a
deployment choice and no class keeps a header. Where the application chooses the sizes, demand
is nearly degenerate: a recorded host-suite trace of 70,937 events had 12 distinct sizes, three
of them 99.8% of the draws, and exact classes served it from a 26,176-byte slab where a
first-fit allocator with coalescing needed 27,448.

**`slab_pool_t`.** A slab is a power of two, `kSlabBytes` (64 KiB) or larger, aligned to its
own size, so a block finds its slab by masking its address. A request takes the smallest row of
`default_config_t::kSizeClasses` that holds it: eight rows of 16 to 128 bytes, then eight per
doubling up to 64 KiB, then 65,584 bytes for a 64 KiB payload's one-block segment, 81 rows in
all. No block wastes more than an eighth of its size. A class takes no memory until its first
draw. A request past the last row, or aligned past 64 bytes, is drawn from the root as its own
block and is not counted in `in_use`.

**Giving memory back.** A class keeps up to `kSlabClassCap` (2) fully free slabs and returns the
rest at once. `host_root_t::trim()` returns every fully free slab, when the application calls
it; nothing trims on a timer.

**The host default root.** `default_root()` on a host build is a `host_root_t`: three sub-pools
over the platform heap. The value sub-pool puts a per-thread cache in front of its classes; the
table and net sub-pools take the class lock per draw. Each reports its census as
`:stats.mem.values`, `:stats.mem.tables` and `:stats.mem.net`.

**Sharing.** A pool's lock is a compile-time parameter, `tr::no_guard_t` by default, which
compiles to nothing. `sync_mutex_t` is for a source shared at wiring frequency, such as a
graph's control source. For a per-frame path, give each receiver its own pool.

## Cost

### Wire bytes

None. A peer the ACL admits can read the census as the `:stats.mem.*` records.

### Allocations

A 1 KiB value is one 1,152-byte block, drawn from the same per-thread cache as a 1,000-byte
one. A 64 KiB payload is one classed block of 65,584 bytes, drawn from 1 MiB slabs. A publish to
more than `8 × kInlineFanout` subscribers snapshots its edges in one table block of 48 bytes per
subscriber; past about 1,365 subscribers that is an oversize draw on every publish, 384 KiB at
8,192.

### Latency

A 64 KiB draw costs 20.9 ns. An oversize draw is kept out of `in_use` because counting it would
raise that to 28.7 ns. The 65,584-byte row exists because a 64 KiB payload's one-block segment
would otherwise fall past the last row to the root, at about twice the cost of a classed draw. A
shared free-list pool on a 12-core host fell from 8.3 M to 1.36 M operations a second, and its
median from 60 ns to 3,587 ns, where the platform heap scaled: a contended cache line, which a
lock-free list head does not fix.

### Flash and RAM per target

| Target | Pool | Flash | RAM |
| --- | --- | --- | --- |
| NARROW | `pool_source_t` over a static slab | 340 B of text plus a 28 B vtable, riscv32 `-Os` | the slab you size |
| MID | same | same | same |
| WIDE | `slab_pool_t`, the host default | not measured | a 64 B header per slab, plus up to 2 free slabs per class |

## What the library does not do

- It does not round requests in `pool_source_t`. Rounding is `slab_pool_t`'s job.
- It does not trim slabs on its own schedule.
- It does not count an oversize block in `in_use`, so a node holding large values holds more
  than its census reports.

## Pitfalls

- **Symptom:** a `pool_source_t` refuses with most of its slab free. **Cause:** peers chose the
  sizes, so every length became its own class. **Fix:** put a `slab_pool_t` in front of it.
- **Symptom:** throughput falls as receive threads are added. **Cause:** one pool shared on a
  per-frame path. **Fix:** a pool per receiver; a mutex only at wiring frequency.
- **Symptom:** a wide fan-out costs far more per publish than a narrow one. **Cause:** the
  subscriber snapshot outgrew the last class. **Fix:** inject a root whose table reaches the
  widest snapshot.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope) leaves allocation to the
implementation; pools are not normative. The census a peer reads is in
[reference 05 §The seam census record](../../reference/05-protocol-tlvs.md#the-seam-census-record--stats),
which [v1 §3](../../spec/v1.md#3-wire-format) incorporates.

**Exercised by:** `mem_pool_source_test`, `slab_pool_test`, `mem_sync_pool_test`,
`mem_sync_policy_test`.

## History

:::{dropdown} How the pools got here
- [ADR-0065](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md)
  (2026-07-27) chose exact-size classes for `pool_source_t`, on the recorded trace.
- [ADR-0083](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md)
  (2026-10-03) made the slab pool the host default root and removed the 1 KiB cliff, where a
  1,072-byte segment had fallen past the platform heap's fast path at half speed.
:::

Next concept: [The static arena](arena.md). Go deeper:
[reference 09 §mem_slab_pool](../../reference/09-memory-substrate.md#mem_slab_pool), then
[a shared pool on a per-frame path](../../design/allocation-and-backpressure.md#a-shared-pool-on-a-per-frame-path-measures-worse-than-the-heap),
then the [backends module](../../modules/backends.md#the-host-slab-pool).
