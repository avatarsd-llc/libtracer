/**
 * @file
 * @brief #952 — the WS *client* link's BOUNDED-BLOCKING discipline: what a send may
 *        cost, what a teardown may cost, and who may still be inside the transport
 *        when the handles are destroyed.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #900/#901 receive-path suite next to it: the REAL chip
 * translation unit (`integrations/esp-idf/libtracer/esp_ws_client_link.cpp`) compiled
 * against a host fake of `esp_transport_ws` (fake_esp_transport.hpp). That suite scripts
 * the fake's inbound frames; this one scripts its BLOCKING — a dial that hangs, a dial
 * that fails, a write that parks — because every defect here is a wait nobody bounded.
 * No socket is opened at any layer (#947).
 *
 * The three defects, all on one seam (`write_m_` and `stop_` were treated as if the
 * operations under them were prompt):
 *
 *   1. `send()` held `write_m_` across up to 3 x 4000 ms of transport I/O. IDF's
 *      `_ws_write` spends the caller's timeout on a poll, then the header write, then
 *      the payload write, so the literal was a 12 s stall against a 5 s task watchdog —
 *      a panic, not a dropped frame. `write_m_` is also the recv thread's read
 *      serializer, so a stalled link could not even re-dial itself.
 *   2. `stop_` was read once per loop turn and by nothing inside the turn: the 1.5 s
 *      reconnect backoff was a plain `sleep_for`, so the destructor's join waited out
 *      the full backoff of exactly the unreachable peer a re-dial exists for.
 *   3. The destructor destroyed the transport handles with `write_m_` held NOWHERE, on
 *      the premise that the joined recv thread was the only handle user — which the
 *      header's own contract contradicts ("`send()` may be called from ANY task"). A
 *      sender queued behind a stalled write woke up owning a destroyed handle.
 *
 * What makes defect 3 observable rather than a code reading: the fake tracks handle
 * LIVENESS, and its parked write re-checks it on the way out. The real
 * `esp_transport_write` dereferences the handle throughout the call, so a handle
 * destroyed while a writer is inside it is a use-after-free even though the pointer was
 * live on entry — `fake_ws::handle_misuse()` counts exactly that, plus every call made
 * on a null or already-destroyed handle.
 *
 * What this suite does NOT claim: that a `send()` STARTING after the destructor has
 * returned is safe. No barrier inside an object can answer that, and the fix does not
 * pretend to — the embedder owns the link's lifetime against its own callers.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_esp_transport.hpp"
#include "libtracer_esp/esp_ws_client_link.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::esp_ws_client_link_t;

/** @brief Failed-check counter; main() turns it into the exit status. */
int g_failures = 0;

/** @brief Record one assertion's verdict. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Record a verdict that also reports the two numbers that decided it. */
void check_le(long long got, long long ceiling, std::string_view what) {
    const bool ok = got <= ceiling;
    std::printf("  [%s] %.*s (got %lld, ceiling %lld)\n", ok ? "PASS" : "FAIL",
                static_cast<int>(what.size()), what.data(), got, ceiling);
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

/** @brief Spin until @p pred holds or @p limit elapses. @return whether it held. */
template <typename pred_t>
bool wait_until(pred_t pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

/**
 * @brief The watchdog window the HOST build derives its bounds from, milliseconds.
 *
 * `esp_ws_client_link.cpp` reads `CONFIG_ESP_TASK_WDT_TIMEOUT_S` when sdkconfig.h
 * defines it and falls back to IDF's own Kconfig default otherwise. There is no
 * sdkconfig on the host, so this build takes the fallback — five seconds. The same
 * fallback `twai_link.cpp` and `httpd_ws_link.cpp` take in their host suites.
 */
constexpr long long kHostWdtMs = 5000;

/** @brief Rx/tx buffer size every case here builds its link with. */
constexpr std::size_t kBufBytes = 256;

/** @brief IDF spends the write timeout on three legs inside ONE esp_transport_write
 *         (poll_write, the WS header write, the payload write — transport_ws.c). */
constexpr long long kIdfWriteLegs = 3;

/** @brief A small outbound frame — the size of the control TLVs this link carries. */
std::vector<std::byte> payload() {
    std::vector<std::byte> f;
    for (int i = 0; i < 8; ++i) f.push_back(static_cast<std::byte>(0xB0 + i));
    return f;
}

/** @brief Build a link on the fake and wait for its first dial. */
std::unique_ptr<esp_ws_client_link_t> dialing_link() {
    auto link =
        std::make_unique<esp_ws_client_link_t>("127.0.0.1", 8080, "/ws", kBufBytes, kBufBytes, 0);
    check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s), "the link dialed");
    return link;
}

/**
 * @brief The headline (defect 3): teardown must not destroy the handles under a sender.
 *
 * Two senders are staged against a peer that has stopped accepting bytes: one INSIDE
 * `esp_transport_write` (holding the send serializer for the whole call) and one queued
 * behind it on that serializer. Pre-fix the destructor joined the recv thread and then
 * destroyed both handles with the serializer held nowhere, so it returned while the
 * first sender was still inside the transport and the second woke up to write through a
 * destroyed (or freshly nulled) handle.
 */
void test_teardown_does_not_destroy_handles_under_a_sender() {
    std::printf("teardown does not destroy the handles under a sender:\n");
    fake_ws::reset();
    fake_ws::hold_writes(true);

    auto link = dialing_link();
    check(wait_until([&] { return link->ok(); }, 2s), "and came up connected");
    const std::vector<std::byte> frame = payload();

    // The senders hold the LINK, never the owning unique_ptr: `link.reset()` writes that
    // smart pointer's stored address while a `link->` in a sender thread would be reading
    // it, which is a race on the test's own bookkeeping (the #962 suite hit exactly this
    // under TSan). The link object itself is safe to hold — that is the contract staged.
    esp_ws_client_link_t* const under_test = link.get();

    std::atomic<int> seq{0};
    std::atomic<int> a_at{0};  // sequence position at which sender A returned
    std::atomic<int> b_at{0};  // ... B ...
    std::atomic<int> d_at{0};  // ... and at which the destructor returned

    std::thread a([&] {
        under_test->send(frame);
        a_at.store(++seq, std::memory_order_release);
    });
    check(wait_until([] { return fake_ws::writers_inside() >= 1; }, 2s),
          "sender A is parked INSIDE the transport, holding the send serializer");

    std::thread b([&] {
        under_test->send(frame);
        b_at.store(++seq, std::memory_order_release);
    });
    // B cannot announce itself from inside the link, so it is placed by time and then
    // pinned by what the fake did NOT see: A holds the serializer, so a B that has
    // reached the transport would show up as a second entered write. Short, because
    // every millisecond spent here is a millisecond of A's park already gone.
    std::this_thread::sleep_for(50ms);
    check(fake_ws::writes_started() == 1,
          "sender B is queued on the serializer, not in the transport");

    // Nothing releases the peer: A leaves when the LINK's OWN write bound expires, which
    // is the whole point. Post-fix that is ~416 ms (case 4 pins the derivation) and the
    // destructor must sit through the remainder of it; pre-fix it was 4000 ms, and the
    // destructor sailed past it at the join (a ~200 ms poll turn) and destroyed the
    // handles with A still inside the transport.
    const auto start = std::chrono::steady_clock::now();
    link.reset();
    d_at.store(++seq, std::memory_order_release);
    const long long elapsed = ms_since(start);
    a.join();
    b.join();

    const int a_pos = a_at.load(std::memory_order_acquire);
    const int b_pos = b_at.load(std::memory_order_acquire);
    const int d_pos = d_at.load(std::memory_order_acquire);
    std::printf("       return order: A=%d B=%d destructor=%d, teardown %lld ms\n", a_pos, b_pos,
                d_pos, elapsed);
    check(a_pos != 0 && a_pos < d_pos,
          "the destructor did not return while a sender was inside the transport");
    check(b_pos != 0 && b_pos < d_pos, "nor while one was queued on the serializer");
    check(fake_ws::handle_misuse() == 0,
          "and no operation touched a destroyed, foreign or null handle");
    check(fake_ws::writes_started() == 1,
          "the queued sender left without writing through the torn-down handle");
    // Waiting for a sender is not licence to wait forever: what the destructor inherits
    // is one write bound, and that bound is now a quarter of a watchdog window.
    check_under_ms(elapsed, kHostWdtMs / 2,
                   "and the wait it inherited was one write bound, not an open one");
}

/**
 * @brief Defect 2: the reconnect backoff is a bound, not a sentence.
 *
 * With every dial failing, the recv thread spends its life in the backoff. Pre-fix that
 * was `sleep_for(1500)`, which the destructor's join inherited whole — on exactly the
 * unreachable peer a re-dial exists to recover from.
 */
void test_teardown_does_not_wait_out_the_reconnect_backoff() {
    std::printf("teardown does not wait out the reconnect backoff:\n");
    fake_ws::reset();
    fake_ws::fail_connects(true);

    auto link = dialing_link();
    check(!link->ok(), "the dial failed, so the link is down and backing off");

    const auto start = std::chrono::steady_clock::now();
    link.reset();
    const long long elapsed = ms_since(start);

    // The backoff is 1500 ms and the recv thread enters it immediately after the failed
    // dial, so pre-fix this reading is the whole of it.
    check_under_ms(elapsed, 500, "the destructor cut the 1500 ms backoff short");
    check(fake_ws::handle_misuse() == 0, "and nothing touched a destroyed handle on the way out");
}

/**
 * @brief Defect 2, the residual: a teardown that lands mid-dial is BOUNDED.
 *
 * `esp_transport_connect` takes no cancellation, so this wait is the one the fix does
 * not remove — it makes it derivable instead. The dial bound is half a watchdog window,
 * so a destroying task that lands inside a dial still gets to feed the watchdog; pre-fix
 * the literal was a whole window, which is the panic itself.
 */
void test_a_teardown_that_lands_mid_dial_is_bounded() {
    std::printf("a teardown that lands mid-dial is bounded by the watchdog window:\n");
    fake_ws::reset();
    fake_ws::hang_connects(true);

    auto link = dialing_link();
    std::printf("       the dial asked for %d ms\n", fake_ws::last_connect_timeout_ms());

    const auto start = std::chrono::steady_clock::now();
    link.reset();
    const long long elapsed = ms_since(start);

    check_under_ms(elapsed, kHostWdtMs * 3 / 4,
                   "the destroying task got the window back inside its watchdog period");
    check(fake_ws::handle_misuse() == 0, "and nothing touched a destroyed handle on the way out");
}

/**
 * @brief Defect 1: both blocking bounds are derived from the task-watchdog period.
 *
 * The numbers the link hands IDF are the whole observable — no wait has to be spent to
 * read them. Pre-fix they were bare literals: 4000 ms per write leg (12 s of stall on a
 * closed TCP window) and 5000 ms per dial, against a 5 s watchdog.
 *
 * The ordinary send is the control: bounding the write must not stop the bytes getting
 * out, and the entry checks the fix added (`stop_`, a null handle) must not swallow a
 * frame on a live link.
 */
void test_the_blocking_bounds_are_derived_from_the_watchdog() {
    std::printf("the blocking bounds are derived from the watchdog period:\n");
    fake_ws::reset();

    auto link = dialing_link();
    check(wait_until([&] { return link->ok(); }, 2s), "and came up connected");

    const std::vector<std::byte> frame = payload();
    link->send(frame);

    const long long write_ms = fake_ws::last_write_timeout_ms();
    const long long dial_ms = fake_ws::last_connect_timeout_ms();
    std::printf("       write %lld ms x %lld legs, dial %lld ms, watchdog %lld ms\n", write_ms,
                kIdfWriteLegs, dial_ms, kHostWdtMs);

    check(write_ms > 0 && dial_ms > 0, "both bounds are real waits, not zero");
    check_le(write_ms * kIdfWriteLegs, kHostWdtMs / 2,
             "one send's TOTAL write bound is at most half a watchdog window");
    check_le(dial_ms, kHostWdtMs / 2, "one dial is at most half a watchdog window");
    check_le(dial_ms + write_ms * kIdfWriteLegs, kHostWdtMs,
             "and a teardown that pays a dial AND a stalled write still fits in one");

    check(fake_ws::last_write_payload() == frame,
          "the ordinary send still reached the transport, byte for byte");
    check(fake_ws::handle_misuse() == 0, "with no handle misuse anywhere in the case");
}

}  // namespace

int main() {
    std::printf("esp_ws_client_link bounded-blocking host suite (#952):\n");
    test_teardown_does_not_destroy_handles_under_a_sender();
    test_teardown_does_not_wait_out_the_reconnect_backoff();
    test_a_teardown_that_lands_mid_dial_is_bounded();
    test_the_blocking_bounds_are_derived_from_the_watchdog();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
