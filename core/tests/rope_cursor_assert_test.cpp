/**
 * @file
 * @brief `rope_cursor` bounds preconditions (#916) — the debug-assert parity death tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `rope_cursor` used to hide an out-of-range read instead of failing on it: `region`
 * clamped nothing, so a cursor could claim bytes the chain does not hold, and `locate`
 * answered any at/past-end offset with `{last_link, 0}` — so `byte_at` returned byte 0
 * of the last link, a REAL but WRONG byte that no sanitizer could see (unlike the
 * sibling `span_cursor`, whose out-of-range read is span UB that ASan/fuzz CI catches).
 * On an empty chain it was hard UB.
 *
 * `for_each_span`, the one BULK reader, had the same hole and none of the backstop: its
 * only guards are chain-end and `locate`'s past-chain assert, neither of which sees a feed
 * that overshoots a NARROWED window while staying inside the chain, so it fed the caller
 * real-but-wrong bytes and reported success. Unlike `byte_at` there is no release fallback
 * either — the overshot bytes really are in the chain, so a release build's violation is
 * silent AND unsanitizable. Its precondition is therefore the only guard there is.
 *
 * The fix is the discipline `view_t::subview` already has: debug-only preconditions,
 * zero release cost. This file is the gate for them. It is compiled with `NDEBUG`
 * forced OFF (the repo's default test build is RelWithDebInfo, which defines it), and
 * each precondition is exercised in a forked child so the abort is observed rather than
 * suffered — a child that exits without SIGABRT means the assert did not fire.
 *
 * The release guarantee (an `optional`/poisoned-flag `byte_at` the grammar maps to
 * FRAME_TRUNCATED) is deliberately NOT in scope here — it is a separate signature and
 * error-threading decision, deferred by the #916 brief.
 */

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cassert>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

#include "libtracer/mem_borrowed.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/rope_decode.hpp"
#include "libtracer/view.hpp"

namespace {

int g_failures = 0;

/** @brief Record one check result. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::grammar::rope_cursor;

/** @brief A borrowed view over @p bytes (the caller's storage must outlive it). */
view_t borrowed_view(std::span<std::byte> bytes) { return view_t::over(tr::view::borrow(bytes)); }

/** @brief Child exit code meaning "the assert fired" (SIGABRT was caught). */
constexpr int kAborted = 91;
/** @brief Child exit code meaning "the assert did NOT fire" — the redden signal. */
constexpr int kSurvived = 0;

/** @brief Child SIGABRT handler: report the abort by exit code, never by a core dump. */
extern "C" void on_abort(int) { ::_exit(kAborted); }

/**
 * @brief Run @p fn in a forked child; true iff the child's assert fired.
 *
 * A precondition that no longer aborts lets the child run to `_exit(kSurvived)`, which
 * reports false — this is what reddens when the assert is removed. The child catches
 * SIGABRT rather than dying on it: a piped `kernel.core_pattern` ignores `RLIMIT_CORE`,
 * so an uncaught abort would hand each case to the host's crash reporter (≈1 s apiece).
 * Its stdio is dropped so the assert banner does not pollute the ctest log.
 */
template <class Fn>
bool aborts(Fn&& fn) {
    std::fflush(nullptr);
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::signal(SIGABRT, on_abort);
        const int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
        }
        fn();
        ::_exit(kSurvived);  // reached only when the precondition did NOT fire
    }
    if (pid < 0) return false;
    int status = 0;
    if (::waitpid(pid, &status, 0) != pid) return false;
    if (WIFSIGNALED(status)) return WTERMSIG(status) == SIGABRT;  // handler bypassed
    return WIFEXITED(status) && WEXITSTATUS(status) == kAborted;
}

/** @brief Keep the compiler from eliding a read whose only purpose is to trip an assert. */
volatile std::uint8_t g_sink = 0;

/** @brief The in-bounds contract still holds — the asserts do not fire on valid reads. */
void test_in_bounds_unaffected() {
    std::printf("rope_cursor in-bounds reads are unaffected (#916):\n");
    std::array<std::byte, 3> a{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}};
    std::array<std::byte, 2> b{std::byte{0x20}, std::byte{0x21}};

    rope_t r(borrowed_view(a));
    r.append(borrowed_view(b));
    const rope_cursor cur{r};
    check(cur.size() == 5, "cursor spans the whole chain");
    check(cur.byte_at(0) == 0x10 && cur.byte_at(2) == 0x12, "byte_at reads inside the first link");
    check(cur.byte_at(3) == 0x20 && cur.byte_at(4) == 0x21, "byte_at walks into the second link");

    const rope_cursor sub = cur.region(2, 3);
    check(sub.size() == 3 && sub.byte_at(0) == 0x12 && sub.byte_at(2) == 0x21,
          "an exactly-fitting region straddles the link boundary");
    check(cur.region(5, 0).size() == 0, "an empty region at the very end is legal");

    std::size_t spans = 0;
    std::size_t bytes = 0;
    cur.for_each_span(0, 5, [&](std::span<const std::byte> s) {
        ++spans;
        bytes += s.size();
    });
    check(spans == 2 && bytes == 5, "for_each_span yields one sub-span per straddled link");

    std::size_t empty_spans = 0;
    cur.for_each_span(5, 0, [&](std::span<const std::byte>) { ++empty_spans; });
    check(empty_spans == 0, "a zero-length feed at the window end yields nothing and is legal");

    // An exactly-fitting feed on a NARROWED window is the case the containment
    // precondition must NOT over-fire on: off + n == size() is in contract.
    std::size_t narrow_bytes = 0;
    cur.region(0, 3).for_each_span(0, 3,
                                   [&](std::span<const std::byte> s) { narrow_bytes += s.size(); });
    check(narrow_bytes == 3, "a feed that exactly fills a narrowed window is legal");
}

/** @brief Each out-of-range access trips its debug precondition (the #916 gate). */
void test_out_of_range_aborts() {
    std::printf("rope_cursor out-of-range access aborts instead of fabricating a byte (#916):\n");
    std::array<std::byte, 3> a{std::byte{0x10}, std::byte{0x11}, std::byte{0x12}};
    std::array<std::byte, 2> b{std::byte{0x20}, std::byte{0x21}};

    check(aborts([&] {
              rope_t r(borrowed_view(a));
              r.append(borrowed_view(b));
              const rope_cursor cur{r};
              g_sink = static_cast<std::uint8_t>(cur.region(3, 3).size());
          }),
          "region(off, len) past the parent window aborts (was: an unclamped end_)");

    check(aborts([&] {
              rope_t r(borrowed_view(a));
              r.append(borrowed_view(b));
              const rope_cursor cur{r};
              g_sink = static_cast<std::uint8_t>(cur.region(6, 0).size());
          }),
          "region whose OFFSET alone is past the window aborts");

    check(aborts([&] {
              rope_t r(borrowed_view(a));
              r.append(borrowed_view(b));
              const rope_cursor cur{r};
              g_sink = cur.byte_at(5);
          }),
          "byte_at at the end aborts (was: byte 0 of the last link — a real, WRONG byte)");

    check(aborts([&] {
              rope_t r(borrowed_view(a));
              r.append(borrowed_view(b));
              const rope_cursor sub = rope_cursor{r}.region(1, 2);
              g_sink = sub.byte_at(2);
          }),
          "byte_at past a NARROWED window aborts even though the chain holds that byte");

    check(aborts([&] {
              const rope_t empty;
              const rope_cursor cur{empty};
              g_sink = cur.byte_at(0);
          }),
          "byte_at on an EMPTY chain aborts (was: links_[0] on an empty span — hard UB)");

    check(aborts([&] {
              rope_t r(borrowed_view(a));
              r.append(borrowed_view(b));
              const rope_cursor cur{r};
              cur.for_each_span(5, 1, [&](std::span<const std::byte> s) {
                  g_sink = static_cast<std::uint8_t>(s.size());
              });
          }),
          "a non-empty feed starting past the end aborts in locate (was: the fabricated "
          "{last, 0})");

    // The one the chain-end and past-chain-locate guards both miss: the feed STARTS
    // in-chain, so `locate` is happy, and it ends in-chain, so `li < links_.size()`
    // never trips — the overshoot is only past the cursor's NARROWED window. Without
    // the containment precondition the walk hands bytes 3 and 4 to `fn` and reports
    // success, while the identical slip through `byte_at(3)` on this cursor aborts.
    check(aborts([&] {
              rope_t r(borrowed_view(a));
              r.append(borrowed_view(b));
              const rope_cursor cur = rope_cursor{r}.region(0, 3);
              cur.for_each_span(0, 5, [&](std::span<const std::byte> s) {
                  g_sink = static_cast<std::uint8_t>(s.size());
              });
          }),
          "a feed past a NARROWED window aborts even though every byte it walks is in "
          "the chain");
}

}  // namespace

int main() {
    test_in_bounds_unaffected();
    test_out_of_range_aborts();
    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
