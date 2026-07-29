# The LKV slot is a compile-time policy, and the host slot reclaims with hazard pointers

Status: **accepted; implementation pending.** This is the follow-on ADR that [ADR-0064](0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md) §2 required before any lock-free slot could land: it makes the reclamation choice §2 explicitly declined to make, on the measurements of [#635](https://github.com/avatarsd-llc/libtracer/issues/635) (`bench/bench_lkv_slot.cpp`). The binding mechanism is [ADR-0068](0068-build-configuration-is-plain-cpp-config-header.md) §2 (`basic_graph_t<Slot>` + a `config.hpp` alias), decided there; this ADR owes only the reclamation argument and the per-target split. Closes the decision half of [#604](https://github.com/avatarsd-llc/libtracer/issues/604).

## Context

ADR-0064 §2 established that today's slot — `std::atomic<std::shared_ptr<const rope_t>>` — is lock-free by contract and spin-locked in libstdc++, listed three reclamation candidates, and stopped: *"None of these is obviously right… A follow-on ADR should make that choice."* At the time the honest estimate was ~15 ns on a ~67 ns path, single-threaded.

`bench_lkv_slot` (2026-07-29) changed the shape of the question three ways:

1. **The read side inverts.** T readers against one shared slot: 29.3 M/s at T=1 falls to **1.3 M/s at T=24** — 24 readers are collectively 22× slower than one. Both real reclamation schemes scale near-linearly instead (epoch 2,730 M/s, hazard 2,205 M/s at T=24). This is paid by every `read_stored()` caller — `graph_t::read`, `await`'s return, replay, and `read_subtree_folded` at one load **per node**.
2. **Latency does not separate the candidates.** Epoch is 24% ahead at T=24 against a 1.2–1.3× run-to-run spread; at T=1 they tie (117 vs 116 M/s). On the project's own priority order the first criterion is a wash.
3. **RAM separates them decisively.** Peak displaced-but-unreclaimed objects, 1 writer + 23 readers, identical 104-byte payloads and identical retire batching:

   | scheme | peak parked | bytes parked | registry (24 threads) |
   | --- | ---: | ---: | ---: |
   | epoch | **2,029** | **211 KB** | 1,536 B |
   | hazard | 64 | 6.6 KB | 1,536 B |

   The number is not an implementation accident. Epoch-based reclamation frees only what every announced reader has moved past, so its parked memory is bounded by **reader quiescence** — and a libtracer subscriber loop never quiesces. A stalled or steadily-reading thread holds the epoch floor down while a fast writer retires behind it without bound. Hazard pointers are bounded **by construction**: at most one pinned object per reader slot, ever, regardless of scheduling.

Two further facts bound the design, both from the same session (recorded in ADR-0068's survey):

- **The two targets want different slots, not one compromise slot.** At T=1, today's `atomic<shared_ptr>` writes at ~32 ns and reads at ~34 ns; the hazard slot reads at **8.6 ns** but writes at **~43 ns** (the writer pays the retire-list bookkeeping and amortized hazard scan). A write-dominated single-core sensor node is better served by today's slot — which also costs **zero** registry RAM and has no reclamation machinery to size. A many-core host, whose collapse is the read side, wants the hazard slot. This is precisely the fixed-identity ∧ hot-path conjunction of [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §1.
- **Reclamation can never be compiled out, only rebound.** Single-core is not single-threaded: the ESP32 target compiles `posix_endpoint.cpp` and `loopback.cpp`, both of which spawn tasks. Whatever slot a target binds must be MT-correct on that target.

## Decision

### §1 — Two slot policies, selected per target through ADR-0068's mechanism

The slot becomes a policy parameter: `basic_graph_t<Slot>` (and the vertex storage it owns), with the public spelling unchanged — `using graph_t = basic_graph_t<lkv_slot_t>;` where `lkv_slot_t` is an alias bound in `libtracer/config.hpp`:

- **`sp_atomic_slot_t`** — today's `std::atomic<std::shared_ptr<const rope_t>>`, verbatim. Reclamation is the refcount; registry RAM is zero. **The checked-in default**, so a raw `-I` consumer (the Cortex-M0 footprint gate) and the stock ESP-IDF component build byte-identical to today.
- **`hazard_slot_t`** — a plain `std::atomic<const rope_t*>` published with `acq_rel`, read via the classic publish-and-revalidate hazard protocol, with per-writer retire lists and a batched scan (batch amortizes the O(threads) scan; the bench used 64 and that shape is what was measured). **Bound by the host CMake preset.**

Explicit instantiation keeps `graph.cpp` a translation unit (measured: ~3 s once per build for the second instantiation). The host test build instantiates **both** policies so one CI leg covers both; an MCU build instantiates exactly one, so the other's code never reaches flash.

### §2 — The reclamation scheme for the lock-free slot is hazard pointers

Chosen over epoch-based reclamation on the priority order this project fixes (latency > RAM > throughput): latency ties within noise, and RAM is not close — 6.6 KB vs 211 KB peak parked in the measured storm, with epoch's figure **unbounded in principle** under a reader that never quiesces, which is the normal shape of a libtracer consumer. An unbounded-by-scheduling memory term is not admissible on a ~16 KB-heap-floor target class, and on the host it converts a latency win nobody measured into a RAM liability somebody will.

The deferred-park-list candidate (ADR-0064's third option) stays rejected for the reason ADR-0064 already gave: it never reclaims before teardown, which is unacceptable for a value replaced on every write.

### §3 — The hazard registry is per-target configuration, not a synthetic constant

A hazard scheme needs a fixed set of reader slots. Per [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md)'s rule (bounds come from injected resources or per-target config, never a magic number), the slot count is a `config.hpp` constant — `tr::graph::kHazardReaderSlots` — alongside `kVertexLockStripes`, rendered from the same template with the same drift gate. Sizing guidance ships with the knob: one slot per thread that may concurrently call `read_stored()`; a 64-byte cache-line-isolated slot each (the bench's false-sharing control is part of the design, not a bench artifact). Exhaustion policy: a reader that cannot claim a slot falls back to the refcounted read path — correctness never depends on the bound being right, only the read-side scaling does.

### §4 — Scope: the slot, not the stripe

This ADR deliberately does not touch [#635](https://github.com/avatarsd-llc/libtracer/issues/635) (the `snapshot_edges` stripe lock, ×16.6 at T=24 on the distinct-vertex shape). One of #635's candidates — a published immutable edge array — faces the *same* displaced-object problem this ADR solves; the hazard machinery landed here is expected to be reusable there, and that is a reason to land this first, not a license to widen this change.

## Considered options

- **One slot for both targets (hazard everywhere).** Rejected: at T=1 the hazard slot's write is ~34% slower than today's (43 vs 32 ns) and its registry is pure overhead on a target whose workload is write-dominated fan-out. The MCU would pay real latency and RAM for read-side scaling it cannot exhibit.
- **One slot for both targets (status quo everywhere).** Rejected on the inversion measurement: leaving the host at 1.3 M/s aggregate reads at T=24 abandons the project's stated design center (latency-first, many-core host a first-class target) on the exact path a Zenoh-class competitor is judged on.
- **Epoch-based reclamation.** Rejected in §2: latency tie, RAM decided, unbounded parked memory under a non-quiescent reader — the common case, not the corner.
- **Deferred reclamation on the retired-seams park list.** Stays rejected (ADR-0064): never reclaims before teardown; wrong lifetime class for a per-write displacement.
- **A runtime-selected slot (virtual or flag-dispatched).** Rejected: the identity is fixed per target at build time and the seam is per-read/per-write hot — ADR-0047 §1's conjunction holds, and a branch or indirection on every `read_stored()` is the cost §2 exists to remove.
- **Sizing the registry by a hard-coded thread ceiling** (the bench's `kMaxThreads = 64`). Rejected for production code by RFC-0006; the bench needed a fixed array to keep allocation out of the timed window, the library needs a per-target constant with a stated fallback.

## Consequences

- `graph_t` stays the public spelling; `basic_graph_t<Slot>` is the implementation type. The 67 files that spell `graph_t` do not change. (ADR-0068 §2 records the idiom and its measured compile cost.)
- `config.hpp` gains two bindings: `using lkv_slot_t = …;` and `inline constexpr std::size_t kHazardReaderSlots = …;` — both rendered through the existing template + drift gate, both settable per target (CMake cache / Kconfig).
- The `LIBTRACER_NO_ATOMIC` refcount macro is rebound through the same alias mechanism in the same implementation change (ADR-0068 already schedules this to "ride the slot ADR"), retiring `substrate_test_no_atomic`'s hand-listed source recompile.
- **The implementation must land with its own measurement**, not this ADR's model numbers: `bench_lkv_slot` measured model slots on a model payload; the accepted-gate for the real change is the same bench shapes driven through `graph_t::read`/`write` on both instantiations, plus the existing perf gate.
- The "lock-free LKV" comments in `vertex.hpp` become true for the hazard instantiation and stay qualified for `sp_atomic_slot_t` — the correction ADR-0064 required stays in place for the default slot.
- A stalled reader pins at most one displaced rope per claimed slot (hazard's bound); a reader that never releases its hazard slot pins one object forever — the same class of liability as a subscriber that never returns, and diagnosable the same way.
