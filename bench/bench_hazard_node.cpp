/**
 * @file
 * @brief Hazard-slot node ACQUISITION, isolated — the gate #873 phase 2 was staged behind,
 *        and the instrument that reverted it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Phase 2 of #873 proposed moving `%tr::graph::detail_hp::node_t` storage off the global heap
 * and onto the hazard domain's injected `%tr::mem::block_source_t`. The maintainer ruling made
 * that move conditional on a **dedicated before/after microbench of hazard-slot acquisition** —
 * not on end-to-end write latency, in which the acquisition is a rounding error and a
 * regression would hide. This file is that instrument.
 *
 * It **failed the gate** (+22.7 % on the allocating publish, +3.5 % on the free-list arm that
 * the substrate never touches, ranges disjoint, against a two-binary A/A null under ±1.2 %),
 * so the migration reverted and the hazard nodes stay on the global heap as a documented
 * carve-out — `docs/reference/09-memory-substrate.md` §"The carve-out" has the whole table.
 * This bench is checked in because that carve-out is a MEASURED decision with a shelf life:
 * anyone reopening the question re-runs this, under the same protocol, before believing a
 * replacement.
 *
 * ## Why a general LKV bench is not sufficient
 *
 * `bench_lkv_slot.cpp` measures the slot under concurrency, where the interesting cost is the
 * announcement store and the refcount promotion. Node acquisition barely appears in it, and
 * deliberately so: `%acquire_node` allocates only when the participant's free list is empty,
 * which after the first publish it never is (ADR-0069 §5's "one allocation per publish" is
 * amortized to zero by the displacement that recycles). A bench that publishes in a loop
 * therefore measures the free-list hit and would report a **flat line whatever the allocator
 * does** — which is a true statement about the steady state and no evidence at all about the
 * arm an allocator change is on.
 *
 * ## The two arms, and why both are reported
 *
 *   - `hazard-acquire` — the COLD arm, and the one the ruling gates on. Every publish
 *     allocates, because each one goes to a slot that has never been written: an empty slot
 *     displaces nothing, so nothing is retired, so the free list never fills. `kLiveSlots`
 *     slots are held live at once for exactly that reason. This is `%acquire_node`'s
 *     allocating path under a stopwatch, plus the `%ticket_t` and the `exchange` that every
 *     publish pays regardless.
 *   - `hazard-steady` — the CONTROL. One slot, republished. Every publish after the first
 *     takes the free list, so an allocator change cannot reach this arm and it must read the
 *     same on both sides of one. It is here to catch a change that leaks onto the hot arm by
 *     re-partitioning the compiler's budget rather than by adding an instruction to the path —
 *     which is exactly what phase 2 turned out to do, and what the cold arm alone could not
 *     have said.
 *
 * The teardown that returns the nodes (`%retire_and_flush` → `%scan` → `%recycle`) is timed as
 * its own arm, `hazard-release`, rather than folded into the cold one: the free path and the
 * allocate path move independently and a fused number cannot say which one did.
 *
 * ## Reading it
 *
 * Same rules as every other latency leg (`docs/methodology.md` §"The A/B protocol"): both
 * arms pinned to the same single logical CPU, interleaved rounds, first discarded, medians
 * AND ranges, best-of-rounds — and an A/A null band from the same binary against itself
 * before any A/B number is believed.
 *
 * The RESULT rows use the standard twelve-field shape so `bench/best_of_rounds.py` and
 * `bench/collate.py` read them without a special case. The three axis columns carry the
 * bench's own knobs: `size` = live slots, `fan` = 1, `ep` = 1.
 *
 * @warning **Every figure here is BATCH-DERIVED**, so `p50`, `p99` and `mean` are the same
 *          number and the distribution columns carry no distribution. That is deliberate and
 *          it is the only way this leg can see what it was built to see: a node acquisition
 *          costs a few tens of nanoseconds and two `%now_ns` reads cost ~22 ns of it, so
 *          per-operation timestamping would spend most of the measurement on the stopwatch
 *          and compress exactly the difference the gate is asking about (measured: with
 *          per-op reads the cold and the free-list arms both read a p50 of 30 ns — the clock,
 *          not the code). The window charges the two reads once per batch instead. Read the
 *          `pub/s` column; the latency columns are `1e9 / pub_s` restated.
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/lkv_slot.hpp"
#include "libtracer/rope.hpp"

namespace {

using tr::graph::hazard_slot_t;

/**
 * @brief How many slots are held live at once on the cold arm.
 *
 * Large enough that the run is dominated by allocation rather than by the surrounding
 * bookkeeping, small enough that the whole working set still fits a core's cache and the
 * measurement is of the allocator rather than of DRAM. Every one of these publishes
 * allocates.
 */
constexpr std::size_t kLiveSlots = 4096;

/**
 * @brief Cold-arm batches; each is `kLiveSlots` allocations followed by `kLiveSlots` frees.
 *
 * Sized for a window of roughly half a second per arm on the reference host. Shorter runs
 * were tried and rejected: at 24 batches the whole arm finished in ~6 ms, which is inside the
 * scheduler's own granularity and produced a round-to-round spread wider than the effect.
 */
constexpr std::size_t kBatches = 2000;

/** @brief Publishes on the steady (free-list) arm — same wall-clock target as the cold one. */
constexpr std::size_t kSteadyOps = 20'000'000;

/** @brief One shared payload, so the arms measure the node and not rope construction. */
[[nodiscard]] std::shared_ptr<const tr::view::rope_t> payload() {
    static const std::shared_ptr<const tr::view::rope_t> p = std::make_shared<tr::view::rope_t>();
    return p;
}

/** @brief Emit one arm's batch-derived per-operation figure in the standard RESULT shape. */
void emit_arm(const char* mode, std::size_t size_axis, std::size_t ops, std::uint64_t ns) {
    const double per = ops == 0 ? 0.0 : static_cast<double>(ns) / static_cast<double>(ops);
    const double rate = ns == 0 ? 0.0 : static_cast<double>(ops) * 1e9 / static_cast<double>(ns);
    const auto q = static_cast<std::uint64_t>(per + 0.5);
    const bench::Latency::Summary s{q, q, q, q, q, ops, ops >= bench::kTailSampleFloor};
    bench::emit("libtracer", mode, size_axis, 1, 1, rate, rate, 0.0, s);
    bench::emit_tail("libtracer", mode, size_axis, 1, 1, s);
}

/**
 * @brief The cold arm plus its release companion.
 *
 * Slots are default-constructed into a vector that is reused across batches, and the
 * construction is deliberately OUTSIDE both windows — it is `%std::make_unique` and the
 * vector's own bookkeeping, neither of which this leg is about. Two windows per batch: the
 * publish loop (`%acquire_node`'s allocating arm) and the teardown (`%retire_and_flush` →
 * `%scan` → `%recycle`, which is where a node is freed).
 */
void run_cold() {
    std::vector<std::unique_ptr<hazard_slot_t>> slots;
    slots.reserve(kLiveSlots);

    const auto sp = payload();
    std::uint64_t acquire_ns = 0;
    std::uint64_t release_ns = 0;

    for (std::size_t b = 0; b < kBatches; ++b) {
        for (std::size_t i = 0; i < kLiveSlots; ++i) {
            slots.push_back(std::make_unique<hazard_slot_t>());
        }

        const std::uint64_t t0 = bench::now_ns();
        for (std::size_t i = 0; i < kLiveSlots; ++i) {
            if (!slots[i]->store(sp)) {
                std::fprintf(stderr, "hazard store refused at %zu — out of memory?\n", i);
                std::exit(1);
            }
        }
        const std::uint64_t t1 = bench::now_ns();
        slots.clear();
        const std::uint64_t t2 = bench::now_ns();
        acquire_ns += t1 - t0;
        release_ns += t2 - t1;
    }

    const std::size_t ops = kBatches * kLiveSlots;
    emit_arm("hazard-acquire", kLiveSlots, ops, acquire_ns);
    emit_arm("hazard-release", kLiveSlots, ops, release_ns);
}

/** @brief The control arm: one slot republished, so every acquisition is a free-list hit. */
void run_steady() {
    hazard_slot_t slot;
    const auto sp = payload();

    if (!slot.store(sp)) std::exit(1);  // the one allocating publish, outside the window

    const std::uint64_t t0 = bench::now_ns();
    for (std::size_t i = 0; i < kSteadyOps; ++i) {
        if (!slot.store(sp)) std::exit(1);
    }
    const std::uint64_t total = bench::now_ns() - t0;

    emit_arm("hazard-steady", 1, kSteadyOps, total);
}

}  // namespace

/**
 * @brief `bench_hazard_node [cold|steady]` — both arms by default.
 *
 * An unrecognised argument REFUSES to run rather than falling through to the default, for
 * #1040's reason: an A/B builds one arm from another commit, and a mode that silently ran a
 * different workload than its partner would emit well-formed rows a harness would join.
 */
int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode.empty()) {
        run_cold();
        run_steady();
        return 0;
    }
    if (mode == "cold") {
        run_cold();
        return 0;
    }
    if (mode == "steady") {
        run_steady();
        return 0;
    }
    std::fprintf(stderr, "bench_hazard_node: unknown mode '%s' (cold|steady)\n", mode.c_str());
    return 2;
}
