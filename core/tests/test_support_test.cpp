/**
 * @file
 * @brief The shared test runner, under test by itself (#874).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One header now decides the verdict of every suite in `core/tests/`. That is the point of
 * #874 and also its one new failure mode: if @ref tr::testing::check ever stopped counting, or
 * @ref tr::testing::summary ever stopped mapping a nonzero count to exit 1, the ENTIRE suite
 * would go green at once and look like a success. Nothing else in the tree can catch that —
 * every other harness reports through the very code that would be broken.
 *
 * @par Why this ONE file keeps a private runner
 * #874 deletes 97 hand-rolled `check`/`g_failures` pairs. This file re-introduces exactly one,
 * @ref must, and it has to: a harness cannot use the thing it is testing as its own judge. That
 * is not a stylistic point — it was MEASURED. An earlier draft of this file reported through
 * `tr::testing::check`, and against a mutant whose `check` did not increment the counter it
 * printed `[FAIL] check(false) increments the failure counter by exactly one` and then
 * **exited 0**, because the exit code came from the counter the mutant had broken. The probe
 * saw the defect and the process still said PASS. @ref must gives this file a verdict channel
 * the subject cannot corrupt; `main` fails if EITHER channel is unhappy.
 *
 * The properties asserted:
 *
 *   1. a passing check does NOT move the counter, and a failing one moves it by exactly one;
 *   1b. @ref tr::testing::check_quiet prints NOTHING on a pass, and still counts a failure and
 *      still names its call site — a quiet form that swallowed failures would be far worse than
 *      the PASS noise it exists to avoid;
 *   2. `summary` answers 0 for a zero count and 1 for a nonzero one;
 *   3. a FAIL line names the CALLER's file and line, not the runner's;
 *   4. the shared collectors block for a real arrival and give up on one that never comes —
 *      a @ref tr::testing::mailbox_t or @ref tr::testing::frame_sink_t whose wait always
 *      answered "yes" would silently un-gate the transport and FWD suites;
 *   5. `make_value` owns a COPY of its input bytes.
 *
 * (3) is checked by capturing stdout, because that string is the whole value of the
 * centralisation: a suite with two checks carrying the same prose used to be unresolvable.
 *
 * The probes deliberately call `check(false, …)` and then RESTORE the shared counter, so this
 * harness's own verdict stays honest. The `(probe)` lines in its output are expected.
 */
#include "test_support.hpp"

#include <unistd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_values.hpp"

namespace {

using namespace std::chrono_literals;
using tr::testing::make_value;

/** @brief This file's INDEPENDENT failure count — see the file header on why it exists. */
int g_local_failures = 0;

/**
 * @brief Record one claim about the shared runner, through a channel the shared runner cannot
 *        break.
 *
 * Deliberately NOT `tr::testing::check`: everything asserted here is a property OF that
 * function, so routing the verdict through it would make a broken runner report itself green.
 */
void must(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_local_failures;
}

/** @brief Run @p fn with `stdout` redirected, and answer everything it printed. */
template <typename fn_t>
std::string capture_stdout(fn_t&& fn) {
    char path[] = "/tmp/libtracer_test_support_XXXXXX";
    const int tmp = ::mkstemp(path);
    if (tmp < 0) return {};
    const int saved = ::dup(STDOUT_FILENO);
    std::fflush(stdout);
    ::dup2(tmp, STDOUT_FILENO);

    fn();

    std::fflush(stdout);
    ::dup2(saved, STDOUT_FILENO);
    ::close(saved);
    ::lseek(tmp, 0, SEEK_SET);
    std::string out;
    char buf[512];
    for (;;) {
        const ssize_t n = ::read(tmp, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(tmp);
    ::unlink(path);
    return out;
}

/** @brief Property 1 — a pass is free, a failure costs exactly one. */
void counting() {
    const int before = tr::testing::failures();
    const std::string passed =
        capture_stdout([] { tr::testing::check(true, "(probe) a passing claim"); });
    must(tr::testing::failures() == before, "check(true) does not move the failure counter");
    must(passed.find("[PASS]") != std::string::npos, "a passing check prints a PASS line");

    const std::string failed =
        capture_stdout([] { tr::testing::check(false, "(probe) a deliberately false claim"); });
    const bool counted = tr::testing::failures() == before + 1;
    tr::testing::g_failures.store(before, std::memory_order_relaxed);  // undo the probe
    must(counted, "check(false) increments the failure counter by exactly one");
    must(failed.find("[FAIL]") != std::string::npos, "a failing check prints a FAIL line");
}

/**
 * @brief Property 1b — the quiet form is silent on a pass and LOUD on a failure.
 *
 * The four suites that stayed quiet-on-pass through #874 (`try_grow_race` above all, whose
 * assertions sit beside a 300k-iteration racing loop) report only through this function. If it
 * ever stopped counting, or stopped printing, those four would go green silently.
 */
void quiet_form() {
    const int before = tr::testing::failures();
    const std::string passed =
        capture_stdout([] { tr::testing::check_quiet(true, "(probe) a passing quiet claim"); });
    must(passed.empty(), "check_quiet(true) prints NOTHING — the regression #874 nearly shipped");
    must(tr::testing::failures() == before, "check_quiet(true) does not move the failure counter");

    // Same adjacency contract as failure_names_its_call_site(): `line` is the probe's line.
    const unsigned line = static_cast<unsigned>(__LINE__) + 2;
    const std::string failed = capture_stdout(
        [] { tr::testing::check_quiet(false, "(probe) a deliberately false quiet claim"); });
    const bool counted = tr::testing::failures() == before + 1;
    tr::testing::g_failures.store(before, std::memory_order_relaxed);  // undo the probe
    must(counted, "check_quiet(false) increments the failure counter by exactly one");
    must(failed.find("[FAIL]") != std::string::npos, "a failing check_quiet prints a FAIL line");
    must(failed.find("test_support_test.cpp:" + std::to_string(line)) != std::string::npos,
         "and that FAIL line names the caller's file:line, exactly as the loud form does");
}

/** @brief Property 3 — the FAIL line carries the CALLER's file:line. */
void failure_names_its_call_site() {
    const int before = tr::testing::failures();
    // These two lines are load-bearing and must stay adjacent: `line` is the line the probe's
    // `check` sits on, and the whole claim is that the runner prints THAT number, not one of
    // its own.
    const unsigned line = static_cast<unsigned>(__LINE__) + 1;
    const std::string out = capture_stdout([] { tr::testing::check(false, "(probe) locate me"); });
    tr::testing::g_failures.store(before, std::memory_order_relaxed);  // undo the probe

    must(out.find("test_support_test.cpp") != std::string::npos,
         "a FAIL line names the file the failing check was written in");
    const std::string want = "test_support_test.cpp:" + std::to_string(line);
    must(out.find(want) != std::string::npos,
         "a FAIL line names the LINE the failing check was written on");
    must(out.find("test_support.hpp") == std::string::npos,
         "and not the runner's own file — std::source_location is defaulted at the call site");
}

/** @brief Property 2 — the exit code is the count, collapsed. */
void summary_maps_the_count() {
    const int before = tr::testing::failures();

    tr::testing::g_failures.store(0, std::memory_order_relaxed);
    int clean = 1;
    const std::string clean_text =
        capture_stdout([&clean] { clean = tr::testing::summary("(probe) clean"); });
    tr::testing::g_failures.store(3, std::memory_order_relaxed);
    int dirty = 0;
    const std::string dirty_text =
        capture_stdout([&dirty] { dirty = tr::testing::summary("(probe) dirty"); });

    tr::testing::g_failures.store(before, std::memory_order_relaxed);
    must(clean == 0, "summary() answers 0 when nothing failed");
    must(dirty == 1, "summary() answers 1 when something failed — the suite's exit code");
    must(clean_text.find("ALL PASS") != std::string::npos, "a clean summary says ALL PASS");
    must(dirty_text.find("FAILURES") != std::string::npos,
         "and a dirty one says FAILURES, naming the count");
}

/** @brief Property 4a — the REPLY inbox blocks for a real arrival, and gives up otherwise. */
void mailbox_waits() {
    tr::testing::mailbox_t box;
    must(!box.wait(50ms).has_value(), "mailbox_t::wait answers nullopt when nothing is pushed");
    must(box.wait_for_count(1, 50ms) == 0, "and wait_for_count answers the count it actually has");

    std::thread producer([&box] {
        std::this_thread::sleep_for(20ms);
        box.push({std::byte{0xAB}, std::byte{0xCD}});
    });
    const auto got = box.wait(2000ms);
    producer.join();
    must(got.has_value(), "mailbox_t::wait wakes on a push from another thread");
    must(got && got->size() == 2 && (*got)[0] == std::byte{0xAB} && (*got)[1] == std::byte{0xCD},
         "and hands back the bytes that were pushed, unaltered");
    must(box.size() == 0, "the popped frame left the queue");
}

/** @brief Property 4b — the transport collector blocks for real deliveries, and gives up otherwise.
 */
void frame_sink_waits() {
    tr::testing::frame_sink_t sink;
    must(!sink.wait_for_count(1, 50ms),
         "frame_sink_t::wait_for_count answers false when nothing is delivered");
    must(sink.count() == 0, "and nothing was collected");

    const std::vector<std::byte> f{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
    std::thread producer([&sink, &f] {
        std::this_thread::sleep_for(20ms);
        sink.push(f);
        sink.push(f);
    });
    const bool arrived = sink.wait_for_count(2, 2000ms);
    producer.join();
    must(arrived, "frame_sink_t::wait_for_count wakes on deliveries from another thread");
    must(sink.count() == 2, "both deliveries were collected");
    must(sink.at(0) == f && sink.at(1) == f, "and each is a byte-exact copy of what was pushed");
}

/** @brief Property 5 — `make_value` owns a COPY; the source buffer may die immediately after. */
void make_value_copies() {
    tr::view::view_t v;
    {
        std::vector<std::byte> src{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
        v = make_value(src);
        src.assign(3, std::byte{0xFF});  // scribble over the source
    }
    must(v.bytes().size() == 3, "make_value keeps the length");
    must(v.bytes()[0] == std::byte{0x10} && v.bytes()[1] == std::byte{0x20} &&
             v.bytes()[2] == std::byte{0x30},
         "make_value copies the bytes — a later write to the source cannot reach the view");

    const tr::view::view_t braced = make_value({0x41, 0x42});
    must(braced.bytes().size() == 2 && braced.bytes()[0] == std::byte{0x41} &&
             braced.bytes()[1] == std::byte{0x42},
         "the braced-list overload builds the same bytes");

    const tr::view::view_t empty = make_value(std::span<const std::byte>{});
    must(empty.bytes().empty(), "an empty value is empty");
    must(static_cast<bool>(empty.owner),
         "but still OWNS a segment — the distinction production's over_bytes deliberately drops");
}

}  // namespace

int main() {
    std::printf("test_support: the runner is the SUBJECT here, so these checks report through a\n");
    std::printf("              private counter; the `(probe)` lines below are expected.\n");
    counting();
    quiet_form();
    failure_names_its_call_site();
    summary_maps_the_count();
    mailbox_waits();
    frame_sink_waits();
    make_value_copies();

    // Both channels must be clean. The shared one alone is not sufficient (a runner that does
    // not count reports 0 no matter what happened); the local one alone would not notice a
    // probe that left the shared counter dirty.
    const int shared = tr::testing::summary("test_support");
    std::printf("test_support: %d independent failure%s\n", g_local_failures,
                g_local_failures == 1 ? "" : "s");
    return (shared != 0 || g_local_failures != 0) ? 1 : 0;
}
