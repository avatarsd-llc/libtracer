/**
 * @file
 * @brief ADR-0085 — the ingress DRAIN BUDGET: a peer that never stops sending cannot keep
 *        the httpd task reading past the budget until its core has idled.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the other httpd_ws suites: the REAL chip translation unit
 * (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against the host fake of
 * `esp_http_server` (fake_httpd.hpp). The seam driven is the highest one the link has on
 * the host: frames are pushed into the registered URI handler exactly as httpd's task
 * would, from a thread that plays that task.
 *
 * The host has no idle task. The fake keeps IDF's idle-hook table; in its sustained-ingress
 * mode it runs the hooks only when a test calls `fake_httpd::run_idle_hooks`, so "the core
 * idled" is a step the test takes.
 * That turns the property under test into plain counting:
 *
 *   - a sustained ingress is read for exactly one budget, then the httpd thread PARKS with
 *     the next frame's payload unread (the bytes stay in the socket for TCP to push back);
 *   - it resumes only after the core idles, and does it again one budget later;
 *   - the byte budget ends a drain of large frames before the frame budget would;
 *   - a core that idles on its own between bursts never makes the link wait.
 *
 * What it cannot show is the silicon half: that the idle task really runs once the httpd
 * task parks, and what the pacing costs in throughput. That is the on-silicon plan in
 * ADR-0085.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_httpd.hpp"
#include "libtracer/config.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

namespace {

using tr::net::httpd_ws_link_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief Payload passes the link has made: the bytes it actually took off the socket. */
std::atomic<std::size_t> g_payload_reads{0};

/**
 * @brief Wait (test-side; the library reads no clock) until @p done holds, or 5 s pass.
 * @return Whether it held.
 */
bool wait_until(const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/** @brief True once the httpd thread is parked on the idle gate with @p reads payloads read. */
bool parked_after(std::size_t reads) {
    return fake_httpd::semaphore_waiters() == 1 && g_payload_reads.load() == reads;
}

/**
 * @brief The httpd task, played by a thread: delivers @p frames frames of @p bytes each
 *        to @p fd back to back, as httpd does while the socket stays readable.
 */
class ingress_t {
   public:
    ingress_t(int fd, std::size_t frames, std::size_t bytes)
        : body_(bytes, std::byte{0x5A}), thread_([this, fd, frames] {
              for (std::size_t i = 0; i < frames; ++i) {
                  (void)fake_httpd::instance().deliver_frame(fd, std::span<const std::byte>(body_));
                  delivered_.fetch_add(1);
              }
          }) {}
    ingress_t(const ingress_t&) = delete;
    ingress_t& operator=(const ingress_t&) = delete;
    ~ingress_t() {
        if (thread_.joinable()) thread_.join();
    }
    /** @brief Frames whose handler call has returned. */
    [[nodiscard]] std::size_t delivered() const { return delivered_.load(); }

   private:
    std::vector<std::byte> body_;
    std::atomic<std::size_t> delivered_{0};
    std::thread thread_;
};

/** @brief A fresh link and a fresh drain: the core idles once before the burst starts. */
std::unique_ptr<httpd_ws_link_t> fresh_link(int fd) {
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws",
                                                  tr::net::httpd_ws_config_t{.peer_named = true});
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::run_idle_hooks();
    g_payload_reads.store(0);
    return link;
}

void finish(std::unique_ptr<httpd_ws_link_t> link) {
    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 1 — a sustained ingress of small frames yields after the FRAME budget, every time.
// ---------------------------------------------------------------------------
void test_sustained_ingress_yields_after_frame_budget() {
    constexpr std::size_t kBudget = tr::net::kRxDrainFrames;
    std::printf("a sustained ingress parks after %zu frames until the core idles:\n", kBudget);
    auto link = fresh_link(801);
    check(fake_httpd::run_idle_hooks() >= 1, "the link installed an idle hook on core 0");

    // Three budgets and one frame: small enough that the byte budget never ends a drain.
    const std::size_t total = 3 * kBudget + 1;
    ingress_t in(801, total, 8);

    check(wait_until([&] { return parked_after(kBudget); }),
          "the httpd thread parks with exactly one budget of payloads read");
    check(in.delivered() == kBudget, "and the frame past the budget has not been handled");
    check(link->stats().rx_drain_waits == 1, "the wait is counted once");

    // Nothing moves until the core idles: the unread frame is still the peer's problem.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(g_payload_reads.load() == kBudget, "no payload is read while the core has not idled");

    (void)fake_httpd::run_idle_hooks();
    check(wait_until([&] { return parked_after(2 * kBudget); }),
          "after the core idles it reads exactly one more budget, then parks again");
    check(link->stats().rx_drain_waits == 2, "the second wait is counted");

    (void)fake_httpd::run_idle_hooks();
    check(wait_until([&] { return parked_after(3 * kBudget); }), "and again for the third");

    (void)fake_httpd::run_idle_hooks();
    check(wait_until([&] { return in.delivered() == total; }),
          "the last frame is read after the third idle: nothing was dropped");
    check(g_payload_reads.load() == total, "every payload was read exactly once");
    check(link->stats().rx_drain_waits == 3, "three waits in all");
    finish(std::move(link));
}

// ---------------------------------------------------------------------------
// 2 — large frames: the BYTE budget ends the drain before the frame budget would.
// ---------------------------------------------------------------------------
void test_byte_budget_ends_a_drain_of_large_frames() {
    constexpr std::size_t kFrame = 4096;
    constexpr std::size_t kByFrames = tr::net::kRxDrainFrames;
    constexpr std::size_t kByBytes = (tr::net::kRxDrainBytes + kFrame - 1) / kFrame;
    constexpr std::size_t kExpected = kByBytes < kByFrames ? kByBytes : kByFrames;
    std::printf("%zu-byte frames park after %zu frames (the byte budget, %zu B):\n", kFrame,
                kExpected, tr::net::kRxDrainBytes);
    auto link = fresh_link(802);
    ingress_t in(802, kExpected + 1, kFrame);

    check(wait_until([&] { return parked_after(kExpected); }),
          "the drain ends once its bytes reach the budget");
    check(kByBytes < kByFrames, "and it was the byte budget, not the frame budget, that ended it");
    check(link->stats().rx_drain_waits == 1, "one wait");

    (void)fake_httpd::run_idle_hooks();
    check(wait_until([&] { return in.delivered() == kExpected + 1; }),
          "the waiting frame is read whole once the core idles");
    finish(std::move(link));
}

// ---------------------------------------------------------------------------
// 3 — a core that idles between bursts never makes the link wait.
// ---------------------------------------------------------------------------
void test_an_idle_core_resets_the_drain() {
    constexpr std::size_t kBudget = tr::net::kRxDrainFrames;
    std::printf("bursts shorter than the budget, with the core idling between them, never wait:\n");
    auto link = fresh_link(803);
    std::atomic<bool> done{false};
    std::thread httpd([&] {
        const std::byte body[8] = {};
        for (int burst = 0; burst < 4; ++burst) {
            for (std::size_t i = 0; i + 1 < kBudget; ++i)
                (void)fake_httpd::instance().deliver_frame(803, std::span<const std::byte>(body));
            (void)fake_httpd::run_idle_hooks();  // the socket ran dry; the core idled
        }
        done.store(true);
    });
    const bool finished = wait_until([&] { return done.load(); });
    check(finished, "four bursts of budget-1 frames complete without parking");
    if (!finished) (void)fake_httpd::run_idle_hooks();  // unpark so the join returns
    httpd.join();
    check(link->stats().rx_drain_waits == 0, "and no wait was counted");
    check(g_payload_reads.load() == 4 * (kBudget - 1), "every payload was read");
    finish(std::move(link));
}

// ---------------------------------------------------------------------------
// 4 — a link torn down while its httpd thread is parked: the destructor joins the frame.
// ---------------------------------------------------------------------------
void test_teardown_while_parked_joins_after_idle() {
    constexpr std::size_t kBudget = tr::net::kRxDrainFrames;
    std::printf("a destructor racing a parked drain completes once the core idles:\n");
    auto link = fresh_link(804);
    ingress_t in(804, kBudget + 1, 8);
    check(wait_until([&] { return parked_after(kBudget); }), "the drain is parked");

    std::atomic<bool> destroyed{false};
    std::thread app([&] {
        link.reset();
        destroyed.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(!destroyed.load(), "the destructor waits for the in-flight handler frame");
    (void)fake_httpd::run_idle_hooks();
    check(wait_until([&] { return destroyed.load(); }), "and returns once the core idles");
    app.join();
    check(wait_until([&] { return in.delivered() == kBudget + 1; }), "the parked frame finished");
    fake_httpd::instance().close_all();
}

}  // namespace

int main() {
    std::printf("httpd_ws_link ingress drain budget (ADR-0085):\n");
    if constexpr (tr::net::kRxDrainFrames == 0 || tr::net::kRxDrainBytes == 0) {
        std::printf("SKIP: this build binds a zero drain budget; the cases assume both are set\n");
        return 0;
    }
    fake_httpd::instance().set_frame_hook([] { g_payload_reads.fetch_add(1); });
    fake_httpd::instance().set_sustained_ingress(true);  // the core idles only when told
    test_sustained_ingress_yields_after_frame_budget();
    test_byte_budget_ends_a_drain_of_large_frames();
    test_an_idle_core_resets_the_drain();
    test_teardown_while_parked_joins_after_idle();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
