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
 *   - a core that idles on its own between bursts never makes the link wait;
 *   - a send queued from another task is sent from INSIDE the park, so the httpd task (the
 *     one drain of its own control queue) never holds the link's egress, and a peer that
 *     provokes a send per frame buys no ingress with it: the budget stays spent until idle.
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
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "esp_freertos_hooks.h"
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
    /**
     * @param between What httpd's loop does between two readable frames; the real server
     *                drains its control socket there (one queued item per pass), which is
     *                where a queued send reaches the wire. Default: nothing.
     */
    ingress_t(
        int fd, std::size_t frames, std::size_t bytes, std::function<void()> between = [] {})
        : body_(bytes, std::byte{0x5A}), thread_([this, fd, frames, between] {
              for (std::size_t i = 0; i < frames; ++i) {
                  between();
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

// ---------------------------------------------------------------------------
// 6 — egress work posted from another task wakes a parked drain: the httpd task is the one
//     drain of its own control queue, so a park for idle must not hold the link's sends.
// ---------------------------------------------------------------------------
/** @brief The directed endpoint of the single peer currently open (see tx_pool_test). */
tr::net::transport_t* only_peer(httpd_ws_link_t& link) {
    std::string name;
    link.enumerate_peers([&name](std::string_view p) { name = std::string(p); });
    return name.empty() ? nullptr : link.peer_link(name);
}

void test_egress_work_is_sent_from_inside_the_park() {
    constexpr std::size_t kBudget = tr::net::kRxDrainFrames;
    std::printf("a push queued from another task is sent from inside the park, no idle step:\n");
    auto link = fresh_link(806);
    fake_httpd::instance().clear_sent_frames();
    // The httpd loop, as the real one runs: drain the control socket, then the next frame.
    const std::size_t total = 2 * kBudget;
    ingress_t in(806, total, 8, [] { (void)fake_httpd::instance().run_pending(); });
    check(wait_until([&] { return parked_after(kBudget); }), "the drain is parked");
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the flooding peer resolved to a directed endpoint");
    if (peer == nullptr) {
        (void)fake_httpd::run_idle_hooks();
        finish(std::move(link));
        return;
    }

    // One push, from this thread (a producer task, never the httpd one): it must reach the
    // wire while the core has NOT idled, and the ingress must not move for it.
    const std::byte body[4] = {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    peer->send(std::span<const std::byte>(body));
    check(wait_until([&] { return fake_httpd::instance().sent_frames().size() == 1; }),
          "the push is on the wire with no idle step");
    check(fake_httpd::semaphore_waiters() == 1 && g_payload_reads.load() == kBudget,
          "and the drain is still parked on the same frame: the egress bought no ingress");
    check(link->stats().rx_drain_waits == 1, "one wait, still in progress");
    check(link->stats().enqueue_drops == 0 && link->stats().tx_pool_misses == 0,
          "nothing was dropped or missed on the way");

    // The peer-provoked case: a producer that posts a send per inbound frame (a subscriber
    // over another link, an app replying off-task) pushes faster than any idle step. Every
    // one of them goes out, and the ingress still reads NOTHING until the core idles.
    // A slot the park sent stays busy until the loop's copy of its item runs, and the loop
    // cannot run while the ingress thread is parked, so one park serves at most a pool depth
    // of pushes. That is the bound the pool always was (#949): the push past it is the
    // counted pool miss it would have been before any of this, never a silent loss.
    constexpr std::size_t kPool = httpd_ws_link_t::kDefaultTxPoolSlots;
    bool each_sent = true;
    for (std::size_t i = 1; i < kPool && each_sent; ++i) {
        peer->send(std::span<const std::byte>(body));
        each_sent =
            wait_until([&] { return fake_httpd::instance().sent_frames().size() == i + 1; });
    }
    check(each_sent, "a flood of pushes goes out one by one through the park, a pool deep");
    check(fake_httpd::instance().sent_frames().size() == kPool,
          "every provoked push that found a slot is on the wire");
    peer->send(std::span<const std::byte>(body));  // the pool is full; this one waits, then drops
    check(wait_until([&] { return link->stats().tx_pool_misses == 1; }),
          "the push past the pool depth is the counted pool miss the pool always imposed");
    check(link->stats().enqueue_drops == 1 && fake_httpd::instance().sent_frames().size() == kPool,
          "counted once as an enqueue drop, and not on the wire");
    check(fake_httpd::semaphore_waiters() == 1 && g_payload_reads.load() == kBudget,
          "and the ingress has not read one frame for all of them");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    check(g_payload_reads.load() == kBudget && in.delivered() == kBudget,
          "nothing moves on a quiet gate: the budget stays spent");

    // The core idles: the second budget is read, the loop's copies of the items release
    // the slots they found already sent, and no push goes out twice.
    (void)fake_httpd::run_idle_hooks();
    check(wait_until([&] { return in.delivered() == total; }),
          "after the core idles the second budget is read to the end");
    (void)fake_httpd::instance().run_pending();
    check(fake_httpd::instance().sent_frames().size() == kPool,
          "no push was duplicated by the loop's copy of its item");
    check(link->tx_slots_busy() == 0, "and every slot came back once the copies ran");
    finish(std::move(link));
}

// ---------------------------------------------------------------------------
// 5 — the hook could not be installed: the link must NOT pace (it would wait forever).
// ---------------------------------------------------------------------------
/** @brief A hook that only occupies a slot. */
bool occupying_hook() { return true; }

void test_full_hook_table_leaves_the_link_unpaced() {
    constexpr std::size_t kBudget = tr::net::kRxDrainFrames;
    std::printf("with core 0's idle-hook table full, the link reads without pausing:\n");
    // Fill IDF's eight per-core slots BEFORE the first link installs its hook (once per
    // process, so this case runs in its own process: `httpd_ws_drain_budget_test unhooked`).
    int filled = 0;
    while (esp_register_freertos_idle_hook_for_cpu(&occupying_hook, 0) == ESP_OK) ++filled;
    check(filled == 8, "the fake's per-core hook table holds eight, as IDF's does");
    auto link = fresh_link(805);
    const std::size_t total = 2 * kBudget + 1;
    ingress_t in(805, total, 8);
    check(wait_until([&] { return in.delivered() == total; }),
          "a sustained ingress past two budgets is read to the end, never parked");
    check(fake_httpd::semaphore_waiters() == 0, "nothing waits on the idle gate");
    check(link->stats().rx_drain_waits == 0,
          "and rx_drain_waits reads 0 — indistinguishable from an unsaturated link");
    finish(std::move(link));
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("httpd_ws_link ingress drain budget (ADR-0085):\n");
    if constexpr (tr::net::kRxDrainFrames == 0 || tr::net::kRxDrainBytes == 0) {
        std::printf("SKIP: this build binds a zero drain budget; the cases assume both are set\n");
        return 0;
    }
    fake_httpd::instance().set_frame_hook([] { g_payload_reads.fetch_add(1); });
    fake_httpd::instance().set_sustained_ingress(true);  // the core idles only when told
    if (argc > 1 && std::string(argv[1]) == "unhooked") {
        test_full_hook_table_leaves_the_link_unpaced();
    } else {
        test_sustained_ingress_yields_after_frame_budget();
        test_byte_budget_ends_a_drain_of_large_frames();
        test_an_idle_core_resets_the_drain();
        test_teardown_while_parked_joins_after_idle();
        test_egress_work_is_sent_from_inside_the_park();
    }
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
