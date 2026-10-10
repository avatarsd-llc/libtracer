/**
 * @file
 * @brief #1533: what M non-delivering edges cost a write that fans out to N live ones.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Three arms per (payload, fan-out N), each one producer vertex with N live callback edges:
 *
 *   - `live`      — the N edges alone: the floor.
 *   - `cleared`   — N live edges interleaved with N slots that were subscribed and then
 *                   unsubscribed. Before #1533 the published array mirrored the slot table, so
 *                   every cleared slot was a dead entry the copy loop loaded and skipped: this
 *                   arm, built on a pre-#1533 tree, IS the "bit test" shape the issue weighed.
 *   - `suspended` — N live edges interleaved with N suspended ones (`graph_t::set_suspended`).
 *
 * After #1533 a cleared slot contributes no published entry, so `cleared` should read as
 * `live`; a suspended slot keeps its entry with the liveness bit clear (that is what makes a
 * toggle allocation-free), so `suspended` reads as the pre-#1533 `cleared` arm did.
 *
 * Rows are `RESULT <arm> size=<S> fan=<N> ns_per_write=<best> mdeliv_per_s=<N/best>
 * ram_bytes=<held>`: the write's latency, the deliveries it sustains, and the bytes the graph's
 * source holds once the fixture is built (its pooled tables, at slab granularity; the published
 * array's own width is `vertex_t::published_edges()` entries of 56 B on LP64). Each point is the
 * BEST of `kRounds` rounds, the arms interleaved within a round (contamination is one-sided, so the
 * best round is the instrument; see bench/README.md). An arm name as `argv[1]` runs that arm alone.
 * Deliveries are counted and a short count aborts the row: a cheap write that delivered less is not
 * a result.
 *
 * `TOGGLE <how> fan=<N> ns_per_switch=<best>` prices switching delivery from one extra edge to
 * another with N live edges standing: suspend the one and resume the other, or, on a tree
 * without `set_suspended`, unsubscribe the one and subscribe the other. `CHURN fan=<N>
 * ns_per_pair=<best>` prices one unsubscribe plus re-subscribe on EITHER tree — what every
 * republish costs, which #1533's count pass lands on. The file builds on both trees (a
 * `requires` test picks the arm), so one source is the A/B instrument.
 *
 * Usage: `bench_subscriber_suspend [arm [fan [writes]]]` — `arm` is `live`, `cleared`,
 * `suspended`, `toggle` or `churn` (one arm per process keeps a cross-tree A/B from comparing
 * heaps that saw different arms); `fan` keeps one fan-out; `writes` overrides the timed write
 * count, and `0` builds the fixtures and writes nothing, so a `perf stat` of the two runs'
 * difference is the timed loop alone, without the O(N^2) fixture build.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::subscription_t;
using tr::graph::value_t;
using tr::graph::vertex_handle_t;
using tr::view::view_t;

constexpr int kRounds = 7; /**< @brief Rounds per point; the best one is reported. */

/** @brief Does this tree have the #1533 in-place suspend? */
template <class G>
constexpr bool kHasSuspend = requires(G& g, const subscription_t& s) { g.set_suspended(s, true); };

/** @brief One arm: how it treats the N interleaved non-delivering edges. */
enum class arm_t { LIVE, CLEARED, SUSPENDED };

/** @brief The delivery counter every live edge bumps. */
std::uint64_t g_seen = 0;

/** @brief The live edges' sink. */
void sink(void*, const value_t&) { ++g_seen; }

/** @brief The non-delivering edges' sink: reaching it is a bench failure. */
void must_not_run(void*, const value_t&) {
    std::fprintf(stderr, "a cleared or suspended edge was delivered to\n");
    std::abort();
}

/** @brief A pass-through source that counts the bytes it holds: the fixture's RAM. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : tr::mem::block_source_t("bench-count") {}

    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* p = ::operator new(bytes, std::align_val_t{align}, std::nothrow);
        if (p != nullptr) held_ += bytes;
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        held_ -= bytes;
        ::operator delete(p, std::align_val_t{align});
    }
    /** @brief Bytes currently held. */
    [[nodiscard]] std::size_t held() const noexcept { return held_; }

   private:
    std::size_t held_ = 0; /**< @brief Live byte balance. */
};

/** @brief A VALUE TLV of @p n payload bytes. */
std::vector<std::byte> value_tlv(std::size_t n) {
    const std::vector<std::byte> p(n, std::byte{0xAB});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, p);
    return out;
}

/** @brief A fresh owned view of @p bytes — one segment and one copy per write, as `inproc`. */
view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief Suspend @p s on a tree that can; elsewhere the arm is never built. */
template <class G>
bool suspend(G& g, const subscription_t& s, bool on) {
    if constexpr (kHasSuspend<G>)
        return g.set_suspended(s, on).has_value();
    else
        return false;
}

/** @brief One arm's fixture: the counted source, the graph, the producer, its edges. */
struct fixture_t {
    counting_source_t src_mem; /**< @brief Counts what the graph holds. */
    graph_t g{src_mem};        /**< @brief The graph. */
    vertex_handle_t src;       /**< @brief The producer. */
    std::size_t ram = 0;       /**< @brief Bytes held once built. */

    /** @brief Build @p arm at fan-out @p n. */
    fixture_t(arm_t arm, std::size_t n)
        : src(g.register_vertex(path_t("/bench/src"), role_t::STORED_VALUE)) {
        const path_t p("/bench/src");
        for (std::size_t i = 0; i < n; ++i) {
            if (!g.subscribe(p, sink, nullptr)) std::abort();
            if (arm == arm_t::LIVE) continue;
            const auto dead = g.subscribe(p, must_not_run, nullptr);
            if (!dead) std::abort();
            if (arm == arm_t::CLEARED ? !g.unsubscribe(*dead) : !suspend(g, *dead, true))
                std::abort();
        }
        ram = src_mem.held();
    }
};

/** @brief Time @p writes writes on @p fx; ns per write. Aborts on a short delivery count. */
double time_writes(fixture_t& fx, const std::vector<std::byte>& tlv, std::size_t writes,
                   std::size_t n) {
    g_seen = 0;
    const std::uint64_t t0 = bench::now_ns();
    for (std::size_t i = 0; i < writes; ++i) (void)fx.g.write(fx.src, owned_view(tlv));
    const std::uint64_t t1 = bench::now_ns();
    if (g_seen != writes * n) {
        std::fprintf(stderr, "short delivery: %llu of %zu\n",
                     static_cast<unsigned long long>(g_seen), writes * n);
        std::abort();
    }
    return static_cast<double>(t1 - t0) / static_cast<double>(writes);
}

/** @brief Price @p k control-plane steps with @p n live edges standing: a SWITCH from one
 *         extra edge to another (@p churn false), or an unsubscribe plus re-subscribe of one
 *         (@p churn true, on either tree). ns per step. */
double time_steps(std::size_t n, std::size_t k, bool churn) {
    counting_source_t mem;
    graph_t g{mem};
    (void)g.register_vertex(path_t("/bench/src"), role_t::STORED_VALUE);
    const path_t p("/bench/src");
    for (std::size_t i = 0; i < n; ++i)
        if (!g.subscribe(p, sink, nullptr)) std::abort();
    // N + 1 delivering edges on both trees: `on` delivers; `off`, where it exists, is the
    // suspended edge a switch resumes.
    auto on = g.subscribe(p, sink, nullptr);
    if (!on) std::abort();
    const bool in_place = kHasSuspend<graph_t> && !churn;
    std::optional<subscription_t> off;
    if (in_place) {
        auto s = g.subscribe(p, sink, nullptr);
        if (!s || !suspend(g, *s, true)) std::abort();
        off = *s;
    }
    const std::uint64_t t0 = bench::now_ns();
    for (std::size_t i = 0; i < k; ++i) {
        if (in_place) {
            if (!suspend(g, *on, true) || !suspend(g, *off, false)) std::abort();
            std::swap(*on, *off);
        } else {
            if (!g.unsubscribe(*on)) std::abort();
            if (!(on = g.subscribe(p, sink, nullptr))) std::abort();
        }
    }
    return static_cast<double>(bench::now_ns() - t0) / static_cast<double>(k);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view only = argc > 1 ? std::string_view{argv[1]} : std::string_view{};
    const std::size_t only_fan = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
    const bool writes_set = argc > 3;
    const std::size_t writes_arg = writes_set ? std::strtoull(argv[3], nullptr, 10) : 0;
    const auto wanted = [only](std::string_view arm) { return only.empty() || only == arm; };
    const auto fan_wanted = [only_fan](std::size_t n) { return only_fan == 0 || only_fan == n; };
    constexpr std::array<std::size_t, 4> kSizes{64, 1024, 4096, 16384};
    constexpr std::array<std::size_t, 4> kFans{1, 8, 32, 1024};
    constexpr std::array<const char*, 3> kNames{"live", "cleared", "suspended"};
    constexpr std::size_t kArms = kHasSuspend<graph_t> ? 3 : 2;
    for (std::size_t s : kSizes) {
        const std::vector<std::byte> tlv = value_tlv(s);
        for (std::size_t n : kFans) {
            if (!fan_wanted(n)) continue;
            const std::size_t writes =
                writes_set ? writes_arg : std::max<std::size_t>(200, 4'000'000 / (n * 8));
            std::array<double, 3> best;
            std::array<std::size_t, 3> ram{};
            best.fill(std::numeric_limits<double>::infinity());
            for (int r = 0; r < kRounds; ++r) {
                for (std::size_t a = 0; a < kArms; ++a) {
                    if (!wanted(kNames[a])) continue;
                    fixture_t fx(static_cast<arm_t>(a), n);
                    ram[a] = fx.ram;
                    if (writes == 0) continue;                       // fixture build only
                    (void)time_writes(fx, tlv, writes / 10 + 1, n);  // warm-up
                    best[a] = std::min(best[a], time_writes(fx, tlv, writes, n));
                }
            }
            for (std::size_t a = 0; a < kArms; ++a)
                if (wanted(kNames[a]))
                    std::printf(
                        "RESULT %s size=%zu fan=%zu ns_per_write=%.1f mdeliv_per_s=%.2f "
                        "ram_bytes=%zu\n",
                        kNames[a], s, n, best[a], static_cast<double>(n) * 1e3 / best[a], ram[a]);
            std::fflush(stdout);
        }
    }
    for (const bool churn : {false, true}) {
        if (!wanted(churn ? "churn" : "toggle")) continue;
        for (std::size_t n : kFans) {
            if (!fan_wanted(n)) continue;
            double best = std::numeric_limits<double>::infinity();
            for (int r = 0; r < kRounds; ++r)
                best = std::min(best, time_steps(n, 20'000 / n + 50, churn));
            if (churn)
                std::printf("CHURN fan=%zu ns_per_pair=%.1f\n", n, best);
            else
                std::printf("TOGGLE %s fan=%zu ns_per_switch=%.1f\n",
                            kHasSuspend<graph_t> ? "suspend+resume" : "unsubscribe+subscribe", n,
                            best);
        }
    }
    return 0;
}
