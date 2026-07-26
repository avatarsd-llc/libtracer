# Per-vertex allocation arena

libtracer will not collapse a vertex registration's sub-allocations into a single
per-vertex arena block. The proposal was measured against `main` and the premise it
rests on does not survive.

## The premise

The request (#392) argued that the MCU per-vertex heap is dominated by **allocator
headers across the 5–7 separate sub-allocations a registration makes**, not by
`sizeof(vertex_t)` — citing ~50–90 B of headers per vertex, and proposing that a
registration draw its struct, name key, LKV value and ext block from one arena block
behind the existing `memory_resource` seam.

## Why this is out of scope

**The sub-allocation count is wrong, and shrinking.** The bench line the issue reasons
from contains its own refutation:

```
RESULT zeroheap vertex allocs=7 frees=6 bytes=136
```

Seven allocations, **six frees** — a bare leaf leaves *one* resident block. Decomposing
with a symbolizing probe, four of the seven were transient `path_t::parse` payload
growth (fixed independently in #546, which also cut `path_t::parse` 34→12 ns), one was a
transient by-value key copy, and one is caller-owned. Several sub-allocations the issue
lists no longer exist at all: the name key became a 16-byte inline SBO (`path_key_t`,
#380), the subscriber head is an empty `std::vector` costing nothing until the first
edge, and the path→vertex index node went away with ADR-0057's Composite tree.

**The header arithmetic is off by 6–20×.** On ESP-IDF the TLSF header is
`block_header_overhead = sizeof(size_t)` = **4 B** with `ALIGN_SIZE = 4`
(`components/heap/tlsf/tlsf_block_functions.h:79`), and `HEAP_POISONING_DISABLED` is the
default — so a block costs `align_up(req,4) + 4`, not `~8–12 + 8`. Measured against
rv32 sizes (`riscv32-esp-elf-g++ -Os -fno-exceptions -fno-rtti`: `vertex_t` 80,
`vertex_ext_t` 112, `value_handlers_t` 48, `subscriber_t` 40):

| shape | rv32 all-in | header + align |
|---|---:|---:|
| bare leaf | ~88–92 B | **4 B ≈ 4.5%** |
| handler-bearing leaf (3 blocks) | ~260 B | **12 B ≈ 4.6%** |

Headers are ~4.5% of per-vertex heap, against the ~30% the issue assumes.

**The measured-vs-`sizeof` gap has a different cause.** The ~270 B on-device bare-leaf
figure matches the *handler-bearing* arithmetic (~260 B) almost exactly and not the bare
one (~90 B). The gap the issue attributes to headers looks like the ext block — which
#388 independently judged irreducible.

**And the two largest blocks cannot live in an arena anyway:**

- The **LKV value is replaced on every write** (a probe on a second write shows 1 alloc +
  1 free, net zero). A per-vertex bump arena would grow without bound under a write
  stream. That path is already behind `mr_` and ADR-0060's `value_backend_` — covered
  twice over.
- The **handler seam is parked, not freed**: `retire()` swaps `e->handlers` to null and
  parks the old block in `graph_t::retired_seams_` for lock-free readers. A bump arena
  would leak every parked seam.

Honest saving from the whole mechanism: **4 B/vertex bare, ~16 B handler-bearing
(4.5–6%)** — against a change that must not break the parked-seam or per-write-swap
lifetimes.

## What was actually costing

The real lever underneath this issue was per-*operation*, not per-registration: every
path-keyed read, write and subscribe parses first, and `path_t::parse` grew its payload
by geometric doubling with no `reserve`. Pre-sizing exactly (#546) gave `path_t::parse`
**34→12 / 49→20 / 75→29 / 80→38 ns** for 1/2/4/8 segments and took registration from 7
to 3 allocations. No bench had caught it because `inproc-path` parses once and reuses
the `path_t`.

## What survives, tracked separately

Two narrower fragments were carried out of this issue rather than closed with it — see
the follow-up linked from #392. Neither is an arena.

## Prior requests

- #392 — "vertex allocation arena/pool: collapse the ~5-7 per-vertex sub-allocations into one block"
