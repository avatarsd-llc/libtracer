# The static arena

## In one paragraph

On a build that binds `kSlabPool = false`, as the ESP-IDF component and the Cortex-M0 footprint
node do, the default root is a static arena: one array of `kArenaBytes` in `.bss`, so the linker
map shows exactly what the library holds by default. Three sub-pools, for values, tables and the
net plane, carve blocks from it front to back, each into the classes of the build's size-class
table, and recycle what they carved on their own free lists. Nothing ever goes back to the
arena, so the RAM map is fixed at build time. An exhausted arena refuses: a write answers
`BACKPRESSURE`, and a registration at init stops with a message naming the sub-pool and the bytes
it needed.

## The picture

```{mermaid}
:caption: "Legend: purple = application, green = vertex, sky blue = record."
flowchart LR
    accTitle: The arena root and its three sub-pools
    accDescr: One static array of kArenaBytes is carved front to back by three sub-pools, values, tables and net. The graph draws values and tables; the router and transports draw from net when nothing else was injected. Each sub-pool keeps its returned blocks on its own free lists and reports its census as a stats record. The application reads the census to size the arena.
    R["static arena, kArenaBytes in .bss"] -->|"carve"| V["values"]
    R -->|"carve"| T["tables"]
    R -->|"carve"| N["net"]
    G("graph_t"):::vertex -->|"draw"| V
    G -->|"draw"| T
    V --> S[":stats.mem.values, .tables, .net"]:::record
    S -->|"peak sizes the arena"| A([application]):::app
```

## When you need it

On any MCU build that leaves a default in place: a `graph_t` built without a source, a link or
transport vertex given no receive or egress source, or a default backend that takes no graph.
Size the arena from the node's peak, not from free memory at boot.

## How it works

**Carving.** The arena moves a cursor and never takes a byte back. A sub-pool asks it for a
fresh block only when the class's free list is empty, and a released block goes onto that list,
reused by that class alone. A request larger than the last row is carved at its own size and,
once released, serves only a later request of exactly that size: the fixed-size large buffers a
node asks for, such as a datagram receive buffer, without a row for each.

**Free-list heads.** Each sub-pool keeps one head per row no larger than `kArenaBytes`, since a
larger class could never be carved. The heads sit in `.bss` and the table in `.rodata`; neither
is copied into initialized RAM.

**Exhaustion.** At run time a refused draw is counted and the operation answers by value:
`try_register_vertex` returns an error, a write answers `BACKPRESSURE`. `register_vertex`, the
infallible init form, cannot return one; past the arena it prints the sizing message and stops,
which on a board is a reset.

**Reading the census.** `tr::mem::default_root().stats()` reports `capacity` as the region and
`in_use` and `peak` as the bytes carved. A `peak` of 0 after a full run means nothing reached
the arena: every default was redirected, and the arena can be lowered, or removed with the
heap-rooted build ([the zero-heap profile](zero-heap.md)). Each sub-pool's own census is its
`:stats.mem.*` record.

**Sizing.** With every source injected (the graph's root, the router's planes, the transport
vertex's receive and egress stores) the arena's minimum is 0 bytes, measured on a bus connection
created, published and listed with two peers. The ESP-IDF example
`integrations/esp-idf/examples/concepts/arena_sizing` sets an 8 KiB arena, fills it, and
shows both refusals.

## Cost

### Wire bytes

None.

### Allocations

None at run time beyond the sub-pools' own: a block comes from a free list, or the cursor. The
arena is reserved whether or not it is used, and it never shrinks.

### Latency

A classed draw is one free-list pop under the sub-pool's lock, which `tr::no_guard_t` removes on
a single-threaded node. No bench times the arena on silicon.

### Flash and RAM per target

| Target | Arena | Free-list heads |
| --- | --- | --- |
| NARROW | `kArenaBytes`; the Cortex-M0 footprint node binds 2,048 B | 3 per kept class, 4 B each on 32-bit |
| MID | `CONFIG_LIBTRACER_ARENA_BYTES`: 32 KiB default, 1 KiB to 4 MiB | 576 B at 4 KiB, 864 B at 32 KiB, on esp32c6 |
| WIDE | not compiled: the host default is the [slab pool](pools.md) | none |

The ESP-IDF Linux target defaults to a 256 KiB arena, because it runs the examples' host peers.

## What the library does not do

- It does not trim the arena or move blocks between classes.
- It does not fall back to a heap when the arena refuses.
- It does not size the arena for you; the number is a build constant.

## Pitfalls

- **Symptom:** every UDP datagram is dropped, counted in `dropped_rx`. **Cause:** a link's SPEC
  raised `max_frame` toward 64 KiB on the default receive backend, so each receive buffer is that
  large and a 32 KiB arena refuses it. **Fix:** give the link a bounded receive backend, or keep
  `max_frame` within the arena.
- **Symptom:** a board resets during start-up after a feature was added. **Cause:** an init
  registration ran past the arena. **Fix:** raise `kArenaBytes` by the bytes the sizing message
  names, or inject a source.
- **Symptom:** RAM is reserved that the node never uses. **Cause:** every source is injected and
  the default root's `peak` is 0. **Fix:** lower the arena, or build heap-rooted.

## Where the rules live

**Normative source:** [v1 §1](../../spec/v1.md#1-scope) leaves memory to the implementation;
the arena is not normative.

**Exercised by:** `mem_arena_test`, `graph_oom_softfail_test`.

## History

:::{dropdown} How the arena got here
- [ADR-0083](https://github.com/avatarsd-llc/libtracer/blob/main/docs/adr/0083-one-allocation-seam.md)
  (2026-10-03) made the static arena the MCU default root, replacing the platform heap there,
  and later added the heap-rooted opt-out with `kArenaBytes = 0`.
:::

Next concept: [The zero-heap profile](zero-heap.md). Go deeper:
[reference 09 §mem_slab_pool](../../reference/09-memory-substrate.md#mem_slab_pool), then the
[backends module](../../modules/backends.md#the-host-slab-pool), then
[sizing a bounded seam](../../design/allocation-and-backpressure.md#sizing-a-bounded-seam).
