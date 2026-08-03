# Transparent PMR: the node draws every non-hot-path allocation from an injected `std::pmr::memory_resource`, unifying with the L0 `mem_backend_t` seam — and "zero-heap" means the *steady-state forward hop*, not init

Status: accepted. **Refines [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)** (sharpens what invariants #2/#5 mean) and **generalizes [ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md)** (the `mem_backend_t` injection seam) to the container/tree layer. Brick 0 of the #83 Stage-2 flip: the memory-ownership contract the forward-path rewrite builds against.

## Context

Two clarifications surfaced while scoping Stage-2, and both are load-bearing enough to pin before code.

**1. "Zero-heap" was under-specified — it means the *steady-state forward hop*, not "never allocate."** [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) invariant #2 says "zero heap on the forward path." That was always meant as: a node that has *finished setup* forwards a frame with zero allocations. **Init / deinit / setup allocate freely** — pool construction, `:children[]` connection creation ([#82](https://github.com/avatarsd-llc/libtracer/issues/82)), catalog registration, the graph vertex map. So does the *host application* and the *user*; none of that is on libtracer's forward path and none of it counts. The `bench_forward_heap` gate ([ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md)) already encodes this precisely — it warms once *outside* the counter window, then arms the allocation counter around exactly one steady-state `on_frame`. The invariant is a property of *when* the counter is armed (steady state), not a claim that the process never mallocs.

**2. The memory-injection seam stops at L1.** [ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md)'s `mem_backend_t` already makes the zero-copy *byte* substrate host-owned: `pool_t` takes a caller slab, a device backend hands up GPU/pbuf memory, "libtracer never allocates on its own; it receives memory." But that seam covers only **L0 segments**. The **container/tree layer above it** — `wire::decode`'s `std::vector<tlv_t>` spine, `fwd_router_t`'s per-hop rebuild vectors, `route_handle_t`'s label `std::map`s, the graph's transient buffers — allocates from the **global heap** via plain STL, entirely outside the injection seam. So a 16KB ESP node can host-own its packet bytes but *not* the structures that parse and route them. That is the inconsistency: the stack is half-injectable.

## Decision

**Adopt `std::pmr` as the standard spelling of the container/tree injection seam, mirroring `mem_backend_t` for segments, so the *entire* stack draws from memory the host chooses — and unify the two into one model, not two.**

### 1. The node takes a `std::pmr::memory_resource*`, defaulting to the standard resource

`graph_t`, `fwd_router_t` (and the Stage-2 connection-vertex / terminus arena) take a `std::pmr::memory_resource*` at construction, **defaulting to `std::pmr::get_default_resource()`**. Explicit at the seam, transparent below: a 16KB node installs a `std::pmr::monotonic_buffer_resource` (or a pool resource) over a **static arena**; a server passes nothing and gets the standard heap; a NUMA / pinned-memory host passes its own. One code path; the host picks the memory.

**Not** a global `std::pmr::set_default_resource()`. That is process-global and breaks the instant two nodes share a process (the conformance tests, `bench_forward_heap`, two 16KB sims in one binary). Per-node explicit resource is the only choice that composes — and defaulting to the standard resource is **zero churn for every existing caller** (the parameter is added last, defaulted).

### 2. `mem_backend_t` (L0 segments) and `memory_resource` (L2+ containers) are ONE model, bridged

They are the same principle at two layers, not two competing seams:

- **`mem_backend_t`** stays the **L0 segment** seam — DMA-aware, device-memory-capable, the zero-copy byte substrate ([ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md)/[ADR-0024](0024-mem-cuda-gpu-backend-heterogeneous-rope.md)). Unchanged.
- **`std::pmr::memory_resource`** is the **L2+ container/tree** seam — the `tlv_t` spine at the terminus, the per-connection label tables, transient control structures.
- **One caller slab feeds both seams** so a 16KB node aligns its *entire* stack to host memory. **The container seam needs no new adapter class**: `std::pmr::monotonic_buffer_resource` (or `unsynchronized_pool_resource`) constructed over a caller-owned `std::array`/`std::span` **already is** the L2+ resource — that is what `std::pmr` is *for*. (Implementation note, corrected while scoping: `pool_t` is a *fixed-slot `segment_t`* allocator — `alloc(size)→segment_t*`, not `do_allocate(bytes, align)→void*` — so a "`pool_t`-backed `memory_resource`" is **not** a thin shim and **not** required; the two allocators serve different shapes. The segment seam stays `mem_backend_t`/`pool_t`; the container seam is a standard `monotonic_buffer_resource` over the same physical slab region.) So "one static slab, whole stack" = the caller partitions one slab into a `pool_t` region (segments) and a `monotonic_buffer_resource` region (containers) — each layer speaks its native seam, no bespoke bridge.

### 3. Non-viral — `wire::tlv_t` is untouched; PMR bites only where a tree is actually built

The elegant consequence of [ADR-0038](0038-net-plane-performance-model-two-plane-forwarding-and-buffer-lifetime.md) invariant #1 (**the forward hop builds no `tlv_t` at all** — it offset-dispatches on the first ~3 headers): making `tlv_t::children` a `pmr::vector` would not help the hot path, because the hot path constructs no `tlv_t`. So **`wire::tlv_t` stays exactly as it is** (heap-defaulted `std::vector<tlv_t>`, the shared cross-core codec type — no `pmr::` virality across the codebase, no conformance-vector churn). PMR appears at exactly two loci, both off the forward hot path:

- **The terminus arena** (invariant #5): a new `wire::decode_into(span, std::pmr::memory_resource&)` parses the frame into a **borrowed, arena-backed tree drawn from the node's resource** — *not* the heap-`std::vector`-spined `wire::tlv_t`. This is the reconciliation of the two constraints above: because `tlv_t::children` stays a plain `std::vector` (non-viral, §3 head), `decode_into` **cannot** populate a `tlv_t` from a `memory_resource` — a `std::vector` has no `memory_resource`. So the arena form is a *distinct* representation ([ADR-0037](0037-net-side-channels-dissolve-into-vertex-tree-compositor.md) inv. #5: *"a borrowed/pool-backed `tlv_t` tree, **not** `vector<tlv_t>`"*): a flat node arena (each node = `{type, opt, payload-span, first-child-index, next-sibling-index}`) allocated from the resource, with payload spans still zero-copy into the input. The resolver reads *this* view of the tree; the convenience `wire::decode(span) → tlv_t` (heap `std::vector`) is unchanged for callers that want the owning model. So "`decode_into` builds the tree from the resource" means the **arena tree**, never a `pmr::vector<tlv_t>` — the two are different types on purpose, which is why `tlv_t` needs no change. **The full arena design — node layout, the borrowed-span contract, span-aliased `path_key`, trailer-sliced stores, and the resolver rewrite — is pinned in [ADR-0041](0041-terminus-arena-decode-span-contract.md).**
- **The per-connection label tables** (invariant #3): the Stage-2 open-addressed `label → route` table is a `pmr`-allocated fixed-capacity structure sized by `:settings`, drawn from the connection's resource.

### 4. What the forward hot path allocates: nothing, from anywhere

The steady-state forward hop takes **neither** seam: no `tlv_t` (offset-dispatch), no container growth (stack `std::array` iov), and its fresh header bytes come from the **M2 `mem_pool_t`** (segment seam, fixed slab) — so even the "allocation" it does is a pool slot hand-out, not a `memory_resource::allocate`. Pool exhaustion is backpressure (drop / await), never OOM. The `memory_resource` is consulted only at the terminus and at flow-setup, both of which are allowed to allocate (§Context 1). **The gate is: `bench_forward_heap` reports 0 with the counter armed around a steady-state hop — from the global heap *and* from any injected resource** (the bench's `operator new` counter already catches both, since a `pmr` resource that falls through to the heap allocates through the counted `operator new`).

## Considered options

- **Global `std::pmr::set_default_resource()`.** Rejected: process-global, so two nodes in one process (tests, bench, multi-node sims) can't have different memory, and a stray library call could repoint the whole process's allocation. Per-node explicit resource composes; the global does not.
- **Make `wire::tlv_t::children` a `pmr::vector` (viral PMR through the codec).** Rejected: it changes the shared cross-core codec type (conformance-vector and TS/Rust-parity churn) to help a path that — post-invariant-#1 — builds no `tlv_t`. PMR belongs at the terminus arena and the label tables, not smeared through the codec.
- **Extend `mem_backend_t` to cover the container layer instead of adopting `std::pmr`.** Rejected: `std::pmr` is the *standard* container-allocator model every C++ dev and every STL container already speaks (the "modern C++ standard" the project targets); reinventing it on `mem_backend_t` would be a bespoke seam where a standard one exists. Keep `mem_backend_t` for what it is uniquely good at (DMA/device segments) and use `std::pmr` for containers — bridged by one adapter.
- **Leave the container layer on the global heap (do nothing).** Rejected: it is the exact half-injectable inconsistency this ADR exists to close — a 16KB node that host-owns its bytes but heap-allocates the structures that route them cannot bound its memory, which is the whole point of the 16KB target.

## Consequences

- **`bench_forward_heap`'s "0" is now precise**: zero allocations from the global heap *or* the node's `memory_resource` on the steady-state forward hop. Init/setup/terminus/host allocations are explicitly out of scope, measured by the armed window.
- **Stage-2 signatures gain a defaulted `std::pmr::memory_resource*`** on `graph_t`/`fwd_router_t`/the connection-vertex/the terminus arena — additive, defaulted to `get_default_resource()`, so no existing caller changes. Public API note in `core/CHANGELOG.md` when it lands.
- **New surface**: `wire::decode_into(span, memory_resource&)` (arena decode). The container seam itself needs **no new `tr::mem` class** — a standard `std::pmr::monotonic_buffer_resource` over a caller slab is the resource (see §2; the earlier "`pool_t`-backed adapter" is dropped — `pool_t`'s fixed-`segment_t`-slot shape does not fit `do_allocate`). `wire::decode(span)`, `mem_backend_t`, and `pool_t` are unchanged.
- **One slab, whole stack**: a 16KB node constructs one static arena → feeds the segment pool and a container `memory_resource` → every libtracer allocation (segments, terminus trees, label tables) comes from it. Host-aligned by construction, boundable, no global heap dependency.

## Errata — five corrections, all measured

Recorded after instrumenting a real terminus resolve (every `operator new` form, symbolized per
allocation), then extended (errata 4-5) while scoping #551. The seam works as designed; five
statements around it invite the wrong reading.

**1. `monotonic_buffer_resource` is the wrong injection, despite being named above.** The terminus
arena is destroyed each frame, but a monotonic resource never reclaims — a measured run exhausted a
1 MB slab after ~200k frames. The realistic injection for a steady-state node is
`std::pmr::unsynchronized_pool_resource` (or a host resource that genuinely recycles). The original
wording named the monotonic form because the ADR was reasoning about a *bounded* node's setup, not a
node running for days; both are now referred to as "a container `memory_resource`" above.

**2. The injected pool buys bounded RAM, not speed — and on a host it is SLOWER.** Measured on the
terminus path: 295 ns on the default heap versus 309 ns with a pool resource injected. glibc's
tcache serves a hot same-size malloc/free in tens of nanoseconds, which a general-purpose pool
cannot beat. The reason to inject is determinism and a bounded ceiling, which is what a 16KB target
needs; reading this ADR as a latency optimisation gets it backwards. (On an MCU allocator, where a
round-trip costs hundreds of nanoseconds rather than tens, the comparison inverts — but that is a
property of the *host* allocator, not of the seam.)

**3. The same `memory_resource` feeds per-frame and long-lived state, so it MUST NOT be reset per
frame.** `fwd_router_t` hands its `mr_` both to the per-frame terminus arena and to the long-lived
`route_handle_t` label tables. A host that "resets the arena between frames" — the natural reading
of a per-frame arena — would free live label state. Nothing in-tree does this and no defect is
being claimed here; the point is that the safety of a per-frame reset is not a property a host can
assume from this ADR's framing, and today it does not hold. Separating the two resources would make
per-frame reset safe, and is a live option rather than a decision recorded here.

**4. The "init/setup is out of scope" carve-out no longer covers vertex registration, and the
registration path never drew from `mr_` in the first place.** §Context 1 lists "the graph vertex
map" among the init-time allocations this ADR deliberately exempts, and under that reading
registering a vertex was fine: it happened once, at startup, before the counter was armed.
[RFC-0014](../spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) ended that.
`transport_vertex_t::make_connection` calls `graph_t::register_vertex_key` when a CREATE op
resolves, on whichever transport thread received the frame — so vertex allocation is now a
**runtime, wire-driven** operation a *peer* drives. This is the same premise-invalidation
[ADR-0063](0063-connection-table-lock-free-reads-trait-serialized-writes.md) recorded for the
connection registry ("immutable after setup" stopped being true when RFC-0014 made create/remove a
runtime operation); the registry got a mechanism, the allocation path did not.

Measured on that path (`bench_forward_heap`'s `reg_escape` probe, a `/net/<module>/<name>`
registration with a peer-length name and one handler): **four global-heap blocks per registration,
and the injected resource serves zero of them.** The probe is ratcheted, so the number cannot drift
once it moves. Tracked as [#551](https://github.com/avatarsd-llc/libtracer/issues/551).

**Ruling (2026-07-28, maintainer): vertex allocation is deliberately OUTSIDE this seam.** The
carve-out stands — but it no longer rests on "init/setup", which is the wording RFC-0014 falsified.
It rests on a measurement.

Routing the registration through `ctl_` requires the allocation's owner to know which resource
freed it, i.e. a source-carrying deleter on `std::unique_ptr<vertex_t>` (`graph.hpp:1083`). Sizes
read out of the compiler on the deployment target (`riscv32-esp-elf-g++ 15.2.0`,
`-march=rv32imac_zicsr_zifencei -mabi=ilp32 -Os -fno-exceptions -fno-rtti -std=c++23`):

| ownership | rv32 | x86-64 |
| --- | --- | --- |
| `std::unique_ptr<vertex_t>` today | **4 B** | 8 B |
| with a stateful, source-carrying deleter | **8 B** | 16 B |
| with a stateless deleter (empty-base optimised) | **4 B** | 8 B |

Every vertex but the root sits in exactly one parent's `children_->sorted`, so the stateful form
costs **+4 B per vertex** — against a benefit measured at *~4 B of allocator header per vertex*. It
pays exactly what it saves, and it additionally changes the ownership type of both `vertex_t` and
`vertex_ext_t` at ~7 sites (a public API break) while a release deleter has no owner to run it,
because no `~graph_t` exists in `core/` ([#576](https://github.com/avatarsd-llc/libtracer/issues/576)).
The stateless form is free in bytes only by resolving a **process-wide** source, which contradicts
the per-graph injection this seam exists for.

So the honest statement is not "registration is setup" — it is *"per-vertex bytes are the scarcer
resource than seam purity, and the seam buys nothing here."* A host that needs every vertex byte
drawn from its own resource is asking for a different ownership model, not a different call site.

**Consequence to carry, so this is not re-derived from the stale wording:** the registration path
*is* wire-driven and peer-reachable (`transport_vertex.cpp:274` reaches it from a CREATE), so the
bound on how many vertices a peer can cause to be allocated is **not** supplied by this seam
(`transport_vertex.cpp:277` reaches `register_vertex_key` from a resolved CREATE). That bound
belongs to the connection/creation admission surface RFC-0014 defines, and is where it must be
enforced.

**Trap for any future attempt.** Do not "just route the root allocation first": `graph.hpp:1083`
declares `root_`, `graph.hpp:1129` declares `ctl_`, and C++ initialises members in **declaration**
order regardless of the constructor's init-list — so routing `graph.cpp:268` reads `ctl_` before it
is constructed. Silent UB that a debug build hides, in a codebase already bitten by "`-Os` deletes a
null check no test covers".

**5. `std::pmr::memory_resource` cannot report allocation failure by value, which makes this seam
unusable for a failable operation on the `-fno-exceptions` profile.** `allocate` is specified to
return storage or throw; there is no null return. ESP-IDF builds with exceptions off (the IDF
default, and a commitment in `integrations/esp-idf/README.md`) and link-wraps `__cxa_throw` /
`__cxa_allocate_exception` to `abort()` stubs. So on the deployment profile, an injected resource
that runs out has exactly one way to say so: reboot the node.

This is not hypothetical. The shipping integration's resource ends its failure path with an
`abort()` and a log line, and its comment states the reason outright: *"pmr cannot report failure
by value and the target builds with `-fno-exceptions`: fail loudly WITH the numbers instead of the
bare bad_alloc-abort this replaces."*

The consequence for anyone reading this ADR as a bounding mechanism: **`mr_` bounds memory, it does
not make exhaustion survivable.** The seam that does is the sibling one this ADR §2 bridges to —
`mem_backend_t::create()` is nothrow and returns `nullptr` (see
[ADR-0060](0060-lkv-copy-store-injected-value-backend.md)), and `vertex_t::try_make_lkv` is that
discipline applied to `mr_` on the one path that already needed it, with a docstring conceding the
gap for injected resources. Closing it for the rest of the stack requires either tightening the
`mr_` contract to nullptr-on-exhaustion (making an injected resource formally non-conforming, which
any resource usable on this profile already is) or routing failable control-plane allocations
through a nothrow seam instead. That choice is open, not recorded here.
**Erratum 6 below closes it** — the second option was taken.

**What the seam does deliver, measured.** Injecting a resource moves the terminus decode off the
global heap exactly as designed — 9 allocations / 937 bytes becomes 4 / 153, i.e. 84% of the bytes
redirected, with no decode leg bypassing the seam. The residual legs are on *other* deliberate
seams (the reply head draws from the ADR-0042/0060 `value_backend_` mem-backend, and the egress
span table uses the plain allocator), so a fully bounded node injects both knobs, not one.
- **The Stage-2 bricks are unchanged in order; this fixes their memory contract**: Brick 1 (kill the forward-path full-decode) and Brick 2 (pooled header rebuild) drive the bench to 0 *from any resource*; Brick 3 (the structural split + label tables) draws its per-connection state from the node's `memory_resource`.

## Erratum 6 — the choice erratum 5 left open was made: a separate nothrow seam, not a tightened `mr_`

*(2026-07-27, ratified with [#551](https://github.com/avatarsd-llc/libtracer/issues/551) Q1–Q3; shipped in the same range.)*

Erratum 5 named two ways to close the gap and recorded neither. The second was taken, and the first turns out **not to be implementable on the profile that ships**: `std::pmr::memory_resource::allocate` is annotated `__attribute__((__returns_nonnull__))` in libstdc++, so a caller's `if (p == nullptr)` is undefined behaviour — and measured on `riscv32-esp-elf-g++ 15.2.0` it is **deleted at `-Os`/`-Oz`** while surviving at `-O0`/`-O1`/`-O2`/`-O3`. `-Os` is what the reference node ships (the IDF default is `-Og`), and no job executes an allocation-failure path at it — so tightening `mr_`'s contract would have replaced a diagnosed reboot with an undiagnosed null-deref and a false SUCCESS on the wire. (See ADR-0065 §1 for the exact CI picture; an earlier revision of this erratum over-claimed that nothing runs at `-Os` at all.)

The seam, its rejected alternatives (including a variant *deriving* from `memory_resource`, refuted on a different probe) and the measurements are in **[ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md)**.

Two consequences for the text above:

- **§3's `decode_into(std::span, std::pmr::memory_resource&)` signature is superseded.** It now takes `tr::mem::block_source_t&` ([#588](https://github.com/avatarsd-llc/libtracer/issues/588)), because the terminus arena is built from a peer's frame behind no ACL and could abort on exhaustion.
- **The Consequences' "no new `tr::mem` class is needed" no longer holds.** `block_source_t`, `heap_source_t`, `null_source_t`, `bump_source_t` and `block_array_t<T>` are exactly that class of addition. The reasoning it rested on — that object construction is `std::pmr`'s job — survives for allocations that **cannot fail at runtime**; it does not survive for the ones a peer provokes.
- **Erratum 1's premise moved.** Its advice (inject an `unsynchronized_pool_resource`, not a `monotonic_buffer_resource`, because the terminus arena is the per-frame consumer that never frees) now targets a consumer that has **left `mr_`**. `mr_`'s remaining per-frame consumer is the LKV control block; the arena draws from the block seam, and a bounded node bounds it with a `bump_source_t` over its slab.
