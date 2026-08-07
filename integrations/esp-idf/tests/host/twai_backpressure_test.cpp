/**
 * @file
 * @brief #962 — the TWAI TX backpressure window must be spent PER FRAME, and
 *        teardown must not queue behind the frames spending it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the four `httpd_ws_link` suites: the REAL chip translation
 * unit (`integrations/esp-idf/libtracer/twai_link.cpp`) compiled against a host
 * fake of the facilities it rests on — `esp_driver_twai`'s node API, FreeRTOS's
 * counting semaphore and queue, and `esp_pthread`'s stack-sizing seam
 * (fake_twai.hpp). This is the first suite to compile that TU anywhere; before it
 * the CAN plane was host-tested only above the `can_link_t` seam
 * (core/tests/transport_can_test.cpp, over a fake link), so nothing exercised the
 * link's own FULL policy.
 *
 * The bus state every case here stages is the one that makes the defect bite: a
 * controller whose tx-done never fires. The fake models that by simply not
 * completing a transmit unless a test asks it to, so the link's free-slot
 * semaphore is never refilled and every writer past the pool depth parks for its
 * full window.
 *
 * The defect: `write_raw` took `write_m_` and only then parked on that semaphore,
 * so the window was per QUEUE rather than per frame — K writers on a stalled
 * controller spent `K * tx_timeout_ms` in series, and the destructor, which takes
 * the same lock, inherited the whole series. The FULL policy's own promise (a
 * bounded drop, counted in `tx_dropped()`) still looked honest the entire time,
 * right up to the watchdog reboot.
 *
 * What makes it observable rather than a code reading: the fake's semaphore counts
 * how many callers are parked in `xSemaphoreTake` AT ONCE. Serialized waits can
 * never show more than one, however many writers are queued; per-frame waits show
 * all of them. The wall-clock readings are reported alongside, but the assertions
 * on them are deliberately loose — a factor-of-five separation asserted at three.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_twai.hpp"
#include "libtracer_esp/twai_link.hpp"

namespace {

using tr::net::can_frame_data_t;
using tr::net::twai_link_config_t;
using tr::net::twai_link_t;

int g_failures = 0;

/** @brief Record one assertion's verdict. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Record a verdict that also reports the number that decided it. */
void check_eq(std::size_t got, std::size_t want, std::string_view what) {
    const bool ok = got == want;
    std::printf("  [%s] %.*s (got %zu, want %zu)\n", ok ? "PASS" : "FAIL",
                static_cast<int>(what.size()), what.data(), got, want);
    if (!ok) ++g_failures;
}

/** @brief Record a verdict that reports the millisecond reading behind it. */
void check_under_ms(long long got, long long ceiling, std::string_view what) {
    const bool ok = got < ceiling;
    std::printf("  [%s] %.*s (took %lld ms, ceiling %lld ms)\n", ok ? "PASS" : "FAIL",
                static_cast<int>(what.size()), what.data(), got, ceiling);
    if (!ok) ++g_failures;
}

/** @brief Milliseconds since @p start, as a plain integer. */
long long ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

/**
 * @brief A link config with a ONE-deep driver queue, so the pool holds two frames.
 *
 * Two is the smallest depth that still distinguishes "the pool is full" from "the
 * pool is empty", and it keeps the number of writes needed to saturate the link at
 * two. @p timeout_ms is the per-frame backpressure window under test.
 */
twai_link_config_t config_with_timeout(std::uint32_t timeout_ms) {
    twai_link_config_t config;
    config.tx_gpio = 4;
    config.rx_gpio = 5;
    config.tx_queue_depth = 1;  // pool = tx_queue_depth + 1 = 2 in-flight frames
    config.rx_queue_depth = 4;
    config.tx_timeout_ms = timeout_ms;
    return config;
}

/** @brief A minimal classic frame — the link only has to accept and submit it. */
can_frame_data_t frame(std::uint8_t seed) {
    can_frame_data_t f;
    f.id = 0x1234u + seed;
    f.fd = false;
    f.len = 4;
    for (std::uint8_t i = 0; i < f.len; ++i) f.data[i] = static_cast<std::byte>(seed + i);
    return f;
}

/** @brief Saturate the link's slot pool: two accepted frames the driver never completes. */
void fill_the_pool(twai_link_t& link) {
    link.write_raw(frame(1));
    link.write_raw(frame(2));
}

/** @brief The watchdog window the HOST build derives its clamp from, milliseconds.
 *
 * `twai_link.cpp` reads `CONFIG_ESP_TASK_WDT_TIMEOUT_S` when sdkconfig.h defines
 * it and falls back to IDF's own Kconfig default otherwise. There is no sdkconfig
 * on the host, so this build takes the fallback — five seconds. */
constexpr std::uint32_t kHostWdtMs = 5000;

/**
 * @brief The headline: K writers on a stalled controller wait side by side.
 *
 * Pre-fix, `write_m_` was held across the take, so the semaphore never saw more
 * than one parked caller and the five windows ran end to end.
 */
void test_the_backpressure_window_is_per_frame_not_per_queue() {
    std::printf("the backpressure window is per frame, not per queue:\n");
    fake_twai::reset();

    constexpr std::uint32_t kWindowMs = 300;
    constexpr int kWriters = 5;
    auto link = std::make_unique<twai_link_t>(config_with_timeout(kWindowMs));
    check(link->ok(), "the link came up on the fake controller");
    if (!link->ok()) return;

    fill_the_pool(*link);
    check_eq(fake_twai::frames_in_flight(), 2, "the driver is holding both pool slots");
    check_eq(link->tx_dropped(), 0, "and nothing has been dropped yet");

    const std::size_t dropped_before = link->tx_dropped();
    const std::size_t accepted_before = fake_twai::transmits_accepted();
    const unsigned peak_before = fake_twai::peak_tx_waiters();

    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back(
            [&link, i] { link->write_raw(frame(static_cast<std::uint8_t>(10 + i))); });
    }
    for (auto& w : writers) w.join();
    const long long elapsed = ms_since(start);

    check(fake_twai::peak_tx_waiters() > peak_before,
          "more than one writer was parked on the slot semaphore at once");
    std::printf("       peak parked writers: %u (was %u before the burst)\n",
                fake_twai::peak_tx_waiters(), peak_before);
    // Serialized, the five windows are 1500 ms; per-frame they overlap into one.
    check_under_ms(elapsed, 3 * kWindowMs, "and the five windows overlapped instead of queueing");

    check_eq(link->tx_dropped() - dropped_before, kWriters,
             "every one of them is a COUNTED drop, as the FULL policy promises");
    check_eq(fake_twai::transmits_accepted() - accepted_before, 0,
             "and none of them reached the controller");
}

/**
 * @brief Teardown releases parked writers instead of queueing behind their windows.
 *
 * Pre-fix the destructor waited on `write_m_`, which a parked writer held for its
 * whole window, so a link removal during a bus fault blocked the destroying task
 * for the same multiple. The second half is the reordering's own hazard: a writer
 * parked on the semaphore must not still be parked on it when it is deleted, and
 * must not submit to the node the destructor just deleted.
 */
void test_teardown_does_not_queue_behind_parked_writers() {
    std::printf("teardown does not queue behind parked writers:\n");
    fake_twai::reset();

    constexpr std::uint32_t kWindowMs = 600;
    constexpr int kWriters = 5;
    auto link = std::make_unique<twai_link_t>(config_with_timeout(kWindowMs));
    check(link->ok(), "the link came up on the fake controller");
    if (!link->ok()) return;

    fill_the_pool(*link);
    const std::size_t accepted_before = fake_twai::transmits_accepted();

    std::atomic<int> returned{0};
    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int i = 0; i < kWriters; ++i) {
        writers.emplace_back([&link, &returned, i] {
            link->write_raw(frame(static_cast<std::uint8_t>(20 + i)));
            returned.fetch_add(1, std::memory_order_relaxed);
        });
    }
    // Long enough for all five to be parked, short enough that most of each
    // window is still ahead of them.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check_eq(static_cast<std::size_t>(returned.load(std::memory_order_relaxed)), 0,
             "all five writers are still parked when teardown starts");

    const auto start = std::chrono::steady_clock::now();
    link.reset();
    const long long elapsed = ms_since(start);
    for (auto& w : writers) w.join();

    // The dispatch thread's own queue poll is 100 ms, so teardown can never be
    // instant; one writer's REMAINING window alone is ~500 ms, and five in series
    // is 3000 ms.
    check_under_ms(elapsed, 350, "the destructor did not inherit a writer's window");
    check_eq(static_cast<std::size_t>(returned.load(std::memory_order_relaxed)), kWriters,
             "and every parked writer was released rather than left on a deleted semaphore");
    check_eq(fake_twai::transmits_accepted() - accepted_before, 0,
             "no writer submitted to the node teardown had already deleted");
    check(!fake_twai::node_alive(), "the node is gone");
}

/**
 * @brief A caller-supplied window is clamped to the task-watchdog period.
 *
 * `tx_timeout_ms` had no ceiling, so a config could park a writing task past the
 * window a task may go unfed. The tick budget the link asks its semaphore for is
 * the whole observable — no wait has to be spent to read it.
 */
void test_the_window_is_clamped_to_the_watchdog_period() {
    std::printf("a caller's window is clamped to the watchdog period:\n");

    fake_twai::reset();
    {
        auto link = std::make_unique<twai_link_t>(config_with_timeout(60000));
        check(link->ok(), "a link asking for a 60 s window came up");
        link->write_raw(frame(1));  // one take, with the window the link settled on
        const TickType_t asked = fake_twai::last_take_ticks();
        check(asked < pdMS_TO_TICKS(60000), "it did not park for the 60 s it asked for");
        check_eq(asked, pdMS_TO_TICKS(kHostWdtMs), "it parked for one watchdog window");
    }

    fake_twai::reset();
    {
        // The control: an ordinary window is passed through untouched, so the
        // clamp is a ceiling rather than a replacement.
        auto link = std::make_unique<twai_link_t>(config_with_timeout(20));
        check(link->ok(), "a link asking for the default 20 ms window came up");
        link->write_raw(frame(1));
        check_eq(fake_twai::last_take_ticks(), pdMS_TO_TICKS(20),
                 "and it got exactly the 20 ms it asked for");
    }
}

/**
 * @brief The ordinary path still works: a completed transmit frees its slot.
 *
 * The control for the whole suite. Moving the take out of the lock added two new
 * places that hand a token back (a node that vanished, a pool that refused), and
 * a token leaked in either of them would starve the pool a few frames later — a
 * failure the stalled-controller cases above cannot see, because they never
 * complete anything.
 */
void test_a_completed_transmit_returns_its_slot() {
    std::printf("a completed transmit returns its slot:\n");
    fake_twai::reset();

    auto link = std::make_unique<twai_link_t>(config_with_timeout(300));
    check(link->ok(), "the link came up on the fake controller");
    if (!link->ok()) return;

    fill_the_pool(*link);
    check_eq(fake_twai::transmits_accepted(), 2, "both frames reached the controller");

    // Twenty write/complete cycles on a two-slot pool: a token lost on any of them
    // shrinks the pool, and a pool that has shrunk to zero turns the next write
    // into a full-window stall and a counted drop.
    std::size_t completions = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
        if (fake_twai::complete_one_tx()) ++completions;
        link->write_raw(frame(static_cast<std::uint8_t>(30 + i)));
    }
    const long long elapsed = ms_since(start);

    check_eq(completions, 20, "the tx-done callback ran on every cycle");
    check_eq(fake_twai::transmits_accepted(), 22, "every follow-on write reached the controller");
    check_eq(link->tx_dropped(), 0, "and not one of them was dropped");
    check_under_ms(elapsed, 300, "none of them spent a backpressure window");
}

}  // namespace

int main() {
    std::printf("twai_link TX backpressure host suite (#962):\n");
    test_the_backpressure_window_is_per_frame_not_per_queue();
    test_teardown_does_not_queue_behind_parked_writers();
    test_the_window_is_clamped_to_the_watchdog_period();
    test_a_completed_transmit_returns_its_slot();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
