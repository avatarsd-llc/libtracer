/**
 * @file
 * @brief The QSBR domain's 31-bit epoch: a wrap or a long-open bracket delays a free, never
 *        allows one (`qsbr.hpp`, section "The 31-bit epoch").
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The counter is set directly, so the test reaches the wrap and the stall lag without 2^29
 * real retirements. One thread, so the only open bracket is this test's own.
 */
#include <cstdint>
#include <cstdio>

#include "libtracer/qsbr.hpp"
#include "test_support.hpp"

namespace {

namespace q = tr::graph::detail_qsbr;
using tr::testing::check;

void set_epoch(std::uint32_t e) { q::registry().ctl.epoch.store(e, std::memory_order_seq_cst); }

/** @brief Across the wrap, a pair retired before it waits for the bracket and then frees. */
void across_the_wrap() {
    std::printf("a bracket open across the 2^31 wrap holds the pair retired inside it\n");
    for (const std::uint32_t start : {q::kEpochMask - 1, q::kEpochMask, 0xFFFFFFFFu}) {
        set_epoch(start);
        q::enter();
        const std::uint32_t e = q::advance();
        (void)q::advance();
        (void)q::advance();  // the counter is now past the wrap
        check(!q::all_quiescent_past(e), "inside the bracket, the pair is not free");
        check(q::leave(), "the bracket closes");
        check(q::all_quiescent_past(e), "once it closed, the pair is free");
        q::enter();
        check(q::all_quiescent_past(e), "a bracket opened after the wrap does not hold it");
        check(q::leave(), "and closes");
    }
}

/** @brief A bracket @ref q::kStallLag behind stalls the counter; frees wait for it. */
void stall_behind_an_old_bracket() {
    std::printf("the counter stalls behind a bracket open across 2^29 advances\n");
    set_epoch(100);
    q::enter();  // online at 100
    set_epoch(100 + q::kStallLag);
    const std::uint32_t before = q::registry().ctl.epoch.load();
    const std::uint32_t e = q::advance();
    check(e == before && q::registry().ctl.epoch.load() == before,
          "advance answers the current epoch and does not bump it");
    check(!q::all_quiescent_past(e), "a pair parked at the stalled epoch waits for the bracket");
    set_epoch(100 + q::kEpochWindow + 1);  // as if the counter had run on regardless
    check(!q::all_quiescent_past(100 + q::kEpochWindow),
          "even 2^30 epochs on, the old bracket never reads as past the pair");
    set_epoch(100 + q::kStallLag);
    check(q::leave(), "the old bracket closes");
    check(q::all_quiescent_past(e), "and the pair parked behind it is free");
    const std::uint32_t after = q::advance();
    check(after == before && q::registry().ctl.epoch.load() == before + 1,
          "the next advance bumps again");
}

/**
 * @brief The asymmetric fence (`qsbr.hpp` §qsbr_asym): where the process registered for
 *        `membarrier`, a bracket enters with a plain store and still holds what it covers.
 */
void asymmetric_entry() {
    std::printf("a bracket entered with a plain store holds a pair; one fence serves a row\n");
    if constexpr (!q::kMembarrier) {
        std::printf(
            "  (not compiled in on this target: the entry is the store and seq_cst fence)\n");
        return;
    }
    const bool on = q::asym_ready();
    std::printf("  membarrier registration: %s\n",
                on ? "accepted, plain entry in use" : "refused, fenced entry kept");
    set_epoch(500);
    q::enter();
    const std::uint32_t e = q::advance();
    bool fenced = false;
    check(!q::ripe(e, fenced) && !fenced, "inside the bracket: not ripe, and no fence was paid");
    check(!q::all_quiescent_past(e), "nor by the single-epoch scan");
    check(q::leave(), "the bracket closes");
    check(q::ripe(e, fenced) && fenced == true, "once it closed: ripe, after one fence");
    check(q::ripe(e - 1, fenced), "a second epoch of the same row reuses that fence");
    check(q::all_quiescent_past(e), "and the single-epoch scan agrees");
}

}  // namespace

int main() {
    across_the_wrap();
    stall_behind_an_old_bracket();
    asymmetric_entry();
    return tr::testing::summary("qsbr_epoch_wrap");
}
