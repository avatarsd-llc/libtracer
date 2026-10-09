/**
 * @file
 * @brief #2024: what a subscription-selector switch costs, and what the selector's presence
 *        costs a fan-out write.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `SWITCH refs=<K> fan=<N> ns_per_switch=<best>` — one producer with N callback edges; a
 * `subscription_selector_t<64, 16>` lists K of them in option `a` and K others in option `b`
 * (the rest are plain live edges), and the timed loop alternates `select("a")` / `select("b")`.
 * Each switch suspends K edges and resumes K. K is 1 (one option per target) and
 * `min(32, N / 2)` (two wide options). At N = 1 there is one edge: `a` lists it and the switch
 * is `a` <-> none.
 *
 * `SWITCH_FIELD refs=1 fan=<N> ns_per_switch=<best>` — the same K = 1 switch spelled as the
 * wire spells it: a `:settings.app.active` field write of `NAME <option>`.
 *
 * `RESULT <arm> size=<S> fan=<N> ns_per_write=<best> mdeliv_per_s=<N/best> ram_bytes=<held>` —
 * a write to a producer with N delivering edges. `plain`: those N edges alone. `selector`: the
 * same N, of which `min(32, N)` are listed in the active option `a`, plus `min(32, N)` more,
 * listed in option `b` and therefore suspended. The selector itself is not on the write path;
 * what the arm prices is the suspended edges it keeps standing (#1533's skipped entries).
 * `ram_bytes` is what the graph's source holds once the fixture is built, `sizeof` the
 * selector is printed once as `RAM`.
 *
 * Each point is the BEST of `kRounds` rounds with the arms interleaved (bench/README.md).
 * Usage: `bench_subscription_selector [switch|field|plain|selector [fan]]`.
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
#include "libtracer/subscription_selector.hpp"
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
using selector_t = tr::graph::subscription_selector_t<64, 16>;

constexpr int kRounds = 7; /**< @brief Rounds per point; the best one is reported. */

/** @brief The delivery counter every delivering edge bumps. */
std::uint64_t g_seen = 0;

/** @brief The edges' sink. */
void sink(void*, const value_t&) { ++g_seen; }

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

/** @brief A fresh owned view of @p bytes — one segment and one copy per write. */
view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief A producer with @p n edges and a selector whose options `a` and `b` list @p k each
 *         (b's suspended), plus @p extra suspended edges listed in `b` beyond the @p n. */
struct fixture_t {
    counting_source_t mem; /**< @brief Counts the graph. */
    graph_t g{mem};        /**< @brief The graph. */
    vertex_handle_t src;   /**< @brief The producer. */
    selector_t sel{g};     /**< @brief The selector. */
    std::size_t ram = 0;   /**< @brief Bytes held once built. */

    /** @brief Build it; @p with_selector false leaves the selector unattached and empty. */
    fixture_t(std::size_t n, std::size_t k, std::size_t extra, bool with_selector)
        : src(g.register_vertex(path_t("/bench/src"), role_t::STORED_VALUE)) {
        const path_t p("/bench/src");
        std::vector<subscription_t> subs;
        for (std::size_t i = 0; i < n + extra; ++i) {
            const auto s = g.subscribe(p, sink, nullptr);
            if (!s) std::abort();
            subs.push_back(*s);
        }
        if (with_selector) {
            if (!sel.attach(path_t("/bench/sel"))) std::abort();
            // a = edges [0, k); b = edges [n - k, n) when switching, or the extra ones.
            const std::size_t b0 = extra != 0 ? n : n - std::min(k, n - k);
            for (std::size_t i = 0; i < k; ++i)
                if (!sel.add("a", subs[i])) std::abort();
            for (std::size_t i = b0; i < b0 + (extra != 0 ? extra : std::min(k, n - k)); ++i)
                if (!sel.add("b", subs[i])) std::abort();
            if (!sel.select("a")) std::abort();
        }
        ram = mem.held();
    }
};

/** @brief Time @p writes writes; ns per write. Aborts on a short delivery count. */
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

/** @brief A `NAME <text>` TLV as a written value. */
view_t name_value(std::string_view text) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, text);
    return owned_view(out);
}

/** @brief Time @p steps switches at fan @p n with @p k refs per option, through the host API
 *         or (@p field) the `active` field write; ns per switch. */
double time_switches(std::size_t n, std::size_t k, std::size_t steps, bool field) {
    fixture_t fx(n, k, 0, true);
    const tr::graph::field_path_t active = path_t("/x:settings.app.active").field();
    // At N = 1 there is no second edge for `b`: the switch is then `a` <-> none.
    const bool has_b = n >= 2 * k;
    const view_t a = name_value("a");
    std::vector<std::byte> none_tlv;
    tr::wire::emit_tlv(none_tlv, tr::wire::type_t::STATUS, {}, {});
    const view_t b = has_b ? name_value("b") : owned_view(none_tlv);
    const vertex_handle_t sel = *fx.sel.vertex();
    const std::uint64_t t0 = bench::now_ns();
    for (std::size_t i = 0; i < steps; ++i) {
        const bool to_b = (i & 1u) == 0;
        const bool ok = field ? fx.g.write(sel, active, to_b ? b : a).has_value()
                              : fx.sel.select(to_b ? (has_b ? "b" : "") : "a").has_value();
        if (!ok) std::abort();
    }
    return static_cast<double>(bench::now_ns() - t0) / static_cast<double>(steps);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view only = argc > 1 ? std::string_view{argv[1]} : std::string_view{};
    const std::size_t only_fan = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 0;
    const auto wanted = [only](std::string_view arm) { return only.empty() || only == arm; };
    const auto fan_wanted = [only_fan](std::size_t n) { return only_fan == 0 || only_fan == n; };
    constexpr std::array<std::size_t, 3> kFans{1, 32, 1024};
    constexpr std::array<std::size_t, 4> kSizes{64, 1024, 4096, 16384};
    std::printf("RAM sizeof_selector_8x4=%zu sizeof_selector_64x16=%zu sizeof_subscription=%zu\n",
                sizeof(tr::graph::subscription_selector_t<>), sizeof(selector_t),
                sizeof(subscription_t));

    for (std::size_t n : kFans) {
        if (!fan_wanted(n)) continue;
        for (const std::size_t k : {std::size_t{1}, std::min<std::size_t>(32, n / 2)}) {
            if (k == 0) continue;
            for (const bool field : {false, true}) {
                if (!wanted(field ? "field" : "switch") || (field && k != 1)) continue;
                double best = std::numeric_limits<double>::infinity();
                for (int r = 0; r < kRounds; ++r)
                    best = std::min(best, time_switches(n, k, 20'000 / k + 50, field));
                std::printf("%s refs=%zu fan=%zu ns_per_switch=%.1f\n",
                            field ? "SWITCH_FIELD" : "SWITCH", k, n, best);
            }
        }
        std::fflush(stdout);
    }

    constexpr std::array<const char*, 2> kArms{"plain", "selector"};
    for (std::size_t s : kSizes) {
        const std::vector<std::byte> tlv = value_tlv(s);
        for (std::size_t n : kFans) {
            if (!fan_wanted(n)) continue;
            const std::size_t writes = std::max<std::size_t>(200, 4'000'000 / (n * 8));
            const std::size_t k = std::min<std::size_t>(32, n);
            std::array<double, 2> best;
            std::array<std::size_t, 2> ram{};
            best.fill(std::numeric_limits<double>::infinity());
            for (int r = 0; r < kRounds; ++r) {
                for (std::size_t a = 0; a < kArms.size(); ++a) {
                    if (!wanted(kArms[a])) continue;
                    fixture_t fx(n, k, a == 1 ? k : 0, a == 1);
                    ram[a] = fx.ram;
                    (void)time_writes(fx, tlv, writes / 10 + 1, n);  // warm-up
                    best[a] = std::min(best[a], time_writes(fx, tlv, writes, n));
                }
            }
            for (std::size_t a = 0; a < kArms.size(); ++a)
                if (wanted(kArms[a]))
                    std::printf(
                        "RESULT %s size=%zu fan=%zu ns_per_write=%.1f mdeliv_per_s=%.2f "
                        "ram_bytes=%zu\n",
                        kArms[a], s, n, best[a], static_cast<double>(n) * 1e3 / best[a], ram[a]);
            std::fflush(stdout);
        }
    }
    return 0;
}
