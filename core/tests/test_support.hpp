/**
 * @file
 * @brief The one runner, value builder and wait-collector the test tree shares (#874).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `core/tests/` had no shared runner at all. The `int g_failures; void check(bool, ...)`
 * micro-runner was re-declared in **97 of the 104 test translation units** — 83 of them
 * byte-identical and 10 more differing only in spelling (`const char*` instead of
 * `std::string_view`, `"ok"` instead of `"PASS"`, a 4-space indent, a `cond` parameter
 * name). The remaining **4 were not a spelling variant: they printed nothing on a pass**,
 * and that is a behavioural difference, not a cosmetic one — see @ref tr::testing::check_quiet,
 * which is what those four call. Around the runner the same three helpers had been copied
 * too: `make_value` in 25 files, a cv-guarded `mailbox_t` in 5, and a **5 ms-polling** frame
 * `sink_t` in 3 (`tcp_test.cpp`, `quic_test.cpp`, `webtransport_test.cpp`). Any improvement
 * to a helper — a file:line on a failure, a poll loop that becomes a real wait — was a
 * 97-file synchronised edit, so none was ever made.
 *
 * What the centralisation buys, beyond one definition:
 *
 * - @ref tr::testing::check names the **file and line of the failing call** (`std::source_location`
 *   defaulted at the call site). Previously a `FAIL` line carried only its own prose, and two
 *   checks in one suite are allowed to share prose.
 * - The failure counter is `std::atomic`. Half a dozen suites here call `check` from spawned
 *   threads (`graph_test`, the `*_race_test` family), where a plain `int++` is a data race the
 *   TSan job could legitimately have reported against the harness rather than the library.
 * - @ref tr::testing::frame_sink_t **waits** on a condition variable instead of sleeping 5 ms
 *   between polls, so a suite that collected N frames no longer pays up to 5 ms of dead time
 *   per frame.
 *
 * `tr::testing` is a tests-only namespace — it is not a layer in the L0..L5 model and nothing
 * under `core/src` or `core/include` may name it. The header deliberately depends on the
 * standard library ONLY, so the translation units that compile a restricted source set
 * (`substrate_test_no_atomic`, `pool_only_dispatch_test`) can include it as-is; the one helper
 * that needs a libtracer type, `make_value`, lives in the companion `test_values.hpp`.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <optional>
#include <source_location>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// The msquic-based suites cross the same non-instrumented library boundary src/msquic_endpoint.hpp
// does, and must restate its happens-before edges for TSan. No-ops outside a TSan build.
#if defined(__SANITIZE_THREAD__)
#define LIBTRACER_TEST_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LIBTRACER_TEST_TSAN 1
#endif
#endif
#ifdef LIBTRACER_TEST_TSAN
#include <sanitizer/tsan_interface.h>
#endif

namespace tr::testing {

/**
 * @brief Failed checks so far — the process exit code, once @ref summary maps it.
 *
 * Atomic because `check` is called from worker threads in the concurrency suites; relaxed
 * ordering is enough, since nothing is published through this counter.
 */
inline std::atomic<int> g_failures{0};

namespace detail {

/** @brief The trailing path component of @p path — a full build path drowns the failure prose. */
constexpr std::string_view basename(std::string_view path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/**
 * @brief Count one failure and print it with the call site @p loc names.
 *
 * The one failure path both @ref tr::testing::check and @ref tr::testing::check_quiet take,
 * so the two forms differ in exactly one thing: whether a PASS is audible.
 */
inline void fail(std::string_view what, const std::source_location& loc) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    const std::string_view file = basename(loc.file_name());
    std::printf("  [FAIL] %.*s  (%.*s:%u)\n", static_cast<int>(what.size()), what.data(),
                static_cast<int>(file.size()), file.data(), static_cast<unsigned>(loc.line()));
}

}  // namespace detail

/**
 * @brief Record one assertion: print its verdict, and count it if it failed.
 *
 * @param ok   The claim under test.
 * @param what What the claim IS, printed verbatim — the suite's own prose.
 * @param loc  Defaulted, therefore the **caller's** location; named only on a failure, where
 *             prose alone has repeatedly been ambiguous.
 */
inline void check(bool ok, std::string_view what,
                  const std::source_location loc = std::source_location::current()) {
    if (ok) {
        std::printf("  [PASS] %.*s\n", static_cast<int>(what.size()), what.data());
        return;
    }
    detail::fail(what, loc);
}

/**
 * @brief @ref check with the PASS line suppressed: same counter, same FAIL line, silent when
 *        the claim holds.
 *
 * Four suites (`try_grow_race`, `pool_only_dispatch`, `mem_sync_policy`, `mem_sync_pool`) had
 * a deliberately quiet micro-runner before #874, and they keep it. `try_grow_race` is the
 * binding case: its assertions sit next to a 300,000-iteration loop that runs concurrently
 * with a racing allocator thread, where a PASS line per iteration would be ~15 MB of output
 * on a green run — every core-ci leg runs `ctest --output-on-failure`, so the next REAL
 * failure anywhere in that suite would arrive buried under it.
 *
 * Use this only where the pass volume is genuinely unbounded. The loud @ref check stays the
 * default: 93 of the 97 migrated suites print their passes, and a suite that prints nothing
 * on a green run offers nothing to read when someone is deciding whether it ran at all.
 *
 * @param ok   The claim under test.
 * @param what What the claim IS — printed only if @p ok is false.
 * @param loc  Defaulted, therefore the **caller's** location; named on the failure line.
 */
inline void check_quiet(bool ok, std::string_view what,
                        const std::source_location loc = std::source_location::current()) {
    if (ok) return;
    detail::fail(what, loc);
}

/**
 * @brief Print the suite verdict and answer the process exit code.
 *
 * @param suite The suite's name, so a `ctest --output-on-failure` log says which harness spoke.
 * @retval 0 Every check passed.
 * @retval 1 At least one check failed.
 */
[[nodiscard]] inline int summary(std::string_view suite) {
    const int n = g_failures.load(std::memory_order_relaxed);
    std::printf("\n%.*s: %s (%d failure%s)\n", static_cast<int>(suite.size()), suite.data(),
                n == 0 ? "ALL PASS" : "FAILURES", n, n == 1 ? "" : "s");
    return n == 0 ? 0 : 1;
}

/** @brief How many checks have failed so far — for a suite that branches on it mid-run. */
[[nodiscard]] inline int failures() { return g_failures.load(std::memory_order_relaxed); }

/**
 * @brief The exit code CTest reads as SKIPPED, per the target's `SKIP_RETURN_CODE` property.
 *
 * 77 is the GNU-autotools convention CTest documents; the value matters only in that the
 * `add_test` site and the suite agree, so it is spelled once, here.
 */
inline constexpr int kSkipExitCode = 77;

/**
 * @brief Report the whole suite as SKIPPED and answer the process exit code (#1438).
 *
 * For a suite whose SUBJECT this build does not contain — a module closed out at compile
 * time (ADR-0047 §1), not a case that happens to be inconvenient. CTest's label filters are
 * the primary deselection (`ctest -LE bus`); this is what the suite does when someone runs
 * it anyway, so the answer is a stated skip instead of a crash or a wall of failures about
 * a feature the binary was never built with.
 *
 * @param suite The suite's name, as @ref summary prints it.
 * @param why   Why the subject is absent — printed, so the log says which build this is.
 * @return @ref kSkipExitCode, for `return` straight out of `main`.
 */
[[nodiscard]] inline int skipped(std::string_view suite, std::string_view why) {
    std::printf("%.*s: SKIPPED — %.*s\n", static_cast<int>(suite.size()), suite.data(),
                static_cast<int>(why.size()), why.data());
    return kSkipExitCode;
}

/** @brief Publish everything written before this point to whoever later acquires @p p. */
inline void tsan_release([[maybe_unused]] void* p) {
#ifdef LIBTRACER_TEST_TSAN
    __tsan_release(p);
#endif
}

/** @brief Take the writes published by the matching @ref tsan_release on @p p. */
inline void tsan_acquire([[maybe_unused]] void* p) {
#ifdef LIBTRACER_TEST_TSAN
    __tsan_acquire(p);
#endif
}

/**
 * @brief RAII happens-before edge for ONE library callback invocation: acquire on entry,
 *        release on exit — the `src/msquic_endpoint.hpp` guard, restated test-side.
 */
struct tsan_cb_guard_t {
    void* p; /**< @brief The object whose callbacks are serialised by the library. */
    explicit tsan_cb_guard_t(void* ptr) : p(ptr) { tsan_acquire(p); }
    ~tsan_cb_guard_t() { tsan_release(p); }
    tsan_cb_guard_t(const tsan_cb_guard_t&) = delete;
    tsan_cb_guard_t& operator=(const tsan_cb_guard_t&) = delete;
};

/**
 * @brief A queue of encoded frames filled on a delivery thread and drained by the test thread.
 *
 * The FWD suites' REPLY inbox. Both drains are **deadline** waits on a condition variable, so
 * a test that expects nothing to arrive pays exactly its budget and a test that expects an
 * arrival pays exactly the latency.
 */
struct mailbox_t {
    std::mutex m;                          /**< @brief Guards `q`. */
    std::condition_variable cv;            /**< @brief Signalled by @ref push. */
    std::vector<std::vector<std::byte>> q; /**< @brief Arrivals, oldest first. */

    /** @brief Append one encoded frame and wake every waiter. */
    void push(std::vector<std::byte> v) {
        {
            const std::lock_guard lock(m);
            q.push_back(std::move(v));
        }
        cv.notify_all();
    }

    /** @brief Pop the oldest frame, waiting up to @p budget. @retval std::nullopt Nothing came. */
    std::optional<std::vector<std::byte>> wait(std::chrono::milliseconds budget) {
        std::unique_lock lock(m);
        if (!cv.wait_for(lock, budget, [this] { return !q.empty(); })) return std::nullopt;
        std::vector<std::byte> v = std::move(q.front());
        q.erase(q.begin());
        return v;
    }

    /**
     * @brief Wait until at least @p n frames have arrived (or @p budget lapses).
     * @return The count actually held — a FLOOR check reads `>= n`, an exactness check `== n`.
     */
    std::size_t wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m);
        cv.wait_for(lock, budget, [&] { return q.size() >= n; });
        return q.size();
    }

    /** @brief How many frames are held right now. */
    std::size_t size() {
        const std::lock_guard lock(m);
        return q.size();
    }
};

/**
 * @brief A collecting frame sink: frames are delivered on a transport RX thread and read from
 *        the test thread.
 *
 * The transport suites' receiver. @ref wait_for_count replaces the hand-rolled
 * `while (count() < n) sleep_for(5ms)` the three copies shared: same signature, same `bool`,
 * but it wakes on the delivery rather than on the next 5 ms tick.
 */
struct frame_sink_t {
    std::mutex m;                               /**< @brief Guards `frames`. */
    std::condition_variable cv;                 /**< @brief Signalled by @ref push. */
    std::vector<std::vector<std::byte>> frames; /**< @brief Arrivals, oldest first. */

    /** @brief Take a copy of one delivered frame and wake every waiter. */
    void push(std::span<const std::byte> f) {
        {
            const std::lock_guard lock(m);
            frames.emplace_back(f.begin(), f.end());
        }
        cv.notify_all();
    }

    /** @brief How many frames have been delivered so far. */
    [[nodiscard]] std::size_t count() {
        const std::lock_guard lock(m);
        return frames.size();
    }

    /** @brief A copy of frame @p i. Throws `std::out_of_range` if it never arrived. */
    [[nodiscard]] std::vector<std::byte> at(std::size_t i) {
        const std::lock_guard lock(m);
        return frames.at(i);
    }

    /** @brief Wait until at least @p n frames have arrived. @retval false The budget lapsed. */
    [[nodiscard]] bool wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, budget, [&] { return frames.size() >= n; });
    }
};

}  // namespace tr::testing
