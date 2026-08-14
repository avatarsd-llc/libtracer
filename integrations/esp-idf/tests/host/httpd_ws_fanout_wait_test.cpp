/**
 * @file
 * @brief #1187 — a fan-out wider than the TX pool costs LATENCY, not its tail: the pool is
 *        an in-flight bound, and an off-httpd-task send waits for the drain instead of
 *        dropping the same publish-order prefix every pass.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #949 TX-pool and #961 fan-out suites: the REAL chip translation
 * unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against the host fake
 * of `esp_http_server` (fake_httpd.hpp).
 *
 * WHAT THIS SUITE HAD TO MODEL, and why the two suites next door could not. The defect is a
 * SCHEDULING fact, not an arithmetic one: on a unicore chip the producer task posts its
 * whole publish sweep before the httpd task can run once, so no slot ever frees mid-pass
 * and exactly the first `tx_slot_capacity()` destinations win — every pass, in publish
 * order, with the rest of the peer set at a flat zero (12 edges, 5 alive, reproduced
 * bit-identically on two boards). A host has many cores and a drainer thread that runs
 * whenever it likes, which would deliver the whole fan-out on the OLD code too and prove
 * nothing.
 *
 * So the drainer here is not free-running: it drains only once the producer has ENTERED a
 * wait, which it reads from the link's own `tx_pool_waits` counter. That is the unicore
 * ordering exactly — the task that frees slots runs only while the producer is off the CPU
 * — and it makes both directions deterministic. Against the old code the producer never
 * waits, so the drainer never runs, and a 12-peer sweep serves four; against the fixed one
 * every stall is broken by a drain and all twelve land.
 *
 * The four properties pinned here:
 *   1. a fan-out wider than the pool reaches EVERY open peer exactly once when the drain is
 *      allowed to run — and the waits counter says the pool really was the reason;
 *   2. the same sweep issued ON the httpd task still stops at the pool's depth, because the
 *      task it would wait for is the one asking. The honest bound, stated as a test rather
 *      than only in a doc comment;
 *   3. an in-call send ALWAYS finds the reserved slot, even with the whole pool claimed by
 *      producers — the secondary symptom in #1187, where a subscribe ack lost its race with
 *      a delivery burst and the requester timed out against a live edge. The reserve is a
 *      slot PAST the pool since #1218, so this costs a producer nothing;
 *   4. a wait that cannot be served EXPIRES, drops and counts. The fix must not turn a
 *      counted loss into an unbounded park (ADR-0081 §1).
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

/** @brief A link that adopts the fake server. */
std::unique_ptr<httpd_ws_link_t> make_link() {
    return std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
}

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/**
 * @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim).
 *
 * It also LATCHES the calling thread as the link's httpd task, which is what makes the
 * main thread this suite's "httpd task" and any `std::thread` a producer. Every case
 * below depends on that split.
 */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief The directed endpoint of the peer on @p fd — resolved by slot name. */
tr::net::transport_t* peer_of(httpd_ws_link_t& link, std::size_t index) {
    std::vector<std::string> names;
    link.enumerate_peers([&names](std::string_view p) { names.emplace_back(p); });
    return index < names.size() ? link.peer_link(names[index]) : nullptr;
}

/** @brief Broadcast one frame to every open peer. */
void broadcast(httpd_ws_link_t& link) { link.send(std::span<const std::byte>(kBody)); }

/** @brief Retire the link, the fake's sessions and its queue settings between cases. */
void reset(std::unique_ptr<httpd_ws_link_t>& link) {
    drain();
    link.reset();
    fake_httpd::instance().set_queue_refusing(false);
    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().close_all();
    drain();
}

/** @brief Peers a wide sweep addresses — three times the pool, as the report's node had. */
constexpr std::size_t kWidePeers = 12;

/**
 * @brief The drain that models a UNICORE httpd task: it runs only while the producer is
 *        blocked, which the link's own `tx_pool_waits` counter is the signal for.
 *
 * Started before the sweep and stopped after it. Every increment of the counter is one
 * producer stall, and each is answered with a full drain — so slots free exactly when the
 * producer is off the CPU and never while it is running, which is the ordering that makes
 * the defect deterministic on silicon.
 */
class waited_drainer_t {
   public:
    explicit waited_drainer_t(httpd_ws_link_t& link) {
        thread_ = std::thread([this, &link] {
            std::uint32_t served = 0;
            while (!stop_.load(std::memory_order_relaxed)) {
                const std::uint32_t waits = link.stats().tx_pool_waits;
                if (waits != served) {
                    served = waits;
                    drain();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }
    ~waited_drainer_t() {
        stop_.store(true, std::memory_order_relaxed);
        thread_.join();
    }
    waited_drainer_t(const waited_drainer_t&) = delete;
    waited_drainer_t& operator=(const waited_drainer_t&) = delete;

   private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

// ---------------------------------------------------------------------------
// 1 — a wide sweep from a producer task loses nothing.
// ---------------------------------------------------------------------------
/**
 * @brief The headline: twelve peers, a pool of four, and every peer served exactly once.
 *
 * The sweep runs on a producer thread (not the latched httpd task) against the
 * wait-gated drainer above, so the only way a peer past the pool can be served is for the
 * producer to have waited for a slot. Two counters make it non-vacuous: not one frame may
 * be dropped, and `tx_pool_waits` must have moved — a link that somehow served twelve
 * peers without ever finding the pool full would not be exercising this path at all.
 */
void test_a_wide_sweep_from_a_producer_task_reaches_every_peer() {
    std::printf("a twelve-peer sweep from a producer task, drained only while it waits:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");

    std::vector<int> fds;
    for (std::size_t i = 0; i < kWidePeers; ++i) {
        fds.push_back(1100 + static_cast<int>(i));
        claim(fds.back());
    }
    drain();
    check_eq(fds.size(), kWidePeers, "every peer of the sweep is open");

    std::vector<std::size_t> writes_before;
    for (const int fd : fds) writes_before.push_back(fake_httpd::instance().writes(fd));
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    const std::uint32_t drops_before = link->enqueue_drops();

    {
        const waited_drainer_t drainer(*link);
        std::thread producer([&link] { broadcast(*link); });
        producer.join();
        drain();
    }

    check_eq(fake_httpd::instance().frames_sent() - sent_before, kWidePeers,
             "every peer of the sweep reached the wire");
    check_eq(link->enqueue_drops() - drops_before, 0, "and not one frame was dropped");
    // Two writes per frame: httpd_ws_send_frame_async puts a frame on the socket as a
    // header write and a payload write (fake_httpd transcribes that).
    std::size_t served = 0;
    for (std::size_t i = 0; i < fds.size(); ++i) {
        const std::size_t w = fake_httpd::instance().writes(fds[i]) - writes_before[i];
        if (w == 2) ++served;
    }
    check_eq(served, kWidePeers, "each peer was written exactly one frame — no tail at zero");
    check(link->stats().tx_pool_waits > 0,
          "the sweep really did meet a full pool and wait (otherwise the case is vacuous)");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — the same sweep ON the httpd task still stops at the pool's depth.
// ---------------------------------------------------------------------------
/**
 * @brief The bound that remains, pinned rather than merely documented.
 *
 * A push provoked by an inbound frame is serviced IN-CALL on the httpd task, and that task
 * is the one that would have to drain the pool — so it cannot wait, and a sweep issued from
 * there still cannot put more than the slots it can claim in flight at once. Making that a
 * test is what stops a later reader from assuming the wait covers every path.
 *
 * What it can claim is the pool PLUS the in-call reserve (#1218): the reserve is a slot of
 * its own past `tx_slot_capacity()`, and the in-call sender is the one claimer entitled to
 * it. The depth is what is pinned here, not which slot served which frame.
 */
void test_an_in_call_sweep_still_stops_at_the_pool_depth() {
    std::printf("the same sweep issued ON the httpd task:\n");
    auto link = make_link();
    std::vector<int> fds;
    for (std::size_t i = 0; i < kWidePeers; ++i) {
        fds.push_back(1200 + static_cast<int>(i));
        claim(fds.back());
    }
    drain();

    const std::size_t capacity = httpd_ws_link_t::kDefaultTxPoolSlots;
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    const std::uint32_t drops_before = link->enqueue_drops();
    broadcast(*link);  // the main thread IS the latched httpd task (see claim)
    drain();

    const std::size_t in_call_depth = capacity + httpd_ws_link_t::tx_reply_reserve();
    check_eq(fake_httpd::instance().frames_sent() - sent_before, in_call_depth,
             "the pool plus the in-call reserve reached the wire — the depth still bounds it");
    check_eq(link->enqueue_drops() - drops_before, kWidePeers - in_call_depth,
             "and the rest were dropped and counted, never parked");

    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — an in-call reply always finds the reserved slot.
// ---------------------------------------------------------------------------
/**
 * @brief #1187's secondary effect: a subscribe ack lost to a delivery burst.
 *
 * The reserve is the httpd task's, because it is the one claimer that cannot wait. Here a
 * producer thread claims everything it is allowed to (nothing drains, so the slots stay
 * held) and the in-call send must STILL be queued. The second in-call send must then miss —
 * the reserve is one slot, not an escape hatch, and without that half the case would also
 * pass on a link with no bound at all.
 *
 * Since #1218 "everything it is allowed to" is the pool's WHOLE depth: the reserve sits past
 * `tx_slot_capacity()` rather than inside it, so this guarantee no longer costs a producer
 * the slot it was promised. That is what makes the case stronger than it was — the burst it
 * survives is now a full-depth one.
 */
void test_an_in_call_send_always_finds_the_reserved_slot() {
    std::printf("an in-call reply against a pool claimed by producers:\n");
    auto link = make_link();
    claim(1300);
    tr::net::transport_t* const peer = peer_of(*link, 0);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();

    const std::size_t capacity = httpd_ws_link_t::kDefaultTxPoolSlots;
    // Exactly what a producer is allowed: the pool, in full. Nothing drains, so each claim
    // is still held when the next one runs, and none of them has to wait.
    std::thread producer([peer, capacity] {
        for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    });
    producer.join();
    check_eq(link->tx_slots_busy(), capacity, "the producers hold every slot the pool offers");

    const std::uint32_t drops_before = link->enqueue_drops();
    peer->send(std::span<const std::byte>(kBody));  // in-call: the main thread is the httpd task
    check_eq(link->tx_slots_busy(), capacity + httpd_ws_link_t::tx_reply_reserve(),
             "the in-call send took the reserved slot");
    check_eq(link->enqueue_drops() - drops_before, 0, "and was not dropped");

    peer->send(std::span<const std::byte>(kBody));
    check_eq(link->enqueue_drops() - drops_before, 1,
             "a second in-call send found pool and reserve genuinely full, and was counted");

    reset(link);
}

// ---------------------------------------------------------------------------
// 4 — a wait that cannot be served expires, drops and counts.
// ---------------------------------------------------------------------------
/**
 * @brief The bound on the fix itself.
 *
 * With every slot held and NOTHING draining, a producer's send must not park: it waits its
 * bound, gives up, and is counted exactly as a pool miss always was (ADR-0081 §1 — an
 * unservable frame is honest, visible loss, never a library-held one). The generous ceiling
 * below is not the bound being asserted — it is the difference between "bounded" and
 * "hung", which is what a test can meaningfully say about a derived duration.
 */
void test_an_unservable_wait_expires_and_counts() {
    std::printf("a producer's send against a pool nothing will drain:\n");
    auto link = make_link();
    claim(1400);
    tr::net::transport_t* const peer = peer_of(*link, 0);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();

    // Every slot the link has: the pool AND the in-call reserve (#1218). These sends are
    // in-call, so they may take both — and they must, or the producer below would simply
    // find the reserve free and never wait at all.
    const std::size_t depth =
        httpd_ws_link_t::kDefaultTxPoolSlots + httpd_ws_link_t::tx_reply_reserve();
    for (std::size_t i = 0; i < depth; ++i) peer->send(std::span<const std::byte>(kBody));
    check_eq(link->tx_slots_busy(), depth, "the pool is fully claimed and nothing is draining");

    const std::uint32_t drops_before = link->enqueue_drops();
    const std::uint32_t waits_before = link->stats().tx_pool_waits;
    const auto started = std::chrono::steady_clock::now();
    std::thread producer([peer] { peer->send(std::span<const std::byte>(kBody)); });
    producer.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    check(elapsed < std::chrono::seconds(5), "the send returned within a bound, it did not hang");
    check(link->stats().tx_pool_waits > waits_before, "it did wait for the drain");
    check_eq(link->enqueue_drops() - drops_before, 1,
             "and the frame it could not send was counted");
    check_eq(link->stats().tx_pool_misses > 0 ? 1 : 0, 1, "as a POOL MISS specifically");

    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link wide-fan-out wait suite (#1187):\n");
    test_a_wide_sweep_from_a_producer_task_reaches_every_peer();
    test_an_in_call_sweep_still_stops_at_the_pool_depth();
    test_an_in_call_send_always_finds_the_reserved_slot();
    test_an_unservable_wait_expires_and_counts();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
