/**
 * @file
 * @brief #1218 — the in-call reserve must not shrink the STEADY-STATE fan-out width: a
 *        sweep that fits `tx_slot_capacity()` still fits it, pass after pass, without ever
 *        waiting for the drain.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #949 TX-pool, #961 fan-out and #1187 wait suites: the REAL chip
 * translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against the
 * host fake of `esp_http_server` (fake_httpd.hpp).
 *
 * WHAT THIS SUITE ADDS over the #1187 wait suite next door. That one asks what a fan-out
 * WIDER than the pool does, and answers "it costs latency". This one asks what a fan-out
 * that FITS the pool does — the case that was never supposed to change — and pins that it
 * still costs nothing. Carving the in-call reserve out of the pool made those two questions
 * the same question: a sweep of exactly `tx_slot_capacity()` destinations could only claim
 * one slot short of it, so the last destination of EVERY pass had to wait for a drain that,
 * on a unicore target, cannot happen while the producer is running. The wait then expired,
 * dropped, and latched — so that destination starved permanently while the producer paid a
 * full send bound of stall per latch cycle, which is the reported shape (#1218).
 *
 * The scheduling model is the unicore one, and it is the whole point: the httpd task (this
 * suite's MAIN thread — `claim` latches it) runs ONLY between passes, never during one. A
 * producer thread issues each pass alone; whatever it cannot claim while it runs, it cannot
 * be handed mid-pass by anyone.
 *
 * The properties pinned here:
 *   1. a repeated fan-out of exactly `tx_slot_capacity()` reaches EVERY peer on EVERY pass,
 *      at one frame per peer per pass — no starved tail, no halved rate — and does it
 *      without a single `tx_pool_waits`, because a pass that fits must never wait;
 *   2. the pool's full depth is claimable by off-task producers WHILE an in-call reply holds
 *      its reserve — the reserve is additional, not carved out;
 * The guarantee the reserve was introduced FOR — that a delivery burst cannot starve a
 * request's reply — keeps its home in the #1187 suite next door, restated there on this
 * arithmetic (producers now hold the pool's whole depth, and the reply still lands).
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

/**
 * @brief The per-socket send bound this suite's links take, milliseconds.
 *
 * Explicit, and small, for one reason: it is what the TX wait bound is DERIVED from, so it
 * is also how long a send that cannot be served stalls its producer. Against the defect this
 * suite reproduces, every pass pays that stall; pinning it keeps the red run quick without
 * changing anything about what makes it red.
 */
constexpr std::uint32_t kSendTimeoutMs = 100;

/** @brief A link that adopts the fake server, with the bound above. */
std::unique_ptr<httpd_ws_link_t> make_link() {
    return std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true, kSendTimeoutMs);
}

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/**
 * @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim).
 *
 * It also LATCHES the calling thread as the link's httpd task, which makes the main thread
 * this suite's "httpd task" and any `std::thread` a producer.
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

/** @brief Retire the link, the fake's sessions and its queue settings between cases. */
void reset(std::unique_ptr<httpd_ws_link_t>& link) {
    drain();
    link.reset();
    fake_httpd::instance().set_queue_refusing(false);
    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().close_all();
    drain();
}

/** @brief Publish passes the steady-state case runs — enough that a starved destination is
 *         a flat zero against a healthy peer's count, not a timing coincidence. */
constexpr std::size_t kPasses = 5;

// ---------------------------------------------------------------------------
// 1 — a fan-out that FITS the pool keeps fitting it, pass after pass.
// ---------------------------------------------------------------------------
/**
 * @brief The regression: width == `tx_slot_capacity()`, from a producer task, repeatedly.
 *
 * Each pass is issued by a producer thread while the httpd task (the main thread) is doing
 * nothing at all — the unicore ordering — and is drained only after that thread has joined.
 * So the pass has exactly the pool it can claim in one go, and nothing more can arrive
 * mid-pass. Every peer must be written one frame per pass: equal counts, no drops, and no
 * halved rate for the survivors.
 *
 * `tx_pool_waits` is the sharpest of the assertions and the one that names the defect: a
 * sweep at the documented outstanding-send bound must not meet a full pool AT ALL. Waiting
 * even once here means a claimer was denied a slot the bound says it owns, and on silicon
 * that wait is what expires, drops, latches and starves the tail.
 */
void test_a_sweep_at_the_pool_width_never_waits_and_never_starves() {
    std::printf("a fan-out of exactly tx_slot_capacity(), pass after pass:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");

    const std::size_t width = httpd_ws_link_t::kDefaultTxPoolSlots;
    std::vector<int> fds;
    for (std::size_t i = 0; i < width; ++i) {
        fds.push_back(2100 + static_cast<int>(i));
        claim(fds.back());
    }
    drain();
    check_eq(fds.size(), width, "one peer per claimable TX slot is open");

    std::vector<std::size_t> writes_before;
    for (const int fd : fds) writes_before.push_back(fake_httpd::instance().writes(fd));
    const std::uint32_t drops_before = link->enqueue_drops();
    const std::uint32_t waits_before = link->stats().tx_pool_waits;

    for (std::size_t pass = 0; pass < kPasses; ++pass) {
        // One pass, alone on the CPU: the drain runs only once the producer is done, which
        // is what a unicore chip does to a publisher that never yields mid-sweep.
        std::thread producer([&link] { link->send(std::span<const std::byte>(kBody)); });
        producer.join();
        drain();
    }

    // Two writes per frame: httpd_ws_send_frame_async puts a frame on the socket as a header
    // write and a payload write (fake_httpd transcribes that).
    std::size_t served = 0;
    for (std::size_t i = 0; i < fds.size(); ++i) {
        const std::size_t w = fake_httpd::instance().writes(fds[i]) - writes_before[i];
        if (w == 2 * kPasses) ++served;
    }
    check_eq(served, width, "every peer took one frame per pass — same rate, none starved");
    check_eq(link->enqueue_drops() - drops_before, 0, "and not one frame was dropped");
    check_eq(link->stats().tx_pool_waits - waits_before, 0,
             "a sweep at the outstanding-send bound never met a full pool");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — the reserve is ADDITIONAL: a held reply does not narrow the pool.
// ---------------------------------------------------------------------------
/**
 * @brief The arithmetic the fix rests on, stated directly.
 *
 * An in-call send takes the reserve; producers must STILL be able to claim the pool's full
 * depth behind it. Nothing drains during the case, so every claim stays held and the busy
 * count is the whole statement: capacity claimable by producers, plus the reserve, is what
 * the link holds — and only then does a producer miss.
 */
void test_an_in_call_reply_does_not_narrow_the_pool() {
    std::printf("producers against a pool while an in-call reply holds its reserve:\n");
    auto link = make_link();
    claim(2200);
    tr::net::transport_t* const peer = peer_of(*link, 0);
    check(peer != nullptr, "the peer resolved to a directed endpoint");
    if (peer == nullptr) return;
    drain();

    const std::size_t capacity = httpd_ws_link_t::kDefaultTxPoolSlots;
    const std::size_t reserve = httpd_ws_link_t::tx_reply_reserve();
    check(reserve >= 1, "the link keeps an in-call reserve at all");

    // In-call first (the main thread is the latched httpd task): it takes the reserve, so
    // the pool behind it must be untouched.
    peer->send(std::span<const std::byte>(kBody));
    check_eq(link->tx_slots_busy(), 1, "the in-call reply is holding one slot");

    const std::uint32_t drops_before = link->enqueue_drops();
    std::thread producer([peer, capacity] {
        for (std::size_t i = 0; i < capacity; ++i) peer->send(std::span<const std::byte>(kBody));
    });
    producer.join();
    check_eq(link->tx_slots_busy(), capacity + 1,
             "producers still claimed the pool's FULL depth behind the reply");
    check_eq(link->enqueue_drops() - drops_before, 0, "none of them was dropped");

    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link fan-out WIDTH suite (#1218):\n");
    test_a_sweep_at_the_pool_width_never_waits_and_never_starves();
    test_an_in_call_reply_does_not_narrow_the_pool();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
