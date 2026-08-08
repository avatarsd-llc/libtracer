/**
 * @file
 * @brief Self-test for the composition bench's RECEIVER-side guards — the per-point nonce and
 *        the sample floor (`bench_compose.hpp`).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Both guards exist because a claim about them was made before they did. The framing comment
 * asserted that "a stray datagram from another run on a reused port is counted as malformed",
 * but the only checks were a compile-time `magic` and the swept `width`, so a CONCURRENT run of
 * this same harness at the same K passed both and was folded into the rate. And the driver spent
 * a fixed VALUE budget per point, so the datagram count fell by K and the window was thinnest at
 * the largest K — precisely where "per-value cost stays flat" is read off.
 *
 * So the tests below are written from the failing direction first: a foreign run id must be
 * REJECTED and must make the point unreportable, and a window under the floor must emit no
 * `RESULT` row. Each negative case has a positive twin that differs in exactly the field under
 * test, so a guard that stopped discriminating would fail rather than pass twice.
 *
 * No network, no subprocess, no timing: this drives `sub_counter_t` with bytes built in-process.
 *
 *     ./build/test_compose_record       # exit 0 = every expectation held
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>
#include <vector>

#include "bench_compose.hpp"

using bench::compose::phase_t;
using bench::compose::sub_counter_t;

namespace {

/** @brief Expectations that did not hold; the process exit status. */
int g_failures = 0;

/** @brief Record one expectation. */
void check(bool ok, const char* what) {
    if (ok) {
        std::printf("ok    %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

/** @brief One datagram: the K records of a group, back to back, as the wire carries them. */
[[nodiscard]] std::vector<std::byte> datagram(std::size_t k, phase_t phase, std::uint32_t run_id) {
    const std::vector<std::vector<std::byte>> recs =
        bench::compose::make_group(k, 64, phase, run_id);
    std::vector<std::byte> out;
    for (const std::vector<std::byte>& r : recs) out.insert(out.end(), r.begin(), r.end());
    return out;
}

/** @brief Feed @p n throughput datagrams of width @p k stamped with @p run_id. */
void feed(sub_counter_t& c, std::size_t k, std::uint32_t run_id, int n) {
    const std::vector<std::byte> d = datagram(k, phase_t::THROUGHPUT, run_id);
    for (int i = 0; i < n; ++i) (void)c.on_message(std::span<const std::byte>(d));
}

/**
 * @brief Feed @p n datagrams with a real gap in the middle.
 *
 * `finish()` also requires a throughput window of positive DURATION, and the window here is
 * built from `now_ns()` at each delivery. A back-to-back loop spans a few hundred nanoseconds,
 * which is fine on a vDSO clock and not guaranteed anywhere else; the sleep makes the span
 * positive on any clock granularity rather than on this host's. It is not a measurement — no
 * duration is read out of these tests.
 */
void feed_over_time(sub_counter_t& c, std::size_t k, std::uint32_t run_id, int n) {
    feed(c, k, run_id, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    feed(c, k, run_id, n - 1);
}

/** @brief A counter for width @p k expecting @p run_id, with sample floor @p floor. */
[[nodiscard]] sub_counter_t counter(std::size_t k, std::uint32_t run_id, std::uint64_t floor) {
    return sub_counter_t("selftest", "compose-udp", k, 64, run_id, floor);
}

/** @brief The positive twin: this harness's own datagrams are counted. */
void accepts_its_own_run() {
    sub_counter_t c = counter(4, 7, 0);
    feed(c, 4, 7, 1);
    check(c.values() == 4, "a K=4 datagram carrying the point's run id counts 4 values");
    check(c.bad() == 0, "...and nothing is malformed");
    check(c.thru_messages() == 1, "...and it is ONE message, not four");
}

/**
 * @brief THE HOLE THIS CLOSES: same harness, same width, different run.
 *
 * `magic` is a constant and `width` is the swept parameter, so before the nonce these bytes
 * were indistinguishable from the point's own and were added to its rate.
 */
void rejects_a_concurrent_run_at_the_same_width() {
    sub_counter_t c = counter(4, 7, 0);
    feed(c, 4, 8, 1);
    check(c.values() == 0, "a datagram from another run at the SAME K contributes no value");
    check(c.bad() == 4, "...its records are counted malformed");
    check(c.finish() != 0, "...and the point is refused, so no RESULT row carries it");
}

/** @brief The width check still discriminates too — it was never the broken half. */
void rejects_another_width() {
    sub_counter_t c = counter(4, 7, 0);
    feed(c, 8, 7, 1);
    check(c.values() == 0, "a K=8 datagram contributes nothing to the K=4 point");
    check(c.bad() > 0, "...and is counted malformed");
}

/**
 * @brief The sample floor, both directions, on windows differing only in datagram count.
 *
 * The control run with the floor disabled is what makes the refusal attributable: the same
 * 8-message window publishes when nothing is demanded of it.
 */
void refuses_a_window_below_the_sample_floor() {
    sub_counter_t small = counter(4, 7, 64);
    feed_over_time(small, 4, 7, 8);
    check(small.thru_messages() == 8, "the thin window really did observe its 8 datagrams");
    check(small.finish() != 0, "a window of 8 datagrams under a floor of 64 emits no RESULT row");

    sub_counter_t control = counter(4, 7, 0);
    feed_over_time(control, 4, 7, 8);
    check(control.finish() == 0, "...and the SAME window publishes with the floor disabled");

    sub_counter_t big = counter(4, 7, 8);
    feed_over_time(big, 4, 7, 64);
    check(big.finish() == 0, "a window of 64 datagrams over a floor of 8 publishes");
}

/** @brief values/message is the whole measurement, so a datagram must never count K times. */
void one_datagram_is_one_message_however_many_records() {
    sub_counter_t c = counter(16, 7, 0);
    feed(c, 16, 7, 10);
    check(c.thru_messages() == 10, "10 K=16 datagrams are 10 messages");
    check(c.values() == 160, "...carrying 160 values");
}

/** @brief A value size that cannot hold the header is refused up front, by name. */
void refuses_a_value_size_below_the_header() {
    check(!bench::compose::value_bytes_ok(bench::compose::kRecordHeader - 1),
          "a value smaller than the record header is refused");
    check(bench::compose::value_bytes_ok(bench::compose::kRecordHeader),
          "...and exactly the header size is accepted");
}

}  // namespace

int main() {
    accepts_its_own_run();
    rejects_a_concurrent_run_at_the_same_width();
    rejects_another_width();
    refuses_a_window_below_the_sample_floor();
    one_datagram_is_one_message_however_many_records();
    refuses_a_value_size_below_the_header();
    std::printf("%s\n",
                g_failures == 0 ? "test_compose_record: PASS" : "test_compose_record: FAIL");
    return g_failures == 0 ? 0 : 1;
}
