/**
 * @file
 * @brief The QSBR domain's 31-bit epoch: a wrap or a long-open bracket delays a free, never
 *        allows one (`qsbr.hpp`, section "The 31-bit epoch").
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The counter is set directly, so the test reaches the wrap and the stall lag without 2^29
 * real retirements. The protocol runs against a domain the test owns (#2119), whose
 * participants it opens and closes by index on one thread; only the asymmetric-entry case uses
 * the default domain, because the `membarrier` registration is the default's.
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "libtracer/qsbr.hpp"
#include "test_support.hpp"

namespace {

namespace q = tr::graph::detail_qsbr;
using tr::testing::check;

/** @brief The test's own domain: no library call site and no other thread touches it. */
constinit q::registry_t g_domain{};
/** @brief The test domain's retired slots, beside it (#2111). */
constinit q::retired_table_t g_table{};

void set_epoch(std::uint32_t e, q::registry_t& d = g_domain) {
    d.ctl.epoch.store(e, std::memory_order_seq_cst);
}

/** @brief The question `retire` asks: is a pair retired at @p e free now, after one fence? */
[[nodiscard]] bool free_now(std::uint32_t e, q::registry_t& d = g_domain) {
    return q::horizon([e](const q::horizon_t& h) { return h.past(e); }, d).past(e);
}

/** @brief Open participant @p i of the test's domain. */
void open_at(std::size_t i) { q::go_online(g_domain, static_cast<std::uint32_t>(i + 1)); }

/** @brief Close participant @p i of the test's domain. */
void close_at(std::size_t i) { q::go_offline(g_domain, static_cast<std::uint32_t>(i + 1)); }

/** @brief Across the wrap, a pair retired before it waits for the bracket and then frees. */
void across_the_wrap() {
    std::printf("a bracket open across the 2^31 wrap holds the pair retired inside it\n");
    for (const std::uint32_t start : {q::kEpochMask - 1, q::kEpochMask, 0xFFFFFFFFu}) {
        set_epoch(start);
        open_at(0);
        const std::uint32_t e = q::advance(g_domain);
        (void)q::advance(g_domain);
        (void)q::advance(g_domain);  // the counter is now past the wrap
        check(!free_now(e), "inside the bracket, the pair is not free");
        close_at(0);
        check(free_now(e), "once it closed, the pair is free");
        open_at(0);
        check(free_now(e), "a bracket opened after the wrap does not hold it");
        close_at(0);
    }
}

/** @brief A bracket @ref q::kStallLag behind stalls the counter; frees wait for it. */
void stall_behind_an_old_bracket() {
    std::printf("the counter stalls behind a bracket open across 2^29 advances\n");
    set_epoch(100);
    open_at(0);  // online at 100
    set_epoch(100 + q::kStallLag);
    const std::uint32_t before = g_domain.ctl.epoch.load();
    const std::uint32_t e = q::advance(g_domain);
    check(e == before && g_domain.ctl.epoch.load() == before,
          "advance answers the current epoch and does not bump it");
    check(!free_now(e), "a pair parked at the stalled epoch waits for the bracket");
    set_epoch(100 + q::kEpochWindow + 1);  // as if the counter had run on regardless
    check(!free_now(100 + q::kEpochWindow),
          "even 2^30 epochs on, the old bracket never reads as past the pair");
    set_epoch(100 + q::kStallLag);
    close_at(0);
    check(!free_now(e), "the pair parked at the stalled epoch waits for the counter to move");
    const std::uint32_t after = q::advance(g_domain);
    check(after == before && g_domain.ctl.epoch.load() == before + 1,
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
        // Every participant of the test's domain is offline here, so these are the only online
        // ones.
        auto& cs = g_domain.cells;
        for (std::size_t i = 0; i < n; ++i) cs[cs.size() - 1 - i].state.store(cells[i]);
        const q::horizon_t h = q::scan<std::memory_order_acquire>(c, g_domain);
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
    // A lane that opts in sets LT_EXPECT_MEMBARRIER=1: there, a build without the plain entry
    // or a refused registration (an older kernel, a seccomp filter, a container) is a failure,
    // never a silent fall back to the fenced entry that would leave the lane testing nothing new.
    // The lane that runs under a seccomp filter refusing membarrier (#2121) sets
    // LT_EXPECT_MEMBARRIER=refused: there the fence must be compiled in AND its registration
    // refused, or the lane is not testing the fenced fallback it exists for.
    const char* const want = std::getenv("LT_EXPECT_MEMBARRIER");
    if (want != nullptr && std::strcmp(want, "1") == 0)
        check(q::kMembarrier && q::asym_ready(),
              "LT_EXPECT_MEMBARRIER=1: the asymmetric fence is compiled in and registered");
    if (want != nullptr && std::strcmp(want, "refused") == 0)
        check(q::kMembarrier && !q::asym_ready(),
              "LT_EXPECT_MEMBARRIER=refused: the asymmetric fence is compiled in, its "
              "registration was refused, and the fenced entry is in use");
    if constexpr (!q::kMembarrier) {
        std::printf(
            "  (not compiled in on this target: the entry is the store and seq_cst fence)\n");
        return;
    }
    const bool on = q::asym_ready();
    std::printf("  membarrier registration: %s\n",
                on ? "accepted, plain entry in use" : "refused, fenced entry kept");
    q::registry_t& d = q::registry();
    set_epoch(500, d);
    q::enter();
    const std::uint32_t e = q::advance(d);
    int asked = 0;
    const auto wants = [&asked, e](const q::horizon_t& h) { return ++asked, h.past(e); };
    check(q::horizon(wants).len == 0 && asked == 1,
          "inside the bracket: nothing past, and the scan before membarrier stopped the pass");
    check(q::leave(), "the bracket closes");
    const q::horizon_t h = q::horizon(wants);
    check(h.past(e) && h.past(e - 1) && !h.past(e + 1),
          "once it closed: one fenced horizon covers every earlier epoch, not a later one");
}

int g_released = 0;

void count_release(void* ctx) { g_released += *static_cast<int*>(ctx); }

/**
 * @brief Two participants and the shared retired table, stepped on one thread: a pair waits for
 *        every bracket open when it retired, not for one opened after, and the drain at the last
 *        close runs its hook once.
 */
void protocol_on_one_thread() {
    std::printf("two participants of a private domain, stepped on one thread\n");
    set_epoch(7);
    g_released = 0;
    int one = 1;
    check(q::retire(&one, count_release, g_domain, g_table) && g_released == 1,
          "with nobody inside, a retire releases inline");
    open_at(0);
    int ten = 10;
    check(q::retire(&ten, count_release, g_domain, g_table) && g_released == 1,
          "a bracket open at the retire parks the pair");
    check(g_domain.ctl.live.load() == 1, "in the shared table");
    open_at(1);  // opened after the retire: it must not hold the pair
    q::drain(g_domain, g_table);
    check(g_released == 1, "the drain while participant 0 is inside frees nothing");
    close_at(0);
    q::drain(g_domain, g_table);
    check(g_released == 11 && g_domain.ctl.live.load() == 0,
          "once participant 0 closes, the drain frees it, though participant 1 is still inside");
    close_at(1);
    q::drain(g_domain, g_table);
    check(g_released == 11, "and the hook ran once");
    check(q::registry().ctl.live.load() == 0, "the default domain saw none of it");
}

}  // namespace

int main() {
    protocol_on_one_thread();
    across_the_wrap();
    stall_behind_an_old_bracket();
    asymmetric_entry();
    horizon_is_the_per_cell_answer();
    return tr::testing::summary("qsbr_epoch_wrap");
}
