# The LKV slot is a compile-time policy, and the host slot reclaims with hazard pointers

Status: **accepted; implemented** (slices #644, #645, #647 — see §6 for what the implementation measured and the three things §1–§5 got wrong). This is the follow-on ADR that [ADR-0064](0064-lkv-publish-is-waiterless-and-the-slot-becomes-lock-free.md) §2 required before any lock-free slot could land: it makes the reclamation choice §2 explicitly declined to make, on the measurements of [#635](https://github.com/avatarsd-llc/libtracer/issues/635) (`bench/bench_lkv_slot.cpp`). The binding mechanism is [ADR-0068](0068-build-configuration-is-plain-cpp-config-header.md) §2 (`basic_graph_t<slot_t>` + a `config.hpp` alias), decided there; this ADR owes only the reclamation argument and the per-target split. Closes the decision half of [#604](https://github.com/avatarsd-llc/libtracer/issues/604).

**Erratum, 2026-07-29 (#642).** As first written, §1 quoted the host slot's read at **8.6 ns** and sized the host win from it. That figure came from a bench arm whose reader does not keep the object, which is not a contract `read_stored()` can offer; §1's numbers and the paragraph below on the owning read are the correction. The **decisions** in §1–§4 are unchanged — the measurement moved the magnitude, not the ruling, and it strengthened the per-target split. The template parameter is also spelled `slot_t` throughout, per [STYLE.md](../../core/STYLE.md)'s "types and aliases are `snake_case` + `_t`, not PascalCase" (precedent: `can_tx_pool.hpp`'s `template <typename slot_t>`).

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

- **An owning read cannot have the cheap read.** `read_stored()` returns a handle the caller keeps, and `read_subtree_folded` stashes one LKV per node into a `std::vector<snap_node_t>` that outlives the map lock and spans three passes — so **N ropes are pinned simultaneously**, which one hazard slot per thread cannot express. A hazard read must therefore promote its pin to a counted reference before dropping it, and that promotion is a read-modify-write on the one cache line every reader shares — the same term that makes today's slot collapse. Measured (#642, arm `hazard-ref`): the owning read runs at **39.5 M/s at T=1 and 27.2 M/s at T=24** against the non-owning arm's 116 and 2,357. It is still **20.8×** today's slot at T=24, and it no longer inverts (1.45× degradation across a 24× thread increase, against today's 25×) — but the host win is 20×, not the three orders of magnitude the non-owning arm implied.
- **The two targets want different slots, not one compromise slot.** At T=1 today's `atomic<shared_ptr>` reads at ~30 ns and writes at ~34 ns; the owning hazard slot reads at ~25 ns (**1.19×**, so single-threaded reads gain almost nothing) and writes at ~53 ns — **0.64×**, a measurably *worse* write. A write-dominated single-core sensor node is therefore better served by today's slot, which also costs **zero** registry RAM and has no reclamation machinery to size. A many-core host, whose collapse is the read side, wants the hazard slot. This is precisely the fixed-identity ∧ hot-path conjunction of [ADR-0047](0047-build-time-closed-module-sets-compile-time-seams.md) §1 — and the corrected numbers make the split *more* clearly right than the original ones did, since they show the MCU paying a real write penalty for a read win it cannot exhibit.
- **Reclamation can never be compiled out, only rebound.** Single-core is not single-threaded: the ESP32 target compiles `posix_endpoint.cpp` and `loopback.cpp`, both of which spawn tasks. Whatever slot a target binds must be MT-correct on that target.

## Decision

### §1 — Two slot policies, selected per target through ADR-0068's mechanism

The slot becomes a policy parameter: `basic_graph_t<slot_t>` (and the vertex storage it owns), with the public spelling unchanged — `using graph_t = basic_graph_t<lkv_slot_t>;` where `lkv_slot_t` is an alias bound in `libtracer/config.hpp`:

- **`sp_atomic_slot_t`** — today's `std::atomic<std::shared_ptr<const rope_t>>`, verbatim. Reclamation is the refcount; registry RAM is zero. **The checked-in default**, so a raw `-I` consumer (the Cortex-M0 footprint gate) and the stock ESP-IDF component build byte-identical to today.
- **`hazard_slot_t`** — a plain `std::atomic<const rope_t*>` published with `acq_rel`, read via the classic publish-and-revalidate hazard protocol, with per-writer retire lists and a batched scan (batch amortizes the O(threads) scan; the bench used 64 and that shape is what was measured). Because the read hands back an owning handle, the pin is **promoted** to a counted reference before it is released — the rope carries the count, and the hazard pin exists to make that promotion safe against a concurrent free, which is exactly the window libstdc++'s `atomic<shared_ptr>` guards with a lock bit instead. Retirement is latched, so a pin that resurrects an already-retired rope re-uses its retire-list entry rather than adding a second one. **Bound by the host CMake preset.**

Explicit instantiation keeps `graph.cpp` a translation unit (measured: ~3 s once per build for the second instantiation). The host test build instantiates **both** policies so one CI leg covers both; an MCU build instantiates exactly one, so the other's code never reaches flash.

### §2 — The reclamation scheme for the lock-free slot is hazard pointers

Chosen over epoch-based reclamation on the priority order this project fixes (latency > RAM > throughput): latency ties within noise — 2,357 vs 2,295 M/s at T=24 on a 1.2–1.3× run spread, and 116 vs 117 at T=1 — so the first criterion does not separate them, and the second does: 6.6 KB vs 211 KB peak parked in the measured storm, a 32× difference on identical payloads and identical retire batching.

Two corrections to how this ADR first argued that, neither of which changes the ruling:

- The original text called epoch's parking *"unbounded in principle under a reader that never quiesces."* That is too strong. A reader announces its epoch per read operation and clears it after, so it **does** quiesce between reads; epoch's parked set is bounded by (write rate × longest single read region), not by the consumer's lifetime. The bound is real — it is simply much larger, and much harder to state in a header, than hazard's "one pinned rope per claimed slot, ever."
- The original text justified rejecting epoch partly because *"an unbounded-by-scheduling memory term is not admissible on a ~16 KB-heap-floor target class."* That does not apply: per §1 the MCU binds `sp_atomic_slot_t` and runs **neither** scheme, so the reclamation choice is a host-only decision and 211 KB of transient parking is immaterial there.

What survives is the argument that actually decides it: on a host, latency is a tie, so the tiebreak is free, and it should go to the scheme whose worst case can be stated as a constant times the reader count rather than as a product involving the write rate. That is hazard. Nothing was traded away to get it.

The deferred-park-list candidate (ADR-0064's third option) stays rejected for the reason ADR-0064 already gave: it never reclaims before teardown, which is unacceptable for a value replaced on every write.

### §3 — The hazard registry is per-target configuration, not a synthetic constant

A hazard scheme needs a fixed set of reader slots. Per [RFC-0006](../spec/rfcs/0006-resource-bounded-nesting-depth.md)'s rule (bounds come from injected resources or per-target config, never a magic number), the slot count is a `config.hpp` constant — `tr::graph::kHazardReaderSlots` — alongside `kVertexLockStripes`, rendered from the same template with the same drift gate. Sizing guidance ships with the knob: one slot per thread that may concurrently call `read_stored()`; a 64-byte cache-line-isolated slot each (the bench's false-sharing control is part of the design, not a bench artifact). Exhaustion policy: a reader that cannot claim a slot falls back to the refcounted read path — correctness never depends on the bound being right, only the read-side scaling does. (**Superseded by §6**: §5's indirection node left no unpinned read to fall back *to*, so what ships is a shared reserved index under a spin lock. Same guarantee, different mechanism.)

### §4 — Scope: the slot, not the stripe

This ADR deliberately does not touch [#635](https://github.com/avatarsd-llc/libtracer/issues/635) (the `snapshot_edges` stripe lock, ×16.6 at T=24 on the distinct-vertex shape). One of #635's candidates — a published immutable edge array — faces the *same* displaced-object problem this ADR solves; the hazard machinery landed here is expected to be reusable there, and that is a reason to land this first, not a license to widen this change.

### §5 — The hazard slot keeps `std::shared_ptr` by pinning an indirection node

Added 2026-07-30, after slices 1–2 (#644, #645) and before the hazard slice, because it removes the largest unknown in what remains. §1 describes `hazard_slot_t` as "a plain `std::atomic<const rope_t*>`", which raises a question that ADR-0064 and this ADR both left open: `read_stored()` returns `std::shared_ptr<const rope_t>`, so how does a slot holding a bare pointer produce one?

Measured, not reasoned (a five-line program, this libstdc++):

| candidate | `is_lock_free()` | verdict |
| --- | ---: | --- |
| `std::atomic<std::shared_ptr<const rope_t>>` | 0 | today's slot — the lock bit ADR-0064 measured |
| `std::atomic<std::weak_ptr<const rope_t>>` | **0** | the obvious alternative buys **nothing**: same `_Sp_locker` |
| `std::atomic<T*>` | 1 | the only lock-free option |

And from a bare `const rope_t*` there is no standard route back to its control block — the rope does not inherit `enable_shared_from_this`, and adding that would put a control-block pointer in every rope.

So the slot does not store a pointer to the *rope*. It stores a pointer to a small **indirection node** that owns the rope's `shared_ptr`:

- **publish** — allocate a node holding the new `shared_ptr` (16 bytes, verified), `exchange` it in, retire the displaced node.
- **read** — pin the node, **copy its `shared_ptr` out** (one control-block increment), unpin, return the copy.
- **reclaim** — the hazard scan frees *nodes*; the rope's own lifetime remains the refcount's business, exactly as today.

Two consequences worth stating because they change the size of the remaining work:

- **No public API change.** `read_stored()` keeps its signature, so the four call sites in `graph.cpp`, `snap_node_t::lkv`, and the N-simultaneous-handles property of `read_subtree_folded` all stand untouched. The hazard slice does not have to modify `graph.cpp` at all.
- **The cost is already measured.** The read is precisely the promotion `hazard-ref` modelled — pin, one RMW on a line every reader shares, unpin — so the 20.8× at T=24 in the Context above is the number to expect, not an optimistic stand-in. The write additionally pays one 16-byte allocation per publish, on top of the rope's own. That lands squarely on the MCU's dominant operation, which is a third independent reason §1 keeps `sp_atomic_slot_t` as the default rather than a first.

**Open question for the maintainer, deliberately not resolved here.** Slices 1–2 delivered per-target slot selection with **zero templates** — `config.hpp` forward-declares the policy and aliases it, `vertex_t` names the alias, and `graph.cpp.o` came out byte-for-byte unchanged. That means ADR-0068 §2's `basic_graph_t<slot_t>` is now needed for exactly one thing: getting two policies instantiated in a single binary for CI coverage. A second host build already achieves that, and `substrate_test_no_atomic` is the standing precedent for doing it that way. Whether to spend the template (and the ~3 s/build second instantiation ADR-0068 measured) on that convenience is a call for whoever owns ADR-0068 §2; this ADR notes only that the mechanism it depends on turned out not to require it. **The implementation answered it in practice**: §6 needed neither — the coverage came from naming both types in one test plus one rebound CI leg.

### §6 — What the implementation measured, and the three things §1–§5 got wrong

Added 2026-07-30 with the hazard slice (#647). This ADR's own Consequences demanded it: *"the implementation must land with its own measurement, not this ADR's model numbers."* It did, and the model numbers did not survive.

**The read win is 4.2×, not 20.8×.** `graph_t::read`, T readers on one shared LKV, both slots built from the same tree, medians of six alternating runs with no rebuild between rounds (24-core host, `lkvgraph_hot1-fan0-read`):

| readers | `sp_atomic_slot_t` | `hazard_slot_t` | gain | p50 gain |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 21.1 M/s | 18.7 M/s | 0.88× | 1.12× |
| 2 | 7.9 M/s | 10.9 M/s | 1.39× | 1.24× |
| 4 | 6.1 M/s | 8.9 M/s | 1.47× | 0.88× |
| 8 | 2.2 M/s | 5.2 M/s | 2.38× | 2.36× |
| 16 | 1.9 M/s | 5.7 M/s | 3.06× | 1.49× |
| 24 | **1.7 M/s** | **7.4 M/s** | **4.22×** | **4.41×** |

Run-to-run spread was 1.04–1.56× on this shape, so everything from T=8 up is decisive and T=1 is not a measurement of anything. The gap between 4.2× and the model's 20.8× is not a defect in either number: `hazard-ref` measured a slot, and a slot is one term of `graph_t::read`. Deleting the `_Sp_locker` lock bit exposes whatever is next, and what is next still does not scale — twenty-four readers remain 2.9× slower than one, against 12.1× before. **The inversion is softened, not removed.**

**Where the residual is — second erratum, 2026-07-30, and this one is measured by ablation rather than inferred.** This paragraph has now been wrong twice. As first written it sent the residual to #635, which is wrong: `snapshot_edges` is on the *delivery* path and a read never calls it. The first correction then replaced that with "nothing process-wide is serializing — not the map lock", inferred from a retention table. **That inference was backwards and the table's arithmetic was wrong.** Both are corrected here.

The arithmetic first: the retention figures of 94% / 91% divided `stripe1-read`'s T=24 rate by **`hot1-read`'s** T=1 rate. Against each shape's own T=1 the numbers are 19.7/18.7 = 106% and 17.1/17.8 = 96%.

The inference second, and it matters more. A **flat aggregate across a 24× thread range is the signature of a perfect serializer, not of scaling**: 24 threads producing the same total as one means each thread is 24× slower. Retention of a T=1 aggregate is not evidence that nothing serializes — it is close to proof that something does.

Ablated (`has_registered_child` short-circuited to `return false`, otherwise the same tree, three runs, medians):

| shape | T | stock | no map lock | |
| --- | ---: | ---: | ---: | ---: |
| `stripe1-read` — **distinct** vertices | 8 | 21.3 M/s | 64.3 M/s | **3.0×** |
| `stripe1-read` — **distinct** vertices | 24 | 19.7 M/s | **165.3 M/s** | **8.4×** |
| `spread-read` — **distinct** vertices | 8 | 18.4 M/s | 64.3 M/s | 3.5× |
| `hot1-read` — **one** vertex | 24 | 1.74 M/s | 1.64 M/s | 0.94× |
| `spread-fan0` — write | 8 | 35.4 M/s | 35.4 M/s | 1.00× |

So there are **two** independent limits, and the first one was invisible because the second hid it:

- **Every `graph_t::read` takes `map_mutex_` in shared mode**, before it reaches the slot — `graph_t::read` called `graph_t::has_registered_child`, which takes it (`graph.cpp:727-728`, still there for its other callers) to decide the leaf/branch fork. One process-wide reader-writer lock therefore caps *all* reads at roughly **20 M/s per process regardless of core count or topology**, which is why the healthy distinct-vertex shape looked flat. `write_impl` takes no map lock, which is the whole of the "writes scale 5×, reads do not" asymmetry. This is a larger finding than anything else in §6, and it is now **fixed** ([#652](https://github.com/avatarsd-llc/libtracer/issues/652)): the fork is a bit on the vertex (`graph.cpp:831`, `v->has_registered_child()`), and twenty-four readers on distinct vertices went **19.7 → 163.5 M ops/s (8.28×)** — 99% of the ablation ceiling below. Nothing about the slot changed and nothing about the shared-vertex shape moved, which is exactly what the two-independent-limits reading predicts.
- **On one shared vertex the map lock is not the binding constraint** — removing it changes nothing (0.94×). There the limit is the vertex itself, and on a read shape the only line every reader *modifies* is the rope's control block: the promotion §5 called irreducible for an owning read. Hazard reclamation moved that shape from 1.7 to 7.4 M/s and left the promotion untouched, so the scoped read (#649) remains the next lever **for that shape**. It is not the lever for the distinct-vertex ceiling, which no slot policy can touch.

**Which points at the next lever, and it is not a reclamation scheme.** Of the four `read_stored()` call sites, only `read_subtree_folded` (`graph.cpp:2096`, its `read_stored()` at `:2151`) keeps the handle; the other three — the leaf read, `deliver_current`, and `await`'s return — are `const shared_ptr sp = v->read_stored(); … use *sp …` and drop it before returning. A **scoped** read that hands back a guard instead of an owning handle would skip the promotion entirely on those three, which is the non-owning contract the `hazard` arm measured at 2,357 M/s. That is an API question, not a slot question, and it is where the rest of this read lives.

Every other shape — the distinct-vertex read controls (`stripe1`, `spread`), all three write topologies, both fan-outs — landed inside a 1.4–2.2× run-to-run spread in both directions. In particular **§1's predicted 0.64× write penalty is not observable through `graph_t::write`**, partly dilution and partly because the implementation recycles displaced nodes through a per-participant free list, so a steady-state publish allocates nothing at all. §5's "one 16-byte allocation per publish" is a warm-up cost, not a per-write one.

Three corrections the implementation forced:

- **§3's exhaustion policy is not implementable as written.** It said a reader that cannot claim a slot "falls back to the refcounted read path". Under §5's design there is no such path: the value is reached *through* a node, so a reader holding no pin cannot touch it at all. What ships keeps the guarantee (correctness never depends on `kHazardReaderSlots` being right) by a different mechanism — one extra reserved index that overflow threads share under a spin lock, serializing with each other and with nobody else.
- **Deferred reclamation collides with ADR-0039's injected memory resource.** A displaced node owns the rope's `shared_ptr`, so a value stays *allocated* until a later scan — past graph destruction, and in the first build of this slice past the destruction of the `std::pmr::memory_resource` the rope came from. `graph_pmr_test` failed its balance check and then faulted at exit freeing a rope through a dead resource. The fix is a flush when a slot is destroyed. The residue is a genuinely weaker contract than `sp_atomic_slot_t`'s: an injected resource must outlive the **threads that wrote through it**, not merely the graph. Vacuous for a host on the process heap; a real constraint for a scoped arena, and a fourth reason the MCU keeps the default.
- **The hazard slot makes publish fallible.** `sp_atomic_slot_t` never allocates on publish; this one does when its free list is empty. As first shipped, a failure left the previous value published while `vertex_t::store` still reported success — a dropped write reported as taken, which is exactly what [#477](https://github.com/avatarsd-llc/libtracer/issues/477)'s nothrow soft-fail doctrine exists to prevent. **Corrected the same day**: the policy contract is now `[[nodiscard]] bool store(...)` plus a separate `void clear(order)` that cannot fail, and `vertex_t::store` maps `false` onto the `nullptr` → `BACKPRESSURE` path an LKV allocation failure already takes. `sp_atomic_slot_t` returns `true` unconditionally. The failure is now unreachable-by-omission rather than merely documented, and `lkv_slot_test` drives it for real by replacing the test binary's nothrow `operator new`. Note also that the node is allocated from the **global heap**, not from a graph's injected `std::pmr::memory_resource` — the slot policy is never handed one.

Coverage, since ADR-0068 §2's template turned out not to be needed: `lkv_slot_test` names **both** policies explicitly, so every leg — including ASan/UBSan/LSan and TSan — exercises the hazard slot's reclamation, its concurrency, its parked-set bound, and its exhausted-registry path regardless of what that build bound. On top of that, one `build-test` leg and one leg of each sanitizer job rebind `lkv_slot_t` so the full suite runs with the hazard slot under `graph_t` — which is the only thing that could have caught the pmr break above.

## Considered options

- **One slot for both targets (hazard everywhere).** Rejected: at T=1 the hazard slot's write is ~34% slower than today's (43 vs 32 ns) and its registry is pure overhead on a target whose workload is write-dominated fan-out. The MCU would pay real latency and RAM for read-side scaling it cannot exhibit.
- **One slot for both targets (status quo everywhere).** Rejected on the inversion measurement: leaving the host at 1.3 M/s aggregate reads at T=24 abandons the project's stated design center (latency-first, many-core host a first-class target) on the exact path a Zenoh-class competitor is judged on.
- **Epoch-based reclamation.** Rejected in §2: latency tie, RAM decided, unbounded parked memory under a non-quiescent reader — the common case, not the corner.
- **Deferred reclamation on the retired-seams park list.** Stays rejected (ADR-0064): never reclaims before teardown; wrong lifetime class for a per-write displacement.
- **A runtime-selected slot (virtual or flag-dispatched).** Rejected: the identity is fixed per target at build time and the seam is per-read/per-write hot — ADR-0047 §1's conjunction holds, and a branch or indirection on every `read_stored()` is the cost §2 exists to remove.
- **Sizing the registry by a hard-coded thread ceiling** (the bench's `kMaxThreads = 64`). Rejected for production code by RFC-0006; the bench needed a fixed array to keep allocation out of the timed window, the library needs a per-target constant with a stated fallback.

## Consequences

- `graph_t` stays the public spelling; `basic_graph_t<slot_t>` is the implementation type. The 67 files that spell `graph_t` do not change. (ADR-0068 §2 records the idiom and its measured compile cost.)
- `config.hpp` gains two bindings: `using lkv_slot_t = …;` and `inline constexpr std::size_t kHazardReaderSlots = …;` — both rendered through the existing template + drift gate, both settable per target (CMake cache / Kconfig).
- The `LIBTRACER_NO_ATOMIC` refcount macro is rebound through the same alias mechanism in the same implementation change (ADR-0068 already schedules this to "ride the slot ADR"), retiring `substrate_test_no_atomic`'s hand-listed source recompile.
- **The implementation must land with its own measurement**, not this ADR's model numbers: `bench_lkv_slot` measured model slots on a model payload; the accepted-gate for the real change is the same bench shapes driven through `graph_t::read`/`write` on both instantiations, plus the existing perf gate. That gate now has its baseline — #642 added the `graph_t::read` topology Part B was missing. **The gate ran: see §6.** Today's `lkvgraph_hot1-fan0-read` (T readers, one shared LKV) runs **15.4 M ops/s at T=1 falling to 1.8 at T=24**, p50 90 ns to 6,642 ns. The distinct-vertex controls do not collapse (`stripe1` 1.6×, `spread` flat), which is what locates the cost in the vertex rather than in a shared lock, and is the reason this ADR's premise survived contact with the real path at all.
- **The single-threaded read is not where the win is.** Today's real read is ~90 ns p50 at T=1, of which the slot is roughly a third; the owning hazard read gains 1.19× at T=1. Anyone reading this ADR as a licence to expect a faster read on a one-core box should not. (§6 measured it at T=1 as *indistinguishable* — even 1.19× overstated it.)
- The "lock-free LKV" comments in `vertex.hpp` become true for the hazard instantiation and stay qualified for `sp_atomic_slot_t` — the correction ADR-0064 required stays in place for the default slot.
- A stalled reader pins at most one displaced rope per claimed slot (hazard's bound); a reader that never releases its hazard slot pins one object forever — the same class of liability as a subscriber that never returns, and diagnosable the same way.
