/**
 * @file
 * @brief `retire` against a concurrent registration under the retired prefix: the sweep-set
 *        entries the newcomer takes must survive the retire's sweep-set cleanup (#1884).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `graph_t::retire` retires the subtree under the unique map lock, releases it, and only then
 * takes `sweep_mutex_` to drop the subtree's entries from `pending_` and `unconditional_` (the
 * two locks never nest). A registration under the retired prefix that completes inside that
 * window — enrolled UNCONDITIONAL, or registered and then marked by an `assign` — owns a fresh
 * entry under the prefix, and a cleanup that erases the whole prefix run drops it: the vertex
 * keeps `delivery_mode == UNCONDITIONAL` (or its pending-mark hint) while no sweep will ever
 * deliver it again.
 *
 * The test runs rounds in lock step. In each, a retirer retires `/r/p` and re-registers it
 * and `/r/p/x` (UNCONDITIONAL), while a registrar registers `/r/p/y` UNCONDITIONAL and `/r/p/z`
 * IF_NEWER and assigns each a payload of its own. One subscriber on `/r`, taken before the
 * race starts, observes both, and is what makes `z`'s assign a mark. Once both workers are
 * done, the observer checks the one claim the round leaves decidable: whichever of `y` and `z`
 * is still registered must be delivered by a covering `propagate(/r)` — `y` because it is
 * UNCONDITIONAL, `z` because its assign marked it. A vertex the round's retire caught is not
 * registered and is not checked. Under TSan (the tsan CI lane builds this suite) the same run
 * also judges the cleanup's reads of vertex state against the registrar's writes. The
 * registrar takes no subscription of its own, so the run judges the sweep sets and not
 * subscribe-versus-retire.
 *
 * A second race (#1920) re-registers the retired address itself, on a thread whose every
 * allocation is refused. The retired vertex's entry is still in `unconditional_` when the
 * registration starts, so an UNCONDITIONAL registration that needs no new entry completes, and
 * one that does must answer BACKPRESSURE and leave the address unregistered. What it must never
 * do is register a vertex that is not UNCONDITIONAL, or one no covering sweep delivers.
 *
 * The round count defaults to @ref kRounds; a first argument overrides it, which is how the
 * 10^6-round acceptance run is made.
 */

#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>

#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::delivery_mode_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::value_t;

/** @brief Rounds a default run makes. A first command-line argument overrides it. */
constexpr long kRounds = 20000;
/** @brief Wall-clock ceiling of a default run; it stops early, not failing, once it is past.
 *         A run given an explicit round count makes every round. */
constexpr int kRunCeilingMs = 60000;
/** @brief Local subscribers hung on `/r/p/x` each round, so the retire's walk over the
 *         dropped slot tables — the window between the map unlock and the sweep lock — is
 *         not empty. */
constexpr int kWindowSubs = 32;
/** @brief The registrar's per-round delay, in spin iterations, is drawn below this mask. */
constexpr unsigned kJitterMask = 0x3fff;

using tr::testing::check;
using tr::testing::make_value;

/** @brief `y`'s payload byte; `z` carries @ref kZ, so one ancestor subscriber tells them apart. */
constexpr std::uint8_t kY = 0x01;
/** @brief `z`'s payload byte. */
constexpr std::uint8_t kZ = 0x02;

/** @brief Deliveries per payload byte, seen by the one subscriber on `/r`. */
struct counter_t {
    std::atomic<int> y{0}; /**< @brief Deliveries of `y`. */
    std::atomic<int> z{0}; /**< @brief Deliveries of `z`. */
    /** @brief Count one delivery by its payload byte. */
    void operator()(const value_t& v) {
        if (v.link_count() == 0 || v.total_length() == 0) return;
        const auto b = std::to_integer<std::uint8_t>(v.links()[0].bytes()[0]);
        if (b == kY) y.fetch_add(1, std::memory_order_relaxed);
        if (b == kZ) z.fetch_add(1, std::memory_order_relaxed);
    }
};

void test_newcomer_entries_survive_a_retire(long rounds, bool ceiling) {
    std::printf("retire vs registration under the prefix — sweep-set cleanup (#1884):\n");
    graph_t g;
    const auto root = g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    counter_t seen;
    counter_t on_x;  // the window's slot table; x carries no value, so it counts nothing
    check(g.subscribe(path_t("/r"), seen).has_value(), "the observer subscribes /r");
    const tr::graph::vertex_policy_t unconditional{.delivery_mode = delivery_mode_t::UNCONDITIONAL};

    std::barrier sync(3);
    std::atomic<bool> stop{false};
    std::thread retirer([&] {
        for (;;) {
            sync.arrive_and_wait();  // round start
            if (stop.load(std::memory_order_relaxed)) return;
            if (auto p = g.find(path_t("/r/p").key())) (void)g.retire(*p);
            (void)g.try_register_vertex(path_t("/r/p"), role_t::STORED_VALUE);
            if (auto x = g.try_register_vertex(path_t("/r/p/x"), role_t::STORED_VALUE, {},
                                               unconditional)) {
                for (int i = 0; i < kWindowSubs; ++i) (void)g.subscribe(path_t("/r/p/x"), on_x);
            }
            sync.arrive_and_wait();  // round end
        }
    });
    std::thread registrar([&] {
        unsigned spin = 1;
        for (;;) {
            sync.arrive_and_wait();
            if (stop.load(std::memory_order_relaxed)) return;
            // A varying delay scans the registration across the retire's window.
            spin = spin * 1103515245u + 12345u;
            for (unsigned i = (spin >> 16) & kJitterMask; i > 0; --i)
                std::atomic_signal_fence(std::memory_order_seq_cst);
            // A newcomer that survived the last round is assigned again, so the IF_NEWER one
            // carries a fresh mark into every check.
            (void)g.try_register_vertex(path_t("/r/p/y"), role_t::STORED_VALUE, {}, unconditional);
            (void)g.try_register_vertex(path_t("/r/p/z"), role_t::STORED_VALUE);
            if (auto y = g.find(path_t("/r/p/y").key())) (void)g.assign(*y, make_value({kY}));
            if (auto z = g.find(path_t("/r/p/z").key())) (void)g.assign(*z, make_value({kZ}));
            sync.arrive_and_wait();
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kRunCeilingMs);
    long done = 0;
    long checked_y = 0;
    long checked_z = 0;
    long lost_y = 0;
    long lost_z = 0;
    for (; done < rounds; ++done) {
        sync.arrive_and_wait();
        sync.arrive_and_wait();
        // Quiescent: both workers wait at the next round's start.
        const bool y_live = g.find(path_t("/r/p/y").key()).has_value();
        const bool z_live = g.find(path_t("/r/p/z").key()).has_value();
        seen.y.store(0, std::memory_order_relaxed);
        seen.z.store(0, std::memory_order_relaxed);
        (void)g.propagate(root);
        if (y_live) {
            ++checked_y;
            if (seen.y.load(std::memory_order_relaxed) == 0) ++lost_y;
        }
        if (z_live) {
            ++checked_z;
            if (seen.z.load(std::memory_order_relaxed) == 0) ++lost_z;
        }
        if (lost_y + lost_z > 0) break;  // the defect, observed
        if (ceiling && (done & 0x3ff) == 0 && std::chrono::steady_clock::now() > deadline) break;
    }
    stop.store(true, std::memory_order_relaxed);
    sync.arrive_and_wait();
    retirer.join();
    registrar.join();

    std::printf("    %ld rounds, %ld checks of y, %ld of z, lost: y=%ld z=%ld\n", done, checked_y,
                checked_z, lost_y, lost_z);
    check(lost_y == 0, "a registered UNCONDITIONAL newcomer stays in its sweep set");
    check(lost_z == 0, "a registered, marked IF_NEWER newcomer keeps its pending mark");
    // Liveness: a run in which no newcomer ever survived a round checked nothing.
    check(checked_y > 0 && checked_z > 0,
          "the newcomers survived some rounds (the racer was live)");
}

/**
 * @brief A heap source that refuses every request made on a thread that raised @ref refuse_here,
 *        so one racer can be starved while the other is served.
 */
class refusing_source_t final : public tr::mem::block_source_t {
   public:
    refusing_source_t() noexcept : tr::mem::block_source_t("refusing") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse_here) return nullptr;
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        ::operator delete(p, bytes, std::align_val_t{align});
    }

    /** @brief Raised on the starved thread: every request it makes is refused. */
    static thread_local bool refuse_here;
};

thread_local bool refusing_source_t::refuse_here = false;

void test_refused_mode_landing_registers_nothing(long rounds, bool ceiling) {
    std::printf("re-registration of a retired address, every allocation refused (#1920):\n");
    refusing_source_t src;
    graph_t g{src};
    const auto root = g.register_vertex(path_t("/r"), role_t::STORED_VALUE);
    counter_t seen;
    counter_t on_p;  // the window's slot table; nothing is assigned while it hangs, so it counts 0
    check(g.subscribe(path_t("/r"), seen).has_value(), "the observer subscribes /r");
    const tr::graph::vertex_policy_t unconditional{.delivery_mode = delivery_mode_t::UNCONDITIONAL};

    std::barrier sync(3);
    std::atomic<bool> stop{false};
    std::thread retirer([&] {
        for (;;) {
            sync.arrive_and_wait();
            if (stop.load(std::memory_order_relaxed)) return;
            if (auto p = g.find(path_t("/r/p").key())) (void)g.retire(*p);
            sync.arrive_and_wait();
        }
    });
    tr::graph::result_t<tr::graph::vertex_handle_t> got = std::unexpected(status_t::NOT_FOUND);
    std::thread registrar([&] {
        refusing_source_t::refuse_here = true;
        unsigned spin = 1;
        for (;;) {
            sync.arrive_and_wait();
            if (stop.load(std::memory_order_relaxed)) return;
            spin = spin * 1103515245u + 12345u;
            for (unsigned i = (spin >> 16) & kJitterMask; i > 0; --i)
                std::atomic_signal_fence(std::memory_order_seq_cst);
            got = g.try_register_vertex(path_t("/r/p"), role_t::STORED_VALUE, {}, unconditional);
            sync.arrive_and_wait();
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(kRunCeilingMs);
    long done = 0;
    long registered = 0;
    long refused = 0;
    long wrong_mode = 0;
    long unswept = 0;
    long half_made = 0;
    for (; done < rounds; ++done) {
        // Quiescent: the address is registered UNCONDITIONAL, by this thread, before each round.
        if (!g.find(path_t("/r/p").key())) {
            (void)g.try_register_vertex(path_t("/r/p"), role_t::STORED_VALUE, {}, unconditional);
            for (int i = 0; i < kWindowSubs; ++i) (void)g.subscribe(path_t("/r/p"), on_p);
        }
        sync.arrive_and_wait();
        sync.arrive_and_wait();
        if (got) {
            ++registered;
            // The handle is the vertex pointer; the mode has no graph-level getter.
            if (std::bit_cast<tr::graph::vertex_t*>(*got)->delivery_mode() !=
                delivery_mode_t::UNCONDITIONAL)
                ++wrong_mode;
            // An assign marks nothing on an UNCONDITIONAL vertex: only its set entry delivers it.
            (void)g.assign(*got, make_value({kY}));
            seen.y.store(0, std::memory_order_relaxed);
            (void)g.propagate(root);
            if (seen.y.load(std::memory_order_relaxed) == 0) ++unswept;
        } else if (got.error() == status_t::BACKPRESSURE) {
            ++refused;
            if (g.find(path_t("/r/p").key())) ++half_made;
        }
        if (wrong_mode + unswept + half_made > 0) break;  // the defect, observed
        if (ceiling && (done & 0x3ff) == 0 && std::chrono::steady_clock::now() > deadline) break;
    }
    stop.store(true, std::memory_order_relaxed);
    sync.arrive_and_wait();
    retirer.join();
    registrar.join();

    std::printf(
        "    %ld rounds, %ld registered, %ld refused; wrong mode=%ld unswept=%ld "
        "half-made=%ld\n",
        done, registered, refused, wrong_mode, unswept, half_made);
    check(wrong_mode == 0, "a registration that answers success landed its UNCONDITIONAL mode");
    check(unswept == 0, "a registered UNCONDITIONAL vertex is in its sweep set");
    check(half_made == 0, "a registration refused BACKPRESSURE leaves the address unregistered");
    // Liveness: the registrar was refused. A win needs the whole registration inside the
    // retire's window, which a loaded host can starve, so wins are reported, not required.
    check(refused > 0, "the registrar was refused (the race was live)");
}

}  // namespace

int main(int argc, char** argv) {
    const long rounds = argc > 1 ? std::strtol(argv[1], nullptr, 10) : kRounds;
    test_newcomer_entries_survive_a_retire(rounds, argc <= 1);
    test_refused_mode_landing_registers_nothing(rounds, argc <= 1);
    return tr::testing::summary("retire_sweep_race");
}
