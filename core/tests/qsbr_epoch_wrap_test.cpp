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
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "libtracer/qsbr.hpp"
#include "test_support.hpp"

namespace {

namespace q = tr::graph::detail_qsbr;
using tr::testing::check;

void set_epoch(std::uint32_t e) { q::registry().ctl.epoch.store(e, std::memory_order_seq_cst); }

/** @brief The question `retire` asks: is a pair retired at @p e free now, after one fence? */
[[nodiscard]] bool free_now(std::uint32_t e) {
    return q::horizon([e](const q::horizon_t& h) { return h.past(e); }).past(e);
}

/** @brief Across the wrap, a pair retired before it waits for the bracket and then frees. */
void across_the_wrap() {
    std::printf("a bracket open across the 2^31 wrap holds the pair retired inside it\n");
    for (const std::uint32_t start : {q::kEpochMask - 1, q::kEpochMask, 0xFFFFFFFFu}) {
        set_epoch(start);
        q::enter();
        const std::uint32_t e = q::advance();
        (void)q::advance();
        (void)q::advance();  // the counter is now past the wrap
        check(!free_now(e), "inside the bracket, the pair is not free");
        check(q::leave(), "the bracket closes");
        check(free_now(e), "once it closed, the pair is free");
        q::enter();
        check(free_now(e), "a bracket opened after the wrap does not hold it");
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
    check(!free_now(e), "a pair parked at the stalled epoch waits for the bracket");
    set_epoch(100 + q::kEpochWindow + 1);  // as if the counter had run on regardless
    check(!free_now(100 + q::kEpochWindow),
          "even 2^30 epochs on, the old bracket never reads as past the pair");
    set_epoch(100 + q::kStallLag);
    check(q::leave(), "the old bracket closes");
    check(!free_now(e), "the pair parked at the stalled epoch waits for the counter to move");
    const std::uint32_t after = q::advance();
    check(after == before && q::registry().ctl.epoch.load() == before + 1,
          "the next advance bumps again");
    check(free_now(e), "and the pair parked behind the old bracket is free");
}

/**
 * @brief The horizon is the arc @ref q::epoch_after gives each cell and the counter, met: it
 *        answers exactly what asking every one of them would, across the wrap too.
 */
void horizon_is_the_per_cell_answer() {
    std::printf("one horizon answers what a per-cell scan answers, for every epoch asked\n");
    std::uint32_t x = 0x9E3779B9u;
    const auto next = [&x] {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return x;
    };
    int wrong = 0;
    for (int round = 0; round < 2000; ++round) {
        const std::uint32_t c = next();
        // Up to four open brackets: near the counter, behind it by up to about kStallLag, a few
        // ahead of it, or anywhere at all (the arc argument needs no bound on the lag).
        std::uint32_t cells[4] = {};
        const std::size_t n = next() % 5;
        for (std::size_t i = 0; i < n; ++i) {
            const std::uint32_t lag = (next() % 4 == 0) ? next() % 8 : next() % (q::kStallLag + 8);
            const std::uint32_t pick = next() % 8;
            const std::uint32_t s = pick == 0 ? c + lag % 4 : pick == 1 ? next() : c - lag;
            cells[i] = (s & q::kEpochMask) | q::kOnlineBit;
        }
        // The test's own thread is offline here, so the cells set below are the only online ones.
        auto& cs = q::registry().cells;
        for (std::size_t i = 0; i < n; ++i) cs[cs.size() - 1 - i].state.store(cells[i]);
        const q::horizon_t h = q::scan<std::memory_order_acquire>(c);
        for (std::size_t i = 0; i < n; ++i) cs[cs.size() - 1 - i].state.store(0);
        for (int k = 0; k < 64; ++k) {
            const std::uint32_t e = (k < 8 ? cells[k % (n ? n : 1)] + (k % 3) - 1 : next());
            bool want = q::epoch_after(c, e);
            for (std::size_t i = 0; i < n; ++i) want = want && q::epoch_after(cells[i], e);
            wrong += h.past(e) != want;
        }
    }
    check(wrong == 0, "the arc and the per-cell answer agree");
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
    int asked = 0;
    const auto wants = [&asked, e](const q::horizon_t& h) { return ++asked, h.past(e); };
    check(q::horizon(wants).len == 0 && asked == 1,
          "inside the bracket: nothing past, and the unfenced scan stopped the pass");
    check(q::leave(), "the bracket closes");
    const q::horizon_t h = q::horizon(wants);
    check(h.past(e) && h.past(e - 1) && !h.past(e + 1),
          "once it closed: one fenced horizon covers every earlier epoch, not a later one");
}

}  // namespace

int main() {
    across_the_wrap();
    stall_behind_an_old_bracket();
    asymmetric_entry();
    horizon_is_the_per_cell_answer();
    return tr::testing::summary("qsbr_epoch_wrap");
}
