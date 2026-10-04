/**
 * @file
 * @brief The host guard sized by the target (#1716): stripe alignment follows
 *        `kCacheLineBytes`, the stripe count is a compile-time parameter, and the default host
 *        build keeps today's 64 x 64-byte table.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The sizing is resolved from a config type, so the configurations below are checked through
 * the same resolver the build uses for its own `config_t`, without building a second library.
 * The run-time half drives the packed sizings — locks with no padding between them, and a
 * table of one — under contending threads: packing them must cost only false sharing, never
 * mutual exclusion. The CI TSan legs run this binary.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <thread>
#include <type_traits>
#include <vector>

#include "libtracer/config.hpp"
#include "libtracer/guard.hpp"
#include "libtracer/guard_mutex.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::default_config_t;
using tr::testing::check;

/** @brief The `.bss` one sizing's process-wide table costs. */
template <class G>
constexpr std::size_t table_bytes = sizeof(G) * G::kStripes;

/** @brief The guard the build would bind for config @p C. */
template <class C>
using sized_t = tr::graph::detail_guard::sized_guard<C>::type;

/** @brief A single-core target: no line to pad to, and a short table. */
struct single_core_config_t : default_config_t {
    static constexpr std::size_t kCacheLineBytes = 0;
    static constexpr std::size_t kGuardStripes = 16;
};

/** @brief A host with 128-byte lines (two adjacent 64-byte lines prefetched as a pair). */
struct wide_line_config_t : default_config_t {
    static constexpr std::size_t kCacheLineBytes = 128;
};

/** @brief A fragment that binds the default-sized host guard by name: it gets exactly that. */
struct explicit_guard_config_t : default_config_t {
    static constexpr std::size_t kCacheLineBytes = 0;
    using guard_t = tr::basic_mutex_guard_t<32, 8>;
};

// The default host build: the same type, alignment and 4 KB table as before #1716.
static_assert(std::is_same_v<sized_t<default_config_t>, tr::mutex_guard_t>);
static_assert(std::is_same_v<tr::mutex_guard_t, tr::basic_mutex_guard_t<64, 64>>);
static_assert(alignof(tr::mutex_guard_t) == 64 && sizeof(tr::mutex_guard_t) == 64);
static_assert(table_bytes<tr::mutex_guard_t> == 4096);
static_assert(tr::guard<tr::mutex_guard_t> && tr::is_mutex_guard_v<tr::mutex_guard_t>);

// A smaller configuration shrinks the table: 16 one-byte locks.
static_assert(std::is_same_v<sized_t<single_core_config_t>, tr::basic_mutex_guard_t<0, 16>>);
static_assert(alignof(sized_t<single_core_config_t>) == alignof(std::atomic<bool>));
static_assert(table_bytes<sized_t<single_core_config_t>> == 16 * sizeof(std::atomic<bool>));
static_assert(tr::guard<sized_t<single_core_config_t>>);

// Alignment follows the line size upward too.
static_assert(alignof(sized_t<wide_line_config_t>) == 128);
static_assert(table_bytes<sized_t<wide_line_config_t>> == 64 * 128);

// A guard the fragment names itself is never re-sized.
static_assert(std::is_same_v<sized_t<explicit_guard_config_t>, tr::basic_mutex_guard_t<32, 8>>);
static_assert(!tr::is_mutex_guard_v<tr::no_guard_t>);

/** @brief Every stripe of @p G is reachable, and the stripes are one contiguous table. */
template <class G>
void check_spread(const char* label) {
    std::vector<std::uint64_t> words(4096);
    std::set<const G*> seen;
    for (const auto& w : words) seen.insert(&G::for_address(&w));
    std::printf("  %s: %zu of %zu stripes reached\n", label, seen.size(), G::kStripes);
    check(seen.size() == G::kStripes, "4096 adjacent words reach every stripe");
    check(*seen.rbegin() - *seen.begin() == static_cast<std::ptrdiff_t>(G::kStripes) - 1,
          "the stripes are one table of kStripes padded locks");
}

/**
 * @brief Threads bump plain counters under @p G's stripes; every total must be exact.
 *
 * Each thread walks all the counters, so adjacent packed locks are taken concurrently by
 * different threads. A lost update means a stripe failed to exclude.
 */
template <class G>
void check_exclusion(const char* label) {
    constexpr int kThreads = 4;
    constexpr int kRounds = 20000;
    constexpr std::size_t kCounters = 32;
    std::vector<std::uint64_t> counters(kCounters, 0);
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&counters, t] {
            for (int r = 0; r < kRounds; ++r) {
                std::uint64_t& c = counters[(static_cast<std::size_t>(r) + t) % kCounters];
                const tr::guard_scope_t<G> section(&c);
                c = c + 1;
            }
        });
    }
    for (auto& t : ts) t.join();
    std::uint64_t total = 0;
    for (const std::uint64_t c : counters) total += c;
    std::printf("  %s: %llu of %d bumps\n", label, static_cast<unsigned long long>(total),
                kThreads * kRounds);
    check(total == static_cast<std::uint64_t>(kThreads) * kRounds,
          "every guarded bump counted exactly once");
}

}  // namespace

int main() {
    std::printf("host guard sizing (#1716):\n");
    check_spread<tr::mutex_guard_t>("64 x 64 B (default)");
    check_spread<tr::basic_mutex_guard_t<0, 16>>("16 x 1 B (single-core)");
    check_spread<tr::basic_mutex_guard_t<0, 1>>("1 x 1 B");
    check_exclusion<tr::mutex_guard_t>("64 x 64 B (default)");
    check_exclusion<tr::basic_mutex_guard_t<0, 16>>("16 x 1 B, packed");
    check_exclusion<tr::basic_mutex_guard_t<0, 1>>("1 x 1 B, one lock");
    return tr::testing::summary("guard_sizing");
}
