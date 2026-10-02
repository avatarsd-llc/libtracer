# One allocation seam: every core allocation draws from one injected block source, placed by one module

<!-- status: accepted -->

Status: **accepted** (maintainer-ratified 2026-10-03, closing a three-round grilling held 2026-10-02 and 2026-10-03, rulings Q1–Q16). **Supersedes in part [ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md)** (§1, §2's container half, §3's `tlv_t` clause, the init carve-out of §Context 1, and the `std::pmr` considered option; see §Supersession and amendments). **Amends [ADR-0056](0056-vertex-handle-infallible-register.md)** (what an abort in `register_vertex` means, Q7) and **[ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md)** (the default composition and the hazard-node carve-out, Q3/Q6/Q8). Amends [RFC-0028](../spec/rfcs/0028-lean-value-path.md) §4.9 and §5.6 by its own amendment. Builds on [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md) (`block_source_t`, failure by value), [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) (compile-time seams) and [ADR-0068](0068-build-configuration-is-plain-cpp-config-header.md)/[ADR-0070](0070-configuration-is-a-named-traits-type.md) (`config_t`).

**No wire surface moves.** `docs/spec/v1.md` is untouched. Every change here is to the reference implementation's C++ surface (implementation-defined per [ADR-0013](0013-v1-scope-boundaries.md)).

## Context

### The trigger: a 1 KiB cliff that one layout choice made

[RFC-0028](../spec/rfcs/0028-lean-value-path.md) slice 10 ([#1660](https://github.com/avatarsd-llc/libtracer/pull/1660)) puts the segment header in the same block as the payload, as §4.9 asks. For a 1,024 B value the heap adapter therefore asks for `malloc(1072)`. glibc's tcache serves requests up to 1,032 B, so that one request falls off the fast path. bench-local shows a 2x slowdown on the 1 KiB heap rows. The fix that stops the bleeding is [#1768](https://github.com/avatarsd-llc/libtracer/issues/1768): the heap adapter splits header and payload above a threshold, with a request-size test and a 1 KiB perf gate. It lands first, on v0.18.0, and does not wait for this ADR.

The regression is a symptom. Whether a header shares the payload's block, how padding is rounded and when to split are decided in **five places**, and the padding rule is spelled three ways:

- `backend.hpp:mem_backend_t::alloc_in_block` (and `mem_backend_t::alloc` around it);
- `mem_heap.hpp:heap_backend_t::alloc`, which bypasses `alloc_in_block`;
- `core/src/mem_source_backend.cpp:source_backend_t`, with its own `kHeaderBytes`;
- `core/src/mem_pool.cpp:pool_t`, with its own header and stride;
- `value.hpp:value_t::make_inline`, with its own recipe.

The receive-loan offsets also leak out of this layout into `length_prefix_framer.hpp`, `value.hpp`, `alloc_rx` and the TCP test. A size-class boundary in any one allocator changes the right answer for all five, and no one module owns the answer.

### The larger problem: the seam does not cover the core

[ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md) and its 2026-08-27 amendment reduced the graph's injection to one `block_source_t`. The rest of core still allocates around it. At `origin/main` on 2026-10-02 core has 402 `std::vector` uses (54 files), 180 `std::string` (36), 147 `new` (37), 116 `unique_ptr`/`make_unique` (29), 30 `std::function` (14), 14 `std::map` and 8 `std::deque`. Two named escapes are deliberate today: `lkv_slot.hpp:acquire_node` uses global `new` (the [#873](https://github.com/avatarsd-llc/libtracer/issues/873) phase-2 carve-out, priced at +22.7 % on the injected source), and the `link_index` outer tables come from the process heap (found by [#1758](https://github.com/avatarsd-llc/libtracer/issues/1758)). The std-interop adapters are `mem_source_alloc.hpp:source_allocator_t`, which throws on refusal, and `mem_source_pmr.hpp`, used from 17 files.

On an MCU, `std::vector`, `malloc` and global `new` are not usable allocation sources: a static-arena node cannot bound what the core takes from a heap it does not want to have. So "an injected source bounds the node" is true only for the paths RFC-0028 already moved.

## Decision

1. **One seam for every core allocation (Q1).** Every allocation core makes draws from the injected `block_source_t`. This includes init, registration, tables, values, LKV nodes and constructor roots. A link-time check enforces it: the MCU build of `libtracer.a` references no `malloc`, `free`, `operator new` or `operator delete`. Host-only tools and tests are outside the check.

2. **Core gets its own small set of failable containers (Q2).** Core uses a vector, a name/string store and a sorted-vector map, all built over `block_source_t` and all reporting refusal by value. It does not use std containers with a throwing allocator, and it does not use `std::pmr`. Host-only tools and tests may keep std.

3. **One injected root per graph; the library derives the sub-pools (Q3, Q6).** A graph takes one root source. From it the library derives per-purpose **sub-pools** (values, tables, net) so it can keep accounts and enforce caps per purpose. That one root is the default. Per-plane and per-thread are opt-in sub-pool layouts the library derives from the same root, not separate sources the deployer must wire. This deliberately reverses the 2026-08-20 ruling that no composition is the default.

4. **The default source per target (Q4).** On a host the default is a size-classed slab pool over the system heap. On an MCU the default is a compile-time static arena with no heap behind it.

5. **One placement module owns the layout, against a `config_t` size-class table (Q5).** One module owns the header, the padding and the choice between one block and a split, for every allocator. The size-class table is a `config_t` trait with a default table. The placement module is the first step of the migration (Decision 12).

6. **Release on the host, never on the MCU (Q11).** The host slab pool keeps a high-water cap per size class, releases a fully free slab above that cap, and offers an explicit `trim()`. The MCU arena never trims.

7. **Exhaustion at init is a sizing bug (Q7).** `register_vertex` stays infallible. When the root cannot satisfy it, the node aborts with a message that names the sub-pool and the bytes needed. `try_register_vertex` stays failable, by value, for runtime registration. ([ADR-0056](0056-vertex-handle-infallible-register.md) gains this clause.)

8. **LKV nodes move onto the pool (Q8).** Hazard/LKV nodes get their own fixed-size class and a per-thread free list. The acceptance bar is within ±3 % of global `new` on bench-local. This ends the #873 phase-2 carve-out.

9. **Callbacks without heap (Q10).** `std::function` leaves core. Synchronous calls take a non-owning `function_ref`. Stored callbacks use inline storage sized at compile time, and a callable that does not fit fails to compile.

10. **Locking and counters (Q13, Q14).** Each size class has one lock, taken through the config `guard_t`; a single-threaded MCU uses `no_guard_t`. There are no per-thread caches, except the LKV node free list of Decision 8, until a bench shows contention. Each sub-pool reports in-use bytes, a high-water mark and refusals through the `:stats` introspection subtree; per-class detail is behind `kInstrumentCounters`.

11. **No owning std type crosses core's public API (Q15).** Inputs are views. Outputs go into caller buffers or core containers. This is a breaking change, so every migration batch carries `CHANGELOG.md` migration lines.

12. **Expand, migrate, contract (Q12, Q16).** The order is: (1) the placement module; (2) the size-classed pool and the host default; (3) the container set; (4) migration batches by directory; (5) the LKV node move; (6) the link check and the MCU arena default. [#1768](https://github.com/avatarsd-llc/libtracer/issues/1768) lands first and ships in v0.18.0. The train is v0.19.0, "one allocation seam": this ADR and the glossary first, then the steps above. The lean is to land it before [RFC-0029](../spec/rfcs/0029-one-path-primitive.md) S2 and later; that is the maintainer's call at the S2 stop.

## Considered options

- **std containers with a throwing allocator** (`source_allocator_t` everywhere). Rejected: core builds `-fno-exceptions`, so a refusal becomes an abort, and the failure-by-value contract of [ADR-0065](0065-failable-allocation-gets-its-own-seam-block-source.md) is lost on exactly the paths a peer can provoke. It also keeps std's growth policy, which the placement module could not see.
- **`std::pmr`.** Rejected for the reason ADR-0065 §1 measured: `memory_resource::allocate` is `returns_nonnull` in libstdc++, so a null check is undefined behaviour and is deleted at `-Os` on the shipped toolchain. The pmr containers also carry a resource pointer per container and a virtual call per allocation that the core containers do not need.
- **Injection per plane** (the deployer wires one source per plane or per thread). Rejected as the default: every deployer must know the planes in order to get a bounded node, and the library cannot account per purpose for sources it did not derive. Kept, as an opt-in sub-pool layout the library derives from the one root (Decision 3).
- **Keep the global-`new` carve-out** for LKV nodes and the constructor roots. Rejected: one carve-out is enough to make the link check impossible, and it is the reason an MCU node cannot be bounded. The #873 measurement priced the naive move; Decision 8's own size class and per-thread free list are the answer to that price, with a ±3 % bar.

## Consequences

- The cliff #1660 hit cannot recur silently. A size-class boundary is a row in one table, read by one module, and the request-size test pins the requests each payload size makes.
- "An injected source bounds the node" becomes true for the whole core, not just the hot path, and the MCU link check proves it.
- The composition vocabulary keeps its words with a new default: one root, with per-plane and per-thread as derived layouts. CONTEXT.md §Block source and §Store composition say so.
- The public C++ API breaks batch by batch (Decision 11). The wire does not.
- Hazard/LKV node acquisition moves onto the pool and must hold within ±3 % of global `new`. If it cannot, the batch does not merge.
- `source_allocator_t` and `mem_source_pmr.hpp` survive only as host-side interop for tools and tests. Core does not use them.

## Supersession and amendments

- **[ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md), superseded in part.** Superseded: §1 (the node takes a `std::pmr::memory_resource*`); §2's claim that `std::pmr` is the container seam; §3's clause that `wire::tlv_t` stays a heap-defaulted `std::vector` spine (it is a core allocation, so it draws from the seam); §Context 1's "init / setup allocate freely" (they allocate freely *from the seam*); the rejected option "extend `mem_backend_t` instead of adopting `std::pmr`"; and Erratum 6's reservation that construction stays `std::pmr`'s job for allocations that cannot fail at runtime. Stands: §4 (the steady-state forward hop allocates nothing, from anywhere), the "one slab, whole stack" aim, and Erratum 8's lifetime rule, which now applies to the injected root.
- **[ADR-0056](0056-vertex-handle-infallible-register.md), amended.** Decision 7's clause on root exhaustion at init.
- **[ADR-0079](0079-allocation-store-composition-defaults-to-per-plane-mid.md), amended.** The 2026-08-20 amendment's "no composition is the default" and "un-wired → all-heap" give way to Decisions 3 and 4. The 2026-08-27 amendment's phase-2 carve-out gives way to Decision 8, and its other carve-out (the `std::vector<std::byte>` sites fixed by signatures) gives way to Decision 11.
- **[RFC-0028](../spec/rfcs/0028-lean-value-path.md), amended** (§4.9 and §5.6): placement moves into the placement module, and the block-source contract widens to every core allocation.

## Verification

- **The link check.** A CI job inspects the MCU `libtracer.a` (`nm` over the archive) and fails when it finds an undefined reference to `malloc`, `calloc`, `realloc`, `free`, `operator new` or `operator delete`, in any variant. It turns on with migration step 6 and stays on.
- **The request-size test.** Introduced by #1768 and kept by the placement module: for each payload size across the size-class boundaries (including 1,024 B and the 1 KiB/4 KiB/16 KiB rows), it asserts the exact request each allocator makes. A layout change that moves a request across a class boundary turns it red.
- **The ±3 % LKV bar.** On bench-local, hazard/LKV node acquisition and the steady store rows are within ±3 % of global `new`, judged best-of-rounds. Each migration batch also reports the perf, latency and RAM gates and the >1 KiB rows.
