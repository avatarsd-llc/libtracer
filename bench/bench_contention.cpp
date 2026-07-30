/**
 * @file
 * @brief What one shared cache line costs, isolated from libtracer entirely.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every other bench here measures libtracer. This one measures the **machine**, so that a
 * claim like "the read path is limited by one shared reader-writer lock" can be checked
 * against what such a lock actually costs on the box in front of you, rather than against a
 * number someone once quoted. It exists because the concurrency documentation
 * (`docs/design/concurrency/`) was first written on two throwaway harnesses that were never
 * committed — which made its two load-bearing constants unreproducible by anyone, including
 * their author.
 *
 * Nine arms, chosen so that each isolates exactly one thing and the differences between
 * adjacent arms are interpretable:
 *
 *   - `local` — bump a thread-private counter. The loop itself, and the scaling ceiling.
 *   - `shared-read` — read one shared, never-written word. Read-only sharing, which is free.
 *   - `rmw1` / `rmw2` / `rmw4` — one, two or four `fetch_add`s on ONE shared atomic. One
 *     contended line transfer, and whether transfers are additive.
 *   - `rwlock` — `shared_lock` and unlock on one `shared_mutex`: what `graph_t::read`'s
 *     process-wide map lock costs.
 *   - `mutex` — lock and unlock on one `std::mutex`: a fully exclusive lock.
 *   - `sp-load` — `std::atomic<std::shared_ptr<T>>::load()`: the default LKV slot's primitive.
 *   - `sp-copy` — copy an already-held `shared_ptr`: the promotion an owning read cannot skip.
 *
 * `rmw1` against `mutex` is the one that answers "why does a lock go retrograde when a bare
 * atomic does not": a contended RMW costs one line transfer per operation no matter how many
 * threads want it, while a lock costs one per *contender* per successful acquisition, because
 * the losers' failed attempts are transfers too.
 *
 * RESULT lines carry `fanout` = thread count and the three latency fields = **picoseconds**
 * per system-wide operation (the reciprocal of the aggregate rate). Picoseconds because the
 * uncontended arms run near 1 ns and those fields are integers.
 *
 * Diagnostic, never a perf gate — these numbers are properties of the host, and the whole
 * point is that they differ per machine. Run it on yours before believing a table.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "bench_common.hpp"

namespace {

using bench::emit;
using bench::now_ns;

/** @brief Operations each thread performs per arm — enough to swamp thread start-up. */
constexpr std::size_t kOpsPerThread = 400'000;

/** @brief Thread counts swept, clamped to the host's hardware concurrency. */
constexpr std::size_t kThreads[] = {1, 2, 4, 8, 16, 24};

/** @brief The one shared line every contended arm fights over. */
struct alignas(64) shared_line_t {
    std::atomic<std::uint64_t> counter{0};
};

/** @brief A thread's private accumulator, isolated so `local` measures no sharing at all. */
struct alignas(64) private_line_t {
    std::uint64_t value = 0;
};

shared_line_t g_shared;                                 /**< @brief The contended counter. */
const std::uint64_t g_readonly = 0x5bd1e5;              /**< @brief Never written; read by all. */
std::shared_mutex g_rw;                                 /**< @brief Stands in for `map_mutex_`. */
std::mutex g_mu;                                        /**< @brief A fully exclusive lock. */
std::atomic<std::shared_ptr<const std::uint64_t>> g_sp; /**< @brief The default slot's shape. */
std::shared_ptr<const std::uint64_t> g_plain;           /**< @brief Copied by `sp-copy`. */
std::atomic<std::uint64_t> g_sink{0};                   /**< @brief Keeps the work observable. */

/** @brief Which primitive an arm exercises. */
enum class arm_t { LOCAL, SHARED_READ, RMW1, RMW2, RMW4, RWLOCK, MUTEX, SP_LOAD, SP_COPY };

/** @brief The RESULT `mode` string for @p a. */
[[nodiscard]] const char* arm_name(arm_t a) {
    switch (a) {
        case arm_t::LOCAL:
            return "local";
        case arm_t::SHARED_READ:
            return "shared-read";
        case arm_t::RMW1:
            return "rmw1";
        case arm_t::RMW2:
            return "rmw2";
        case arm_t::RMW4:
            return "rmw4";
        case arm_t::RWLOCK:
            return "rwlock";
        case arm_t::MUTEX:
            return "mutex";
        case arm_t::SP_LOAD:
            return "sp-load";
        case arm_t::SP_COPY:
            return "sp-copy";
    }
    return "?";
}

/**
 * @brief One operation of arm @p a, accumulating into @p acc so nothing can be elided.
 *
 * Deliberately one function with a switch hoisted out of the timed loop by the caller's
 * template-free structure: the branch is perfectly predicted and costs the same in every arm,
 * so it cancels out of every comparison between arms.
 */
inline void step(arm_t a, std::uint64_t& acc) {
    switch (a) {
        case arm_t::LOCAL:
            acc += 1;
            break;
        case arm_t::SHARED_READ:
            acc += g_readonly;
            break;
        case arm_t::RMW1:
            acc += g_shared.counter.fetch_add(1, std::memory_order_relaxed);
            break;
        case arm_t::RMW2:
            acc += g_shared.counter.fetch_add(1, std::memory_order_relaxed);
            acc += g_shared.counter.fetch_add(1, std::memory_order_relaxed);
            break;
        case arm_t::RMW4:
            for (int i = 0; i < 4; ++i) {
                acc += g_shared.counter.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case arm_t::RWLOCK: {
            const std::shared_lock lock(g_rw);
            acc += g_readonly;
            break;
        }
        case arm_t::MUTEX: {
            const std::lock_guard lock(g_mu);
            acc += g_readonly;
            break;
        }
        case arm_t::SP_LOAD: {
            const std::shared_ptr<const std::uint64_t> sp = g_sp.load();
            acc += *sp;
            break;
        }
        case arm_t::SP_COPY: {
            const std::shared_ptr<const std::uint64_t> sp = g_plain;  // refcount ++ then --
            acc += *sp;
            break;
        }
    }
}

/**
 * @brief Run one arm at @p threads threads and emit a RESULT line.
 *
 * Threads spin on a start flag rather than sleeping, so the measured window contains no
 * wake-up latency; the wall clock starts once every thread has announced itself ready.
 */
void run(arm_t a, std::size_t threads) {
    std::atomic<std::size_t> ready{0};
    std::atomic<bool> go{false};
    std::vector<private_line_t> acc(threads);
    std::vector<std::thread> pool;
    pool.reserve(threads);

    for (std::size_t t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) { /* spin to a common start */
            }
            std::uint64_t mine = 0;
            for (std::size_t i = 0; i < kOpsPerThread; ++i) step(a, mine);
            acc[t].value = mine;
        });
    }
    while (ready.load(std::memory_order_acquire) < threads) { /* wait for the field */
    }
    const std::uint64_t t0 = now_ns();
    go.store(true, std::memory_order_release);
    for (auto& th : pool) th.join();
    const std::uint64_t elapsed = now_ns() - t0;

    for (const private_line_t& p : acc) g_sink.fetch_add(p.value, std::memory_order_relaxed);

    const double ops = static_cast<double>(kOpsPerThread) * static_cast<double>(threads);
    const double per_s = ops * 1e9 / static_cast<double>(elapsed);
    // PICOseconds per completed op SYSTEM-WIDE (not per thread): the reciprocal of the
    // aggregate rate, which is the number that stays flat when a primitive is a fixed-cost
    // serializer. Picoseconds because the uncontended arms run near 1 ns and the RESULT
    // latency fields are integers — in nanoseconds the interesting arms would all read 0.
    const std::uint64_t ps_per_op = static_cast<std::uint64_t>(1e12 / per_s);
    emit("machine", arm_name(a), /*size_bytes=*/8, /*fanout=*/threads, /*endpoints=*/1,
         per_s / static_cast<double>(threads), per_s, 0.0, {ps_per_op, ps_per_op, ps_per_op});
}

}  // namespace

/** @brief Sweep every arm across every admissible thread count. */
int main() {
    g_sp.store(std::make_shared<const std::uint64_t>(7));
    g_plain = std::make_shared<const std::uint64_t>(11);

    const std::size_t hw = std::max<std::size_t>(1, std::thread::hardware_concurrency());
    for (arm_t a : {arm_t::LOCAL, arm_t::SHARED_READ, arm_t::RMW1, arm_t::RMW2, arm_t::RMW4,
                    arm_t::RWLOCK, arm_t::MUTEX, arm_t::SP_LOAD, arm_t::SP_COPY}) {
        for (std::size_t t : kThreads) {
            if (t > hw) continue;
            run(a, t);
        }
    }
    return 0;
}
