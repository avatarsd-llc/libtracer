/**
 * @file
 * @brief #2095 — one conflating TX slot per peer: a slow WebSocket client loses its own
 *        older updates and never sets any other peer's rate.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 teardown, #835 send-stall and #954 session-identity
 * suites: the REAL chip translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`)
 * compiled against the host fake of `esp_http_server` (fake_httpd.hpp).
 *
 * It replaces the #949 pool, #1187 fan-out-wait and #1218 fan-out-width suites. Those pinned
 * a link-wide pool of TX slots that every peer shared, a bounded wait for one, and a blocking
 * socket write: together, the slowest peer set everyone's delivery rate. What is pinned now:
 *   1. a stalled peer and a healthy peer on one link: the healthy one receives every update,
 *      the stalled one's frame is replaced and counted, and no write waits on a socket;
 *   2. latest wins: a peer that recovers is sent the newest frame, not the one it stalled on;
 *   3. a frame a non-blocking write left part-way is finished on the next flush, byte-exact,
 *      and is not a desync;
 *   4. anything else written to that socket first finishes it (a close here);
 *   5. the link keeps ONE flush in the control queue, however many frames and peers;
 *   6. a refused flush is a counted drop and costs no frame storage;
 *   7. producers, refusals and the drain racing for real wedge nothing (the sanitizer lanes);
 *   8. the RAM: per-link and per-peer costs are reported and bind to the constructor sizes.
 */

#include <atomic>
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
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
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

constexpr int kStalled = 610; /**< @brief The peer whose socket takes nothing. */
constexpr int kHealthy = 611; /**< @brief The peer whose socket takes everything. */

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief A link that adopts the fake server, peer-named so each peer has a name. */
tr::mem::poly_ptr_t<httpd_ws_link_t> make_link(tr::net::httpd_ws_config_t config = {}) {
    config.peer_named = true;
    return tr::mem::make_poly<httpd_ws_link_t>(tr::mem::net_source(), handle(), "/ws", config);
}

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief A one-byte frame carrying @p tag, so the frame a peer received can be named. */
std::array<std::byte, 1> tagged(std::size_t tag) { return {static_cast<std::byte>(tag & 0xFF)}; }

/** @brief The frames the fake recorded as fully written to @p fd, in send order. */
std::vector<std::vector<std::byte>> frames_to(int fd) {
    std::vector<std::vector<std::byte>> out;
    for (const auto& f : fake_httpd::instance().sent_frames())
        if (f.fd == fd && f.type == HTTPD_WS_TYPE_BINARY) out.push_back(f.payload);
    return out;
}

/** @brief The `tx_conflated` count of the open session on @p fd's slot, read through the
 *         public per-peer stats (the peer named `p<slot>`; slots are claimed in order). */
std::uint32_t conflated(httpd_ws_link_t& link, std::size_t slot) {
    std::uint32_t n = 0;
    link.enumerate_peer_stats([&](const httpd_ws_link_t::peer_stats_t& p) {
        if (p.slot == slot) n = p.c.tx_conflated;
    });
    return n;
}

/** @brief Retire the link, the fake's sessions and its queue settings between cases. */
void reset(tr::mem::poly_ptr_t<httpd_ws_link_t>& link) {
    link.reset();
    fake_httpd::instance().set_queue_refusing(false);
    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().close_all();
    fake_httpd::instance().clear_sent_frames();
    drain();
}

// ---------------------------------------------------------------------------
// 1 — the acceptance case: a stalled peer and a healthy one, one link.
// ---------------------------------------------------------------------------
/**
 * @brief The healthy peer receives every update at full rate while the stalled peer's slot
 *        conflates, and neither the drain nor ingest waits on the stalled socket.
 *
 * "Waits" is measured where lwIP would spend it: a write that meets a full socket without
 * `MSG_DONTWAIT` parks the task for the whole send bound, and the fake counts exactly those
 * (`waited_writes`). Before #2095 every update to the stalled peer was one such write, so the
 * healthy peer was paced by the stalled one's bound.
 */
void test_a_stalled_peer_sets_only_its_own_rate() {
    std::printf("a stalled peer and a healthy peer on one link:\n");
    auto link = make_link();
    claim(kStalled);
    claim(kHealthy);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_sent_frames();
    const std::size_t waited_before = fake_httpd::instance().waited_writes(kStalled);

    constexpr std::size_t kUpdates = 32;
    for (std::size_t i = 0; i < kUpdates; ++i) {
        const auto frame = tagged(i);
        link->send(std::span<const std::byte>(frame));
        drain();
    }
    const auto healthy = frames_to(kHealthy);
    check_eq(healthy.size(), kUpdates, "the healthy peer received every update");
    bool in_order = healthy.size() == kUpdates;
    for (std::size_t i = 0; in_order && i < kUpdates; ++i)
        in_order = healthy[i].size() == 1 && healthy[i][0] == tagged(i)[0];
    check(in_order, "in publish order, each one intact");
    check_eq(frames_to(kStalled).size(), 0,
             "the stalled peer received nothing (its socket is full)");
    check_eq(fake_httpd::instance().waited_writes(kStalled) - waited_before, 0,
             "no write ever WAITED on the stalled socket");
    check_eq(conflated(*link, 0), kUpdates - 1,
             "every update but the newest was replaced, and counted on the stalled peer");
    check_eq(conflated(*link, 1), 0, "the healthy peer conflated nothing");
    check_eq(link->stats().tx_conflated, kUpdates - 1, "the link total agrees");
    check(link->drop_stats().dropped_tx >= kUpdates - 1, "and the generic drop seam sees them");
    check(fake_httpd::instance().has_session(kStalled),
          "a full socket is not a fault: the stalled peer is still connected");
    check_eq(link->stats().tx_send_failed, 0, "and nothing was counted as a failed send");
    check(fake_httpd::instance().deliver_frame(kHealthy, kBody) == ESP_OK,
          "ingest is served while the stalled peer holds a frame");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — latest wins.
// ---------------------------------------------------------------------------
/**
 * @brief A peer whose socket drains again is sent the newest frame, not the one it stalled
 *        on: a frame not one byte of which went out is replaced like a waiting one.
 */
void test_a_recovered_peer_gets_the_newest_frame() {
    std::printf("a stalled peer that recovers:\n");
    auto link = make_link();
    claim(kStalled);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_sent_frames();
    for (std::size_t i = 0; i < 5; ++i) {
        const auto frame = tagged(i);
        link->send(std::span<const std::byte>(frame));
        drain();
    }
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL});
    // The send-complete signal is event-driven: the next frame posted on the link is what
    // moves a held one on. No timer stands in for it.
    const auto last = tagged(5);
    link->send(std::span<const std::byte>(last));
    drain();
    const auto got = frames_to(kStalled);
    check_eq(got.size(), 1, "exactly one frame went out once the socket drained");
    check(got.size() == 1 && got[0].size() == 1 && got[0][0] == last[0],
          "and it is the NEWEST one");
    check_eq(conflated(*link, 0), 5, "the five it superseded are counted");

    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — a part-written frame is finished, byte-exact.
// ---------------------------------------------------------------------------
/** @brief A 64-byte payload, distinct per @p seed. */
std::array<std::byte, 64> payload(std::uint8_t seed) {
    std::array<std::byte, 64> p{};
    for (std::size_t i = 0; i < p.size(); ++i) p[i] = static_cast<std::byte>(seed + i);
    return p;
}

/**
 * @brief The socket takes the header and half the payload, then nothing more until the next
 *        flush. A blocking write would have counted that as the #951 desync and condemned the
 *        session; a non-blocking one owes the rest, and writes it before the next frame.
 */
void test_a_part_written_frame_is_finished_first() {
    std::printf("a frame the socket took only part of:\n");
    auto link = make_link();
    claim(kStalled);
    drain();
    const auto a = payload(0x10);
    const auto b = payload(0x80);
    fake_httpd::instance().set_send_script(
        kStalled, {send_result_t::FULL, send_result_t::SHORT, send_result_t::FULL});
    fake_httpd::instance().clear_wire(kStalled);
    link->send(std::span<const std::byte>(a));
    drain();
    check_eq(fake_httpd::instance().wire(kStalled).size(), 2 + a.size() / 2,
             "the header and half the payload are on the wire");
    check(fake_httpd::instance().has_session(kStalled), "and it is NOT a desync: still connected");
    link->send(std::span<const std::byte>(b));
    drain();
    const auto wire = fake_httpd::instance().wire(kStalled);
    std::vector<std::byte> want(2, std::byte{0});  // the fake writes IDF's header as zeroes
    want.insert(want.end(), a.begin(), a.end());
    want.insert(want.end(), 2, std::byte{0});
    want.insert(want.end(), b.begin(), b.end());
    check(wire == want, "the rest of A went out, then all of B: the stream still parses");
    check_eq(link->stats().sessions_condemned, 0, "no session was condemned");
    check_eq(conflated(*link, 0), 0, "a frame part-way on the wire is never replaced");

    reset(link);
}

/**
 * @brief Anything else written to the socket finishes the part-written frame first — here
 *        the administrative close, whose code would otherwise land inside the frame.
 */
void test_a_close_finishes_the_part_written_frame() {
    std::printf("a close behind a part-written frame:\n");
    auto link = make_link();
    claim(kStalled);
    drain();
    const auto a = payload(0x20);
    fake_httpd::instance().set_send_script(
        kStalled, {send_result_t::FULL, send_result_t::SHORT, send_result_t::FULL});
    fake_httpd::instance().clear_wire(kStalled);
    link->send(std::span<const std::byte>(a));
    drain();
    std::string name;
    link->enumerate_peers([&name](std::string_view p) { name = std::string(p); });
    check(link->close_peer(name), "the close was accepted");
    drain();
    const auto wire = fake_httpd::instance().wire(kStalled);
    check(wire.size() >= 2 + a.size() && std::equal(a.begin(), a.end(), wire.begin() + 2),
          "the frame was completed before the close frame was written");
    check_eq(wire.size(), 2 + a.size() + 2 + 2, "followed by exactly the close frame");

    reset(link);
}

// ---------------------------------------------------------------------------
// 5 — one flush per link in the shared control queue.
// ---------------------------------------------------------------------------
/**
 * @brief A burst to several peers posts ONE control-queue item, and each peer gets the newest
 *        frame of the burst. The pool posted one item per frame per peer into a six-deep mbox
 *        it shared with the closes `condemn` needs landed.
 */
void test_one_flush_per_link() {
    std::printf("a burst of eight frames to three peers:\n");
    auto link = make_link();
    constexpr int kFds[] = {620, 621, 622};
    for (const int fd : kFds) claim(fd);
    drain();
    fake_httpd::instance().clear_sent_frames();
    for (std::size_t i = 0; i < 8; ++i) {
        const auto frame = tagged(i);
        link->send(std::span<const std::byte>(frame));
    }
    check_eq(fake_httpd::instance().queue_depth(), 1, "one flush in the control queue");
    drain();
    bool newest = true;
    for (const int fd : kFds) {
        const auto got = frames_to(fd);
        newest = newest && got.size() == 1 && got[0][0] == tagged(7)[0];
    }
    check(newest, "each peer received the burst's newest frame, once");
    check_eq(link->stats().tx_conflated, 3 * 7, "the seven older frames per peer are counted");

    reset(link);
}

// ---------------------------------------------------------------------------
// 6 — a refused flush.
// ---------------------------------------------------------------------------
/** @brief A control queue that refuses the flush drops the frame it would have carried,
 *         counted on the link, and the link serves as soon as the queue takes work again. */
void test_a_refused_flush_is_a_counted_drop() {
    std::printf("a control queue that refuses the flush:\n");
    auto link = make_link();
    claim(kHealthy);
    drain();
    fake_httpd::instance().clear_sent_frames();
    const std::uint32_t drops_before = link->stats().enqueue_drops;
    fake_httpd::instance().set_queue_refusing(true);
    for (int i = 0; i < 16; ++i) link->send(std::span<const std::byte>(kBody));
    check_eq(link->stats().enqueue_drops - drops_before, 16, "every refused frame is counted");
    check_eq(conflated(*link, 0), 0, "and none of them was left held to be conflated");
    fake_httpd::instance().set_queue_refusing(false);
    const auto frame = tagged(9);
    link->send(std::span<const std::byte>(frame));
    drain();
    const auto got = frames_to(kHealthy);
    check(got.size() == 1 && got[0][0] == frame[0], "the next frame goes out");

    reset(link);
}

// ---------------------------------------------------------------------------
// 7 — producers, refusals and the drain racing for real.
// ---------------------------------------------------------------------------
/**
 * @brief Several producers and the httpd task concurrently, over a two-deep control queue
 *        and one stalled peer. The post-condition is deterministic: once everything has
 *        quiesced, a final frame reaches every healthy peer, so no flag or frame was wedged
 *        by a lost race.
 */
void test_producers_and_the_drain_race_without_wedging() {
    std::printf("producers, refusals and the drain racing:\n");
    auto link = make_link();
    claim(kStalled);
    claim(kHealthy);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().set_queue_capacity(2);

    std::atomic<bool> stop{false};
    std::vector<std::thread> producers;
    httpd_ws_link_t* const raw = link.get();
    for (int t = 0; t < 3; ++t)
        producers.emplace_back([raw] {
            for (int i = 0; i < 300; ++i) raw->send(std::span<const std::byte>(kBody));
        });
    std::thread drainer([&stop] {
        while (!stop.load(std::memory_order_relaxed)) (void)fake_httpd::instance().run_pending();
    });
    for (auto& p : producers) p.join();
    stop.store(true, std::memory_order_relaxed);
    drainer.join();
    drain();

    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().clear_sent_frames();
    const auto last = tagged(0x5A);
    link->send(std::span<const std::byte>(last));
    drain();
    const auto got = frames_to(kHealthy);
    check(got.size() == 1 && got[0][0] == last[0], "the healthy peer still receives");
    check(fake_httpd::instance().has_session(kStalled), "the stalled peer is still connected");

    reset(link);
}

// ---------------------------------------------------------------------------
// 8 — the RAM, and that the sizes bind.
// ---------------------------------------------------------------------------
/** @brief The per-link and per-peer costs are reported, and a sized link reports and uses
 *         its own sizes. */
void test_the_ram_is_reported_and_binds() {
    std::printf("per-link and per-peer TX RAM:\n");
    {
        auto stock = make_link();
        check_eq(stock->tx_inline_bytes(), httpd_ws_link_t::kDefaultTxInlineBytes,
                 "a stock link takes the default inline capacity");
        check_eq(stock->buffer_bytes(),
                 httpd_ws_link_t::kDefaultRxScratchBytes + httpd_ws_link_t::kDefaultTxInlineBytes,
                 "per link: the RX scratch and one reply scratch");
        check(stock->peer_tx_bytes() >= 2 * httpd_ws_link_t::kDefaultTxInlineBytes,
              "per peer: two frames' inline buffers and their bookkeeping");
        std::printf("    buffer_bytes=%zu peer_tx_bytes=%zu\n", stock->buffer_bytes(),
                    stock->peer_tx_bytes());
        reset(stock);
    }
    constexpr std::size_t kRx = 512;
    constexpr std::size_t kInline = 256;
    auto link = make_link({.rx_scratch_bytes = kRx, .tx_inline_bytes = kInline});
    check_eq(link->rx_scratch_bytes(), kRx, "a sized link reports its RX scratch");
    check_eq(link->tx_inline_bytes(), kInline, "and its inline capacity");
    check_eq(link->buffer_bytes(), kRx + kInline, "and the per-link cost they make");
    check(link->peer_tx_bytes() >= 2 * kInline && link->peer_tx_bytes() < 2 * kInline + 512,
          "and the per-peer cost they make");
    claim(kHealthy);
    drain();
    fake_httpd::instance().clear_sent_frames();
    std::vector<std::byte> big(kInline + 1, std::byte{0x33});
    link->send(std::span<const std::byte>(big));
    drain();
    const auto got = frames_to(kHealthy);
    check(got.size() == 1 && got[0].size() == big.size(),
          "a frame one byte past the inline buffer still goes out (from memory.io)");

    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link per-peer conflating TX slot host suite (#2095):\n");
    test_a_stalled_peer_sets_only_its_own_rate();
    test_a_recovered_peer_gets_the_newest_frame();
    test_a_part_written_frame_is_finished_first();
    test_a_close_finishes_the_part_written_frame();
    test_one_flush_per_link();
    test_a_refused_flush_is_a_counted_drop();
    test_producers_and_the_drain_race_without_wedging();
    test_the_ram_is_reported_and_binds();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
