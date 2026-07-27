# Failable allocation gets its own seam: `tr::mem::block_source_t`, because `std::pmr` cannot carry a failure signal on the profile that ships

Status: accepted (maintainer-ratified 2026-07-27 across three grill-with-docs questions on [#551](https://github.com/avatarsd-llc/libtracer/issues/551)). **Supersedes the "route it through `mr_`" reading of [ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) errata 4–5**, which named the hole but assumed the existing `std::pmr` seam could close it. Refines [ADR-0060](0060-lkv-copy-store-injected-value-backend.md) §1 (which chose `mem_backend_t` over `std::pmr` for *byte buffers*, on adjacent grounds) and upholds [ADR-0016](0016-substrate-zero-copy-layer-namespaces-no-templates-through-seam.md) §2 (`tr::mem` is where L0 allocation seams live), [ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) §2/§4 (one slab, exhaustion-is-backpressure), and [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md) (bounds are injected resources, never magic constants).

## Context

[ADR-0039](0039-pmr-memory-model-host-aligned-allocation.md) carved allocation into *init/setup* (allowed to be infallible — a source bug, not a runtime condition) and *runtime* (must soft-fail). [RFC-0014](../spec/rfcs/0014-creator-endpoint-connection-lifecycle-and-link-liveness.md) then moved **vertex registration** across that line: a peer's `CREATE` frame reaches `register_vertex_key` on a transport thread, so registration became a runtime, wire-driven operation. ADR-0039's errata 4 and 5 recorded the consequence — the carve-out no longer covers registration, and `std::pmr` cannot report failure by value on `-fno-exceptions`.

Erratum 5 stated the problem. It did not resolve it, and the obvious resolution — tighten the contract on the existing `graph_t::mr_` to "returns `nullptr` on exhaustion, never throws" — is what this ADR rejects, on measurement.

The scope is wider than registration. Sweeping `core/` for every unguarded allocation reachable from a peer's bytes found a second family on the **receive** path ([#588](https://github.com/avatarsd-llc/libtracer/issues/588)): `wire::decode_into`, the terminus arena decoder, made three unguarded `std::pmr` allocations — the node array, the walk's open-node stack, and `walk_stack_t::grow`'s spill past its inline slots. Unlike `CREATE`, that path sits behind **no ACL**, and a peer chooses both the nesting depth and the node count. A third instance sat in `graph.cpp`'s branch-write decode, already carrying a comment conceding it drew from a `monotonic_buffer_resource`'s **throwing** default upstream.

On ESP-IDF, `__cxa_throw` and `__cxa_allocate_exception` are link-wrapped to `abort()` stubs. Every one of these was a peer-reachable reboot.

## Decision

**A second L0 allocation seam, `tr::mem::block_source_t`, vends raw single-owner blocks and reports exhaustion by value (`try_alloc` returns `nullptr`). Every FAILABLE allocation migrates onto it.** `graph_t` and `fwd_router_t` each gained an **appended, defaulted** constructor parameter for it.

Five sub-decisions, each with its rejected alternative.

### 1. Not `std::pmr::memory_resource` with a documented may-return-null contract

This is the option the issue proposed and the one this ADR exists to reject. `memory_resource::allocate` is annotated `__attribute__((__returns_nonnull__))` in libstdc++ — including in the deployment toolchain's own header — so a caller's `if (p == nullptr)` is undefined behaviour and the optimizer may delete it.

It does. Measured on `riscv32-esp-elf-g++ 15.2.0` with the deployment flags (`-march=rv32imac_zicsr_zifencei -mabi=ilp32 -fno-exceptions -fno-rtti -std=c++23`), counting references to the soft-fail leg inside each function body:

| level | reuse `pmr` | own type | derived from `memory_resource` |
| --- | --- | --- | --- |
| `-O0` `-O1` `-O2` `-O3` | 1 | 1 | 1 |
| **`-Os` `-Oz`** | **0** | 1 | 1 |

At `-Os` the reuse form emits no branch at all: the call, then `fill` on the null pointer, then `memcpy`, then `return OK` — a wild write followed by SUCCESS returned to the peer.

`-Os` is what an ESP-IDF node ships (`CONFIG_COMPILER_OPTIMIZATION_SIZE`). It is also the one optimization level **no test executes at** — every libtracer CI job that runs a test builds `-O0`/`-O1`/`-O2`/`-O3`, and the only job compiling at `-Os` is a footprint sentinel that runs nothing. So reusing `pmr` would have traded a diagnosed reboot for an undiagnosed null-deref plus a false SUCCESS on the wire, invisible to the entire suite. That asymmetry is recorded as a consequence below, because it outlives this decision.

### 2. Not derived from `memory_resource` either

The strongest alternative was a libtracer-owned `nothrow_resource_t : public std::pmr::memory_resource` with a public `try_alloc` and a private `final do_allocate` — keeping one type for both contracts during the migration. The `-Os` finding does **not** defeat it: its soft-fail branch survives at every level, and its codegen is instruction-identical to the own-type form.

It is defeated by the slip it leaves behind. On such a seam, `allocate` instead of `try_alloc` — one token, same object, same arguments — compiles with **zero `-Wall -Wextra` diagnostics** and reproduces the dead `-Os` codegen exactly. On `block_source_t` it is `error: … has no member named 'allocate'`. Inheriting keeps a `returns_nonnull`-attributed, publicly callable `allocate()` on the failable seam permanently on the failable seam, one keystroke from every correct call site, and its safety then rests on precisely the class of rule this repository cannot enforce — no `-Werror` on the library, no clang-tidy in CI.

### 3. Not `mem_backend_t`

[ADR-0060](0060-lkv-copy-store-injected-value-backend.md) §1 already chose `mem_backend_t` over `std::pmr` for durable *byte buffers*, so the natural question is why this is not a third use of it. Because `mem_backend_t` vends a **refcounted `view::segment_t`**, and the objects here have exactly one owner and no header: a `segment_t` measures 20 B on rv32 / 40 B on x86-64 against an 80 B `vertex_t`, so a refcount on them is pure overhead on the axis (RAM) that motivated the seam. The two L0 seams are complementary: `mem_backend_t` for shared payload bytes, `block_source_t` for single-owner failable blocks.

### 4. Latency, RAM and throughput do not discriminate — the decision is made below them

Stated explicitly because the priority order (latency > RAM > throughput > minimalism > unification) invites the assumption that it did. The call site is **33 instructions** through `block_source_t` and **33 instructions** through a `memory_resource`-derived class; the two bodies differ by one line, the vtable-slot immediate. The seam pointer is 4 B rv32 / 8 B x86-64 either way; the implementation object is one vptr either way. The vtable-size delta is `.rodata`.

So the ruling rests on the `-Os` precondition (§1), the slip probe (§2), and minimalism — two virtuals against five, three of which serve containers the migration deletes. Unification is the one axis the rejected option wins, and it wins only *during* the migration: at the destination there are no `std::pmr` containers left to unify with.

### 5. Parameters are APPENDED, never prepended

`graph_t(mr, value_backend, ctl)` and `fwd_router_t(graph, mr, rx)`. This is not a style point — it is what makes the host diff **zero lines**. The shipping firmware's `graph_t graph{&mr};` compiles unchanged and picks up the default `heap_source()`, so a peer-reachable abort stops being reachable the moment the library lands, with no coordinated release. A prepended parameter would have made every migration slice a host-visible change and gated each one on a firmware PR.

### 6. Runtime virtual dispatch, and why the ADR-0047 appropriateness rule permits it here

[ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §1 requires every new seam to say which side of the appropriateness rule it falls on, and §2 ruled **compile-time** dispatch appropriate for the L0 *backend* set — so a second L0 seam using runtime virtuals needs its reason on the record.

The rule turns on two conditions together: identity per-target-fixed **and** the path hot or size-critical. The backend set meets both. This seam meets the first and not the second in the way that matters. Its `try_alloc` is called **once or twice per decoded frame** — to size the node array and the open-node stack — not once per node, and never on the forward hop, which is offset-dispatch and allocates nothing (ADR-0038). The measurement is on the record: the migrated terminus decode came out at 236 ns against `main`'s 241–251 ns, i.e. one indirect call per allocation is below the noise of the operation it serves.

Against that, a compile-time seam would have to be closed at build time across **five** shapes that hosts genuinely mix at runtime — heap, null, bump-over-a-buffer, a host's own bounded source, and (for `bump_source_t`) an upstream chosen independently of the buffer. `bump_source_t` *composes* another `block_source_t` as its upstream, which a tag-dispatched closed set expresses badly. Runtime dispatch is also what lets the parameter be **appended and defaulted** (§5) — the property that makes the host diff zero.

If a target later proves the indirect call matters, the escape is the ordinary one: that target closes its own set. Nothing here forecloses it.

### Companions

Three types ship with the seam, because every migrated call site needs the same handful:

- **`bump_source_t`** — a caller buffer handed out by bump, **with an upstream**. The upstream is what makes it a capability-preserving substitution for `monotonic_buffer_resource`: that type also spills past its buffer, but it spills to a *throwing* resource. Pass `null_source()` as the upstream to make the buffer a hard bound instead.
- **`null_source()`** — serves nothing. The honest replacement for `std::pmr::null_memory_resource()`, which signals the same thing by throwing.
- **`block_array_t<T>`** — a nothrow growable array of trivially-copyable `T`; growth returns `false`, relocation is a `memcpy`.

`block_array_t` exposes **`push_slot()`** — claim one uninitialized slot, fill it in place — alongside `push_back`, and hot paths must use it. See Consequences.

## Consequences

**The abort is gone from the paths migrated so far, and it is measurable.** Same 24-deep frame, same 64 B budget, host build: `exit 134` (SIGABRT, `std::bad_alloc`) before; `TLV_NESTING_TOO_DEEP` and `exit 0` after. No new status was invented — [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md) already defines that status as "exceeds this receiver's decode resources", and `walk_stack_t::push` already returned `false` for the no-spill case. Only the allocation was dishonest.

**The `#477` residual is closed as a side effect.** The branch-write decode's comment claimed converting its throwing-upstream overflow leg "needs a node-counting pre-pass". It does not; a `bump_source_t` with a nothrow upstream sufficed. That leg draws from the graph's injected `ctl_`, making it the seam's first consumer.

**No test executes at the shipping optimization level.** §1 turned on that fact, and it is not specific to `pmr`: any construct whose correctness depends on the optimizer's assumptions can differ between what CI runs and what deploys. Recorded here as a standing gap rather than fixed by this ADR.

**A hot path that builds elements as temporaries pays a store-forwarding stall.** The first working migration of `decode_into` was **45 % slower**, while executing *fewer* instructions (IPC 5.03 → 2.55). The cause was `push_back(arena_tlv_t{…})`: a 48-byte aggregate materialized on the stack field-by-field, then read back as wide loads. `std::pmr::vector`'s rvalue `push_back` constructs in place and never pays it; `block_array_t::push_slot()` restores that. Five other hypotheses — modulo-versus-mask alignment, growth inlining, index-versus-pointer cursor, latch stores, callee inlining — were each measured and refuted before a profile pointed here. The migrated decode now measures **236 ns against `main`'s 241–251 ns**, so the seam is not a latency cost.

**Migration is ordered, and incomplete.** Landed: the terminus arena (node array, sink stack, walk spill) and the branch-write decode. Outstanding: **vertex registration** — the seven allocations that opened [#551](https://github.com/avatarsd-llc/libtracer/issues/551) — plus the `route_handle` label tables, the `fwd_router` iov, and the `can_reassembly` maps. Registration is the hard one and is not merely mechanical: its allocations are `unique_ptr`-owned, a stateful deleter costs 8 B on rv32, and `vertex_t` has exactly **one** 4-byte hole (`+mr_` alone already puts x86-64 at 120 = `kMax64`, and both members put rv32 at 88 > `kMax32` = 80). It needs an ownership scheme that spends no per-vertex bytes, and the size gate will judge it.

**`std::pmr` does not leave.** `graph_t::mr_` still serves the LKV control block and the `route_handle` tables; `heap_resource_t` and its `abort()` in the reference firmware are still required by `fwd_router_t`'s remaining pmr containers. Retiring `mr_` is the end state, not this step — and keeping the two as **different C++ types** is what makes that retirement a compile error rather than a silent rebind.

## Considered options

1. **Tighten `graph_t::mr_`'s contract to nullptr-on-exhaustion.** Rejected: §1. It is not implementable on the profile that ships.
2. **A libtracer-owned nothrow class deriving from `memory_resource`.** Rejected: §2. Correct codegen, permanent one-token footgun.
3. **Reuse `mem_backend_t`.** Rejected: §3. A refcount on a single-owner block, on the axis the seam exists to protect.
4. **Boundedness only — accept the abort and fix it separately.** Rejected: the abort *is* the bug; a peer can reboot the node.
5. **`tr::mem::block_source_t` (chosen).**
