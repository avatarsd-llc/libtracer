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
 * After #1533 a cleared or suspended slot contributes no published entry, so both arms should
 * read as `live`: the partition shape, with the non-delivering tail elided.
 *
 * Rows are `RESULT <arm> size=<S> fan=<N> ns_per_write=<best> mdeliv_per_s=<N/best>
 * ram_bytes=<held>`: the write's latency, the deliveries it sustains, and the bytes the graph's
 * source holds once the fixture is built (its pooled tables, at slab granularity; the published
 * array's own width is `vertex_t::published_edges()` entries of 56 B). Each point is the BEST of
 * `kRounds` rounds, the arms interleaved within a round (contamination is one-sided, so the best
 * round is the instrument; see bench/README.md). An arm name as `argv[1]` runs that arm alone.
 * Deliveries are counted and a short count aborts the row: a cheap write that delivered less is not
 * a result.
 *
 * `TOGGLE fan=<N> ns_per_switch=<best>` prices switching one edge off and another on with N
 * live edges standing: a suspend plus a resume, or, on a tree without `set_suspended`, the
 * unsubscribe plus re-subscribe it replaces. The file builds on both trees (a `requires` test
 * picks the arm), so one source is the A/B instrument.
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

/** @brief Price @p k switches with @p n live edges standing; ns per switch (off + on). */
double time_toggles(std::size_t n, std::size_t k) {
    counting_source_t mem;
    graph_t g{mem};
    (void)g.register_vertex(path_t("/bench/src"), role_t::STORED_VALUE);
    const path_t p("/bench/src");
    for (std::size_t i = 0; i < n; ++i)
        if (!g.subscribe(p, sink, nullptr)) std::abort();
    auto a = g.subscribe(p, sink, nullptr);
    if (!a) std::abort();
    const std::uint64_t t0 = bench::now_ns();
    for (std::size_t i = 0; i < k; ++i) {
        if constexpr (kHasSuspend<graph_t>) {
            if (!suspend(g, *a, true) || !suspend(g, *a, false)) std::abort();
        } else {
            if (!g.unsubscribe(*a)) std::abort();
            a = g.subscribe(p, sink, nullptr);
            if (!a) std::abort();
        }
    }
    return static_cast<double>(bench::now_ns() - t0) / static_cast<double>(k);
}

}  // namespace

int main(int argc, char** argv) {
    // An optional arm name runs that arm alone (`toggle` for the switch rows): one arm per
    // process keeps a cross-tree A/B from comparing processes whose heaps saw different arms.
    const std::string_view only = argc > 1 ? std::string_view{argv[1]} : std::string_view{};
    const auto wanted = [only](std::string_view arm) { return only.empty() || only == arm; };
    constexpr std::array<std::size_t, 3> kSizes{64, 1024, 16384};
    constexpr std::array<std::size_t, 4> kFans{1, 8, 32, 1024};
    constexpr std::array<const char*, 3> kNames{"live", "cleared", "suspended"};
    constexpr std::size_t kArms = kHasSuspend<graph_t> ? 3 : 2;
    for (std::size_t s : kSizes) {
        const std::vector<std::byte> tlv = value_tlv(s);
        for (std::size_t n : kFans) {
            const std::size_t writes = std::max<std::size_t>(200, 4'000'000 / (n * 8));
            std::array<double, 3> best;
            std::array<std::size_t, 3> ram{};
            best.fill(std::numeric_limits<double>::infinity());
            for (int r = 0; r < kRounds; ++r) {
                for (std::size_t a = 0; a < kArms; ++a) {
                    if (!wanted(kNames[a])) continue;
                    fixture_t fx(static_cast<arm_t>(a), n);
                    ram[a] = fx.ram;
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
    for (std::size_t n : kFans) {
        if (!wanted("toggle")) break;
        double best = std::numeric_limits<double>::infinity();
        for (int r = 0; r < kRounds; ++r) best = std::min(best, time_toggles(n, 20'000 / n + 50));
        std::printf("TOGGLE %s fan=%zu ns_per_switch=%.1f\n",
                    kHasSuspend<graph_t> ? "suspend+resume" : "unsubscribe+subscribe", n, best);
    }
    return 0;
}
