# 67. A bounded seam recycles through segregated exact-size classes, and scales by giving each owner its own source rather than by locking a shared one

Status: accepted (maintainer-ratified 2026-07-28). Closes the gap [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md) left in its own *Outstanding* list and that [#597](https://github.com/avatarsd-llc/libtracer/issues/597) tracks. Upholds [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §1 (the compile-time/runtime appropriateness rule) and [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md) (bounds are injected resources, never magic constants). Acts on [ADR-0060](0060-arena-route-pooled-value-backend.md) erratum 1, whose measurement makes the second half of this decision non-optional.

## Context

ADR-0065 gave failable allocation its own seam, `tr::mem::block_source_t`. The three sources shipped with it do not span the space a bounded node needs:

| source | bounded | recycles |
| --- | --- | --- |
| `heap_source_t` | no | yes |
| `null_source_t` | — | — |
| `bump_source_t` | yes | **no** |

A **long-lived** seam — `graph_t::ctl_`, `fwd_router_t::rx_` — therefore could not be bounded at all. A bump block is never reclaimed, which is correct for a scope-lifetime consumer and fatal for a long-lived one. Measured: an 8 KiB bump source wired as a router's `rx`, decoding a 53-byte FWD, served **six frames and rejected the next 194**. The node does not abort — the seam behaves exactly as specified — it simply stops working.

That left the published bounded-node recipe saying *"write your own, or leave it on the heap"*, and it matters beyond tidiness. The project's stated goal is that a deployment can **configure** a minimal RAM footprint. A separate measurement taken while pursuing that goal found libtracer's own static RAM to be approximately zero: a Cortex-M0 sentinel's 4,580 B of `.bss` traced through the linker map to `crt0 → exit() → __stdio_exit_handler → libc_a-findfp.o` — the C runtime, not this library. So "minimal footprint" is not reachable by shrinking a static image. It means *an injected bound actually holds*, and today no long-lived seam can hold one.

## Decision

### 1. Ship `pool_source_t` — segregated exact-size free lists over a caller slab, zero per-block header

The seam's `release(p, bytes, align)` hands the size back, so a block needs no header to be reclaimed. Free lists are keyed on the exact normalized `(bytes, align)` pair; the link lives in the free block itself.

Both bounds are injected, per RFC-0006: the caller supplies the slab **and** the span of `size_class_t` slots, so neither the byte ceiling nor the class count is a constant in this library. Running out of class slots is safe but lossy — the block stays carved and `overflowed()` counts it — and `classes_used()` is the figure a deployment sizes the span against.

### 2. The shape is chosen on the measured demand, which is nearly degenerate

Every `try_alloc`/`release` across the host suite was recorded through an instrumented source: **70,937 events, 12 distinct sizes, three of which cover 99.8 % of all allocations.** They are the arena's geometrically growing arrays (48×n and 8×n, doubling). Alignments are only 4, 8, 16, 64.

Replaying those traces per-process against each candidate — peak slab needed to serve every request:

| policy | slab | vs peak-live floor (23,552 B) |
| --- | ---: | ---: |
| segregated exact-size classes | **26,176 B** | **+11.1 %** |
| first-fit + boundary-tag coalescing | 27,448 B | +16.5 % |
| TLSF (4 B header + 2nd-level rounding) | 28,440 B | +20.8 % |

With only 12 sizes, exact classes give **zero internal fragmentation**; the +11.1 % is entirely the inability to reuse a freed 64 B block for a 128 B request. Coalescing can do that and still loses. Decomposing why, by re-running the replay with the header zeroed: **1,088 B of external fragmentation against only 184 B of header.** Splitting a remainder under geometric growth rarely produces the size of the next request.

That decomposition also **retires the usual argument for a header-free pool**, which this ADR would otherwise have leaned on: peak live blocks across all five processes is **19**, so the header axis is worth 0.7 % of the difference. The reason to choose this shape is fragmentation behaviour under geometric growth, not the absent header.

Latency is **not** a discriminator. Replaying the two largest traces: 9.8 vs 9.2 ns/op and 19.9 vs 20.6 ns/op — the two policies swap places, and the replay harness's own lookup dominates both. The expected "alloc is a pointer pop" win does not appear at this call frequency and is deliberately not claimed.

### 3. Scale by ownership, not by synchronization — a shared pool on a hot path is measured-worse than the heap

ADR-0060 erratum 1 measured a shared free-list pool on a 12-core host:

| threads | pool ops/s | pool p50 | heap ops/s | heap p50 |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 8.3 M | 60 ns | 15.8 M | 30 ns |
| 4 | 3.6 M | 802 ns | 25.5 M | 70 ns |
| 8 | **1.36 M** | **3587 ns** | 31.0 M | 70 ns |

It collapses to roughly **a fifteenth of its own single-thread rate** — a cacheline storm, not serialization — while glibc's per-thread tcache *scales*. The erratum further records that a lock-free `[index | ABA-tag]` CAS **would not have fixed it**: it replaces one contended word with the same word.

`pool_source_t` is structurally that same object. Therefore:

> **A `pool_source_t` is owned by one thread wherever it sits on a per-frame path. Sharing one behind a lock is admissible only at wiring frequency.**

The synchronization policy is a template parameter, defaulting to the empty `sync_none_t`, which compiles to nothing under `[[no_unique_address]]`. This passes ADR-0047 §1 on both limbs: the identity is per-target build configuration, and the mechanism's *existence* costs code size on MCU profiles. A target instantiates only what it injects. `sync_mutex_t` lives in a separate `mem_source_sync.hpp` so `mem_source.hpp` stays freestanding-clean for the footprint sentinel; a single-core FreeRTOS target supplies an interrupt-disable policy of its own, and the seam asks only for `lock()`/`unlock()`.

### 4. `heap_source()` remains the default, and that is the honest answer for a host

This ADR makes boundedness **reachable**, not default. A host that does not want a ceiling should keep `heap_source()`: it is nothrow, it recycles, and it is measured to scale (31 M ops/s at T=8). A bounded source is a deployment choice for a node with a RAM ceiling — the MCU, and any host that prefers deterministic exhaustion to the OOM killer.

## Consequences

- The `bump_source_t` warning that a recycling source "does not exist yet" is retired; the bounded-node recipe can stop saying *"write your own"*.
- Shipped cost, `riscv32-esp-elf-g++ -Os -fno-exceptions -fno-rtti`: **322 B** of text plus a 24 B vtable. The 256 B / 380 B figures used to choose between policies were feature-matched prototypes carrying neither the alignment key, the overflow counter, nor the foreign-pointer check, and are not the shipped cost of either shape.
- A target that does not instantiate it pays **nothing**, verified rather than assumed: the Cortex-M0 required-modules sentinel — which links `mem_source` — reports **20,937 B flash / 4,580 B RAM byte-identically** before and after this change. That is what §3's template-policy shape buys, and it is why `sync_mutex_t` is in a separate header.
- **`fwd_router_t` is left non-conforming to §3 by this ADR and must be fixed by a follow-on.** It holds a single `rx_` (`fwd_router.hpp:461`) while its own class comment states that `on_frame` fires on transports' receive threads *"possibly several concurrently"* and that *"no per-request locking is required"* (`:63-64`). `rx_` is the one mutable object on that path, so injecting a shared pool there today would put a contended lock onto a deliberately lock-free per-frame path. The fix is available and cheap — `child_rx_ctx_t` already exists one-per-child in a pointer-stable deque (`:464`), and each transport has its own receive thread (`:472`) — but it changes the router's construction API and is therefore its own slice. **Until it lands, do not inject a `pool_source_t` as a router's `rx`.**
- That per-child topology carries a condition worth stating rather than assuming: it is synchronization-free **iff** a transport delivers on a single receive thread. A transport using a delivery pool needs a locking policy, which is a per-transport property.
- `ctl_` may be shared with a locking policy. Note that the 59,262 seam events in `net_control_plane_race_test` are a deliberate race hammer and must **not** be read as production registration frequency — including by the author of this ADR.

## Considered options

- **Ship nothing; let each caller write its own.** Rejected: the reference bounded-node example cannot bound its terminus arena, and every embedder would re-derive the same 322 B.
- **First-fit with coalescing.** Rejected on measurement: larger slab (+16.5 % vs +11.1 %) on a workload whose sizes are geometric, which is the case coalescing handles worst.
- **TLSF.** Rejected: largest slab of the three (+20.8 %), and ~2-3 KB of code by literature report for generality this demand does not exercise. Not measured here, and should not be quoted as if it were.
- **One shared pool with a lock-free CAS free list.** Rejected on ADR-0060 erratum 1's explicit finding that this does not address the contention it appears to address.
- **A per-thread magazine layer inside the allocator.** Rejected as premature: the same effect is available for free by scoping the source to its owner, and an internal magazine would add exactly the shared structure §3 exists to avoid.
