/**
 * @file
 * @brief #944 — a TX work slot must not be pinned for the rest of the boot by a work item
 *        the control socket accepted and then silently binned.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 teardown, #835 send-stall and #954 session-identity
 * suites: the REAL chip translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`)
 * compiled against the host fake of `esp_http_server` (fake_httpd.hpp). The event this
 * suite stages is the one the fake already models but no suite had spent — a control-queue
 * enqueue that is DROPPED while `httpd_queue_work` still returns ESP_OK
 * (`server_t::set_queue_capacity`, modelling `CONFIG_LWIP_UDP_RECVMBOX_SIZE`).
 *
 * That distinction is the whole defect. A REFUSED enqueue is visible (ESP_FAIL) and was
 * always recycled; a BINNED one is not, and the only site that cleared the slot's flag
 * was the work item itself — which never ran. Four of those killed the pool for good,
 * after which every frame on a hot publish path took two global-heap allocations.
 *
 * The four properties pinned here:
 *   1. the pool SURVIVES four binned work items — the headline, and the one that fails
 *      loudest against the pre-fix code, which pinned all four slots permanently;
 *   2. the sweep does not eat live work: a pool exhausted by frames that are merely
 *      QUEUED reclaims nothing and delivers every one of them. Without this, property 1
 *      is satisfied by a link that throws its pool away whenever it is busy;
 *   3. a token that arrives AFTER its slot was reclaimed is harmless — it sends the frame
 *      the slot now holds, exactly once, and never a torn or already-freed one. This is
 *      what makes reclaiming safe without an identity token, which a pooled work item
 *      cannot carry (it lives inside the slot a re-claim overwrites);
 *   4. the strand really is invisible to the existing accounting — `enqueue_drops` stays
 *      at zero throughout, which is why a new counter had to exist at all.
 *
 * The strand window is `kTxPoolSlots * send_timeout_ms_`, so the suite constructs its
 * links with an explicit, tight send bound through the CONSTRUCTOR PARAMETER that has
 * always been there. No test-only seam: a caller that wants a tighter bound than the
 * derived one passes it, and this suite is simply such a caller.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_httpd.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

namespace {

using fake_httpd::send_result_t;
using tr::net::httpd_ws_link_t;

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

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief A minimal frame body — the link only has to accept and route it. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/** @brief The one socket every case here serves. */
constexpr int kFd = 500;

/**
 * @brief The per-socket send bound these links are built with, milliseconds.
 *
 * Chosen only so the suite runs in milliseconds rather than seconds: the strand window is
 * `kTxPoolSlots * send_timeout_ms_`, so a 20 ms bound makes it 80 ms. A stock link derives
 * its bound from the watchdog period and the peer cap instead, which at the default cap
 * puts the same window at one full watchdog period.
 */
constexpr std::uint32_t kSendBoundMs = 20;

/** @brief The window a slot must sit armed in before the sweep may reclaim it. */
constexpr std::uint32_t kStrandWindowMs = 4 * kSendBoundMs;

/** @brief Sleep comfortably past the strand window. */
void wait_out_the_window() {
    std::this_thread::sleep_for(std::chrono::milliseconds(kStrandWindowMs + kStrandWindowMs / 2));
}

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief A link that adopts the fake server with the suite's tight send bound. */
std::unique_ptr<httpd_ws_link_t> make_link() {
    return std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true, kSendBoundMs);
}

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief The directed endpoint of the single peer currently open. */
tr::net::transport_t* only_peer(httpd_ws_link_t& link) {
    std::string name;
    link.enumerate_peers([&name](std::string_view p) { name = std::string(p); });
    return name.empty() ? nullptr : link.peer_link(name);
}

/** @brief Retire the link, the fake's sessions and its queue settings between cases. */
void reset(std::unique_ptr<httpd_ws_link_t>& link) {
    link.reset();
    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().close_all();
    drain();
}

// ---------------------------------------------------------------------------
// 1 — the pool survives four silently binned work items.
// ---------------------------------------------------------------------------
/**
 * @brief The headline: four drops used to kill the pool for the rest of the boot.
 *
 * The queue is capped and pre-filled, so every enqueue below is binned inside the fake's
 * lwIP model while `httpd_queue_work` reports ESP_OK — the link is told its frames are
 * queued and hears nothing further. Before the fix each of those claims pinned its slot
 * permanently: `tx_slots_busy()` stayed at the pool's full capacity for good.
 */
void test_the_pool_survives_four_binned_work_items() {
    std::printf("four work items accepted by the control socket and silently binned:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();

    const std::size_t capacity = httpd_ws_link_t::tx_slot_capacity();
    // Cap the control queue at one entry and spend that entry on a filler the drain never
    // sees. Every enqueue from here on is binned, not refused.
    fake_httpd::instance().set_queue_capacity(1);
    fake_httpd::instance().post([] {});
    const std::size_t drops_before = fake_httpd::instance().queue_drops();
    for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));

    check_eq(fake_httpd::instance().queue_drops() - drops_before, capacity,
             "every frame was BINNED by the control socket, not refused");
    check_eq(link->enqueue_drops(), 0,
             "the link saw NO enqueue failure: the strand is invisible to that counter");
    check_eq(link->tx_slots_busy(), capacity, "the whole pool is pinned by items that never ran");
    check_eq(link->tx_strands(), 0, "nothing has been reclaimed yet — the window has not passed");

    // The congestion passes and the window expires, which is the state the pre-fix link
    // never left: pool dead, every subsequent frame on the global heap.
    wait_out_the_window();
    fake_httpd::instance().set_queue_capacity(0);

    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});
    peer->send(std::span<const std::byte>(kBody));
    check_eq(link->tx_strands(), capacity, "all four stranded slots were reclaimed");
    check_eq(link->tx_slots_busy(), 1, "the pool is serving again: only the NEW send holds a slot");

    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, 1,
             "and that send was actually delivered");
    check_eq(link->tx_slots_busy(), 0, "the slot went back to the pool on the ordinary path");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — the sweep does not eat work that is merely queued.
// ---------------------------------------------------------------------------
/**
 * @brief The guard without which case 1 is satisfied by a link that discards its pool
 *        whenever it is busy.
 *
 * The pool is exhausted here by frames that are genuinely QUEUED and about to run, with no
 * wait in between. The exhausted claim still sweeps — that is its only trigger — and must
 * find every slot young, put each one back exactly as its claimer left it, and reclaim
 * nothing. The fifth frame then takes the heap fallback it always did, and all five are
 * delivered.
 */
void test_a_busy_pool_is_never_reclaimed() {
    std::printf("a pool exhausted by frames that are merely queued:\n");
    auto link = make_link();
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    const std::size_t capacity = httpd_ws_link_t::tx_slot_capacity();
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    // Fill the pool, then ask for one more IMMEDIATELY — the exhausted claim that sweeps.
    for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    check_eq(link->tx_slots_busy(), capacity, "the pool is fully claimed, all of it live");
    peer->send(std::span<const std::byte>(kBody));

    check_eq(link->tx_strands(), 0, "the sweep reclaimed NOTHING: every slot was young");
    check_eq(link->tx_slots_busy(), capacity, "and it put every one of them back");

    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, capacity + 1,
             "all five frames were delivered — the pooled four and the heap fallback");
    check_eq(link->tx_slots_busy(), 0, "the pool drained normally");

    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — a token that arrives after its slot was reclaimed.
// ---------------------------------------------------------------------------
/**
 * @brief The property that makes reclaiming safe at all, staged as a worst case.
 *
 * A pooled work item lives INSIDE its slot, so it cannot carry a generation the way a
 * session reference does — a re-claim overwrites the very field the check would read. It
 * is therefore treated as a bare token meaning "send whatever slot i has armed", and this
 * case makes every token in flight a stale one: four frames are queued for real, left to
 * go stale WITHOUT being drained, and then swept by a fifth send that re-claims one of
 * their slots. Draining afterwards runs five tokens against one armed payload.
 *
 * Exactly one frame must go out. Anything higher means a token sent a payload it did not
 * own — the reclaimed slots have had their overflow buffers freed and their payload
 * pointers cleared, so those sends are reads of a slot that belongs to somebody else.
 *
 * Note what this case also shows honestly: the four stale frames are DROPPED. They were
 * deliverable, and the sweep abandoned them. That is the designed trade — a slot armed
 * for longer than every drain latency the link can itself produce is presumed lost, and
 * frames are droppable by contract (#481) while a dead pool is not recoverable at all.
 */
void test_a_late_token_sends_the_current_frame_exactly_once() {
    std::printf("five tokens draining against one armed payload:\n");
    auto link = make_link();
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    const std::size_t capacity = httpd_ws_link_t::tx_slot_capacity();
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    check_eq(fake_httpd::instance().queue_depth(), capacity, "four real tokens are in the queue");

    // Let them go stale in the queue rather than binning them: every token below is one
    // that WILL arrive, which is the case a reclaim has to survive.
    wait_out_the_window();
    peer->send(std::span<const std::byte>(kBody));
    check_eq(link->tx_strands(), capacity, "the exhausted claim reclaimed all four stale slots");
    check_eq(fake_httpd::instance().queue_depth(), capacity + 1,
             "and all five tokens are still queued, about to run");

    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, 1,
             "exactly ONE frame went out: four tokens found nothing of theirs to send");
    check_eq(link->tx_slots_busy(), 0, "no slot was left claimed by a token that lost its race");

    // And the link is still fully serviceable afterwards — the pool was not corrupted by
    // the storm, which a double release would have shown up as here.
    const std::size_t sent_mid = fake_httpd::instance().frames_sent();
    for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    drain();
    check_eq(fake_httpd::instance().frames_sent() - sent_mid, capacity,
             "a full pool's worth of frames still goes out after the storm");

    reset(link);
}

// ---------------------------------------------------------------------------
// 4 — the same, with claims, reclaims and drains racing for real.
// ---------------------------------------------------------------------------
/**
 * @brief Several sending tasks and the httpd task, concurrently, over a queue small
 *        enough that a large share of the enqueues is binned.
 *
 * The three cases above are single-threaded: they pin the protocol's OUTCOMES but give a
 * sanitizer nothing to look at, and every transition here is lock-free. This one puts a
 * claim, a reclaim, a token's ownership CAS and a release on different threads at the same
 * time, which is the arrangement the design argument rests on and the one the tsan/asan CI
 * lanes can actually falsify.
 *
 * The post-condition is deliberately the strongest DETERMINISTIC one available, and it is
 * checked after everything has quiesced: once the storm is over, the window has expired,
 * and one more round has forced a sweep, not a single slot may still be claimed. A slot
 * wedged by a lost race — a token that released a slot it did not own, a claim that raced
 * a reclaim — shows up here as a pool that never comes back to zero.
 */
void test_concurrent_claims_reclaims_and_drains_wedge_nothing() {
    std::printf("senders, reclaims and the drain racing over a starved control queue:\n");
    auto link = make_link();
    claim(kFd);
    tr::net::transport_t* const peer = only_peer(*link);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    const std::size_t capacity = httpd_ws_link_t::tx_slot_capacity();
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    // Two entries deep: enough that work really drains, small enough that a great many
    // enqueues are binned while `httpd_queue_work` keeps reporting ESP_OK.
    fake_httpd::instance().set_queue_capacity(2);

    std::atomic<bool> stop{false};
    std::vector<std::thread> senders;
    for (int t = 0; t < 3; ++t) {
        senders.emplace_back([peer] {
            for (int i = 0; i < 300; ++i) peer->send(std::span<const std::byte>(kBody));
        });
    }
    std::thread drainer([&stop] {
        while (!stop.load(std::memory_order_relaxed)) (void)fake_httpd::instance().run_pending();
    });
    for (auto& s : senders) s.join();
    stop.store(true, std::memory_order_relaxed);
    drainer.join();
    drain();

    check(fake_httpd::instance().queue_drops() > 0,
          "the storm really did bin enqueues (otherwise this case proves nothing)");
    check(fake_httpd::instance().frames_sent() > sent_before, "and frames really did go out");

    // Quiesced. Expire the window, then force a sweep and let everything drain.
    wait_out_the_window();
    fake_httpd::instance().set_queue_capacity(0);
    for (std::size_t i = 0; i <= capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    drain();
    check_eq(link->tx_slots_busy(), 0, "every slot came back: nothing was wedged by the race");

    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link TX slot-strand host suite (#944):\n");
    test_the_pool_survives_four_binned_work_items();
    test_a_busy_pool_is_never_reclaimed();
    test_a_late_token_sends_the_current_frame_exactly_once();
    test_concurrent_claims_reclaims_and_drains_wedge_nothing();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
