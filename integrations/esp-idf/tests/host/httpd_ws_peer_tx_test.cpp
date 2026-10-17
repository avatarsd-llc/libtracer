/**
 * @file
 * @brief #2095 / #2195 — one bounded, in-order TX queue per peer: a slow WebSocket client
 *        is refused past its own queue's depth and never sets any other peer's rate.
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
 *      the stalled one's past its queue's depth are refused and counted, and no write waits
 *      on a socket;
 *   2. a peer that recovers is sent every frame it held, oldest first;
 *   3. a frame a non-blocking write left part-way is finished on the next flush, byte-exact,
 *      and is not a desync;
 *   4. anything else written to that socket first finishes it (a close here);
 *   5. the link keeps ONE flush in the control queue, however many frames and peers;
 *   6. a refused flush is a counted drop and costs no frame storage;
 *   7. producers, refusals and the drain racing for real wedge nothing (the sanitizer lanes);
 *   8. the RAM: per-link and per-peer costs are reported and bind to the constructor sizes;
 *   9. a producer that posts once and goes quiet: the newest frame still arrives whole, moved
 *      by the link's own events (a write that took bytes, an inbound frame, flush_held);
 *  10. a burst past the full queue into a shut TCP window is counted and never a strike
 *      (#2195), but a peer that pins the large class's only block is struck (refused
 *      draws count against it);
 *  11. producers racing session close and reclaim on the httpd task (the TSan lane);
 *  12. latest wins per PATH, never across paths (#2195): two vertices written back to back
 *      on a busy wire both deliver, one path still conflates to its newest value (counted in
 *      `tx_conflated`), frames from no vertex are never replaced, and a frame offered to a
 *      full queue is refused and counted;
 *  13. the lanes' refusal paths: a SAMPLE still replaces in a full queue while a new path is
 *      refused, a full best-effort STREAM sheds its oldest (at its depth or at the peer's
 *      cap), a full reliable STREAM refuses the newest, and a refused node draw is a counted
 *      refusal.
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
#include "libtracer/egress.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/value.hpp"
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
    // One on the wire and kTxWaitDepth behind it are held; every later one is refused at the
    // full queue and counted, never put in a held frame's place (#2195).
    constexpr std::size_t kRefused = kUpdates - 1 - httpd_ws_link_t::kTxWaitDepth;
    check_eq(link->stats().enqueue_drops, kRefused,
             "every update past the stalled peer's queue was refused, and counted");
    check_eq(link->stats().tx_conflated, 0, "none replaced another");
    check(link->drop_stats().dropped_tx >= kRefused, "and the generic drop seam sees them");
    check(fake_httpd::instance().has_session(kStalled),
          "a full socket is not a fault: the stalled peer is still connected");
    check_eq(link->stats().tx_send_failed, 0, "and nothing was counted as a failed send");
    check(fake_httpd::instance().deliver_frame(kHealthy, kBody) == ESP_OK,
          "ingest is served while the stalled peer holds a frame");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — a recovered peer gets what it held, in order.
// ---------------------------------------------------------------------------
/**
 * @brief A peer whose socket drains again is sent every frame it held, oldest first: no
 *        frame stands in for another (#2195).
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
    // The next frame posted on the link moves the held one on (case 9 covers a link that
    // posts nothing more). No timer stands in for it.
    const auto last = tagged(5);
    link->send(std::span<const std::byte>(last));
    drain();
    const auto got = frames_to(kStalled);
    check_eq(got.size(), 6, "every held frame went out once the socket drained");
    bool in_order = got.size() == 6;
    for (std::size_t i = 0; in_order && i < 6; ++i)
        in_order = got[i].size() == 1 && got[i][0] == tagged(i)[0];
    check(in_order, "oldest first, each one intact");
    check_eq(conflated(*link, 0), 0, "and none was replaced");

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
 * @brief The socket takes the header and half the payload, then nothing (the re-post that a
 *        partial write makes finds it full). A blocking write would have counted that as the
 *        #951 desync and condemned the session; a non-blocking one owes the rest, and writes
 *        it before the next frame.
 */
void test_a_part_written_frame_is_finished_first() {
    std::printf("a frame the socket took only part of:\n");
    auto link = make_link();
    claim(kStalled);
    drain();
    const auto a = payload(0x10);
    const auto b = payload(0x80);
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL, send_result_t::SHORT,
                                                      send_result_t::TIMEOUT, send_result_t::FULL});
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
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL, send_result_t::SHORT,
                                                      send_result_t::TIMEOUT, send_result_t::FULL});
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
 * @brief A burst to several peers posts ONE control-queue item, and each peer gets every
 *        frame of the burst, in order. The pool posted one item per frame per peer into a six-deep
 * mbox it shared with the closes `condemn` needs landed.
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
    bool all = true;
    for (const int fd : kFds) {
        const auto got = frames_to(fd);
        all = all && got.size() == 8;
        for (std::size_t i = 0; all && i < 8; ++i) all = got[i][0] == tagged(i)[0];
    }
    check(all, "each peer received all eight, in order, once");
    check_eq(link->stats().tx_conflated, 0, "none replaced another");

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
        check(stock->peer_tx_bytes() <= 96,
              "per peer: the wire frame and one waiting frame by reference, with their lanes, "
              "no copy (a few words, whatever the frame)");
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
    check(link->peer_tx_bytes() <= 96, "and the per-peer cost does not grow with them");
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

// ---------------------------------------------------------------------------
// 9 — a producer that posts once and goes quiet.
// ---------------------------------------------------------------------------
/**
 * @brief Nothing more is posted after the one frame, and it still reaches the slow peer
 *        whole. ESP-IDF's server selects for read only, so what moves a held frame is the
 *        link's own events: (a) a write that took bytes re-posts the flush, so a socket that
 *        drains piecemeal is written to the end; (b) an inbound frame from any peer; (c) the
 *        application's flush_held() on its own tick, for a link with no traffic at all.
 */
void test_a_quiet_producer_still_delivers_the_newest_frame() {
    std::printf("one frame, then a quiet producer:\n");
    auto link = make_link();
    claim(kStalled);
    claim(kHealthy);
    drain();
    const auto a = payload(0x40);
    std::vector<std::byte> want(2, std::byte{0});  // the fake writes IDF's header as zeroes
    want.insert(want.end(), a.begin(), a.end());

    // (a) the socket takes a part on every pass: half, a quarter, an eighth, then the rest.
    fake_httpd::instance().set_send_script(
        kStalled, {send_result_t::FULL, send_result_t::SHORT, send_result_t::SHORT,
                   send_result_t::SHORT, send_result_t::FULL});
    fake_httpd::instance().clear_wire(kStalled);
    {
        const std::span<const std::byte> one[] = {std::span<const std::byte>(a)};
        link->peer_link("p0")->send(std::span<const std::span<const std::byte>>(one));
    }
    drain();
    check(fake_httpd::instance().wire(kStalled) == want,
          "(a) a socket that drains piecemeal gets the whole frame with nothing more posted");

    // (b) the socket takes nothing, then drains; the next event is a frame from ANOTHER peer.
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_wire(kStalled);
    {
        const std::span<const std::byte> one[] = {std::span<const std::byte>(a)};
        link->peer_link("p0")->send(std::span<const std::span<const std::byte>>(one));
    }
    drain();
    check_eq(fake_httpd::instance().wire(kStalled).size(), 0, "(b) held on a full socket");
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL});
    drain();
    check_eq(fake_httpd::instance().wire(kStalled).size(), 0,
             "    and nothing moves it while the link is idle");
    (void)fake_httpd::instance().deliver_frame(kHealthy, kBody);
    check(fake_httpd::instance().wire(kStalled) == want,
          "    an inbound frame on the link offers it the socket again: whole, newest");

    // (c) the same stall, and no traffic at all: the application's own tick.
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_wire(kStalled);
    {
        const std::span<const std::byte> one[] = {std::span<const std::byte>(a)};
        link->peer_link("p0")->send(std::span<const std::span<const std::byte>>(one));
    }
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL});
    link->flush_held();
    drain();
    check(fake_httpd::instance().wire(kStalled) == want,
          "(c) flush_held() from the application's tick delivers it");
    link->flush_held();
    check_eq(fake_httpd::instance().queue_depth(), 0, "    and is a no-op once nothing is held");
    check_eq(link->stats().tx_send_failed, 0, "no send failed");

    reset(link);
}

// ---------------------------------------------------------------------------
// 10 — a full queue never strikes; pinning the large class does.
// ---------------------------------------------------------------------------
/**
 * @brief More than kTxWaitDepth + kTxStallStrikes frames into a peer whose socket takes
 *        nothing, with a flush between each, are counted refusals and no strike: the peer
 *        stays connected. A slow peer that drains a little at a time is never struck either.
 */
void test_a_full_queue_never_strikes() {
    std::printf("a burst into a peer whose TCP window is shut:\n");
    auto link = make_link();
    claim(kStalled);
    claim(kHealthy);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    const auto push = [&](std::size_t i) {
        const auto frame = tagged(i);
        link->send(std::span<const std::byte>(frame));
        drain();  // the httpd task preempts the producer: a flush runs between stages
    };
    // A subtree subscribe or a many-field settings write fans one frame per vertex out in a
    // step, and the first KiB fill the send buffer until the next ACK. The first frame goes on
    // the wire, kTxWaitDepth wait, and every later one is refused, each followed by a flush
    // whose write moves no byte. None of that is a stall strike (#2195, as #2174 rules).
    constexpr std::size_t kFill = 1 + httpd_ws_link_t::kTxWaitDepth;
    constexpr std::size_t kBurst = kFill + httpd_ws_link_t::kTxStallStrikes + 16;
    const std::uint32_t drops_before = link->stats().enqueue_drops;
    for (std::size_t i = 0; i < kBurst; ++i) push(i);
    check_eq(link->stats().enqueue_drops - drops_before, kBurst - kFill,
             "every frame past the queue is a counted refusal");
    check_eq(link->stats().tx_stall_strikes, 0, "and none of them is a stall strike");
    check_eq(link->stats().sessions_condemned, 0, "no session condemned");
    check(fake_httpd::instance().has_session(kStalled), "the peer stays connected");
    std::size_t census = 0;
    link->enumerate_peers([&census](std::string_view) { ++census; });
    check_eq(census, 2, "both peers are still served");
    // The window opens: the held frames go out, oldest first, and the peer is served again.
    fake_httpd::instance().clear_sent_frames();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL});
    link->flush_held();
    drain();
    check_eq(frames_to(kStalled).size(), kFill, "its held frames all go out once it drains");

    // A peer that drains a little at a time is not struck, however long it is slow.
    reset(link);
    link = make_link();
    claim(kStalled);
    drain();
    std::vector<send_result_t> slow;
    for (std::size_t i = 0; i < 4 * httpd_ws_link_t::kTxStallStrikes; ++i) {
        slow.push_back(send_result_t::TIMEOUT);
        slow.push_back(send_result_t::TIMEOUT);
        slow.push_back(send_result_t::FULL);
        slow.push_back(send_result_t::FULL);
    }
    fake_httpd::instance().set_send_script(kStalled, slow);
    for (std::size_t i = 0; i < 3 * httpd_ws_link_t::kTxStallStrikes; ++i) push(i);
    check_eq(link->stats().tx_stall_strikes, 0, "a peer whose socket takes bytes is never struck");
    check(fake_httpd::instance().has_session(kStalled), "and stays connected");

    reset(link);
}

/**
 * @brief A peer that never drains and holds the large class's ONLY block is struck too: every
 *        later large frame is refused at the draw, so no replacement ever reaches the peer,
 *        and the refused draws count against it instead. Once it is struck the block comes
 *        back and the healthy peer receives large frames again.
 */
void test_a_peer_pinning_the_last_large_block_is_struck() {
    std::printf("a peer that pins the large class's only block:\n");
    auto link = make_link({.tx_large = {.bytes = 4096, .slots = 1}});
    check_eq(link->tx_large_slot_capacity(), 1, "the large class has one block");
    claim(kStalled);
    claim(kHealthy);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_sent_frames();
    const auto push = [&](std::size_t tag) {
        std::vector<std::byte> frame(2000, std::byte{0x33});
        frame[0] = static_cast<std::byte>(tag & 0xFF);
        link->send(std::span<const std::byte>(frame));
        drain();
    };
    push(0);  // the class's one block: the healthy peer takes it, the stalled one pins it
    check_eq(frames_to(kHealthy).size(), 1, "the first large frame reached the healthy peer");
    check_eq(link->tx_large_in_use(), 1, "and the stalled peer still holds the block");
    for (std::size_t i = 1; i < httpd_ws_link_t::kTxStallStrikes; ++i) push(i);
    check_eq(link->stats().tx_large_dropped, httpd_ws_link_t::kTxStallStrikes - 1,
             "every later large frame was refused at the draw, for every peer");
    check_eq(link->stats().tx_stall_strikes, 0, "one refused draw short of the bound: not struck");
    push(httpd_ws_link_t::kTxStallStrikes);
    check_eq(link->stats().tx_stall_strikes, 1, "the bound reached: the pinning peer is struck");
    check_eq(link->stats().sessions_condemned, 1, "through the strike path (condemned)");
    check_eq(link->tx_large_in_use(), 0, "and the block is back in the class");
    fake_httpd::instance().clear_sent_frames();
    push(200);
    const auto healthy = frames_to(kHealthy);
    check(healthy.size() == 1 && healthy[0].size() == 2000 && healthy[0][0] == std::byte{200},
          "the healthy peer receives large frames again");
    reset(link);
}

// ---------------------------------------------------------------------------
// 11 — producers racing close and reclaim.
// ---------------------------------------------------------------------------
/** @brief A published value of @p n patterned bytes (the payload a delivery retains). */
tr::graph::value_ref_t retained_value(std::size_t n) {
    std::vector<std::byte> b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = std::byte(static_cast<std::uint8_t>(i * 3u + 1u));
    return tr::graph::value_ref_t::adopt(tr::graph::value_t::make_copy(b, tr::mem::heap_source()));
}

/**
 * @brief stage() on producer tasks against the httpd task's flush AND its session close and
 *        reclaim (free_ctx -> reclaim_slot releases both frames under tx_m and recycles the
 *        generation), with sessions re-admitted onto the same descriptors. Both gathered and
 *        retained frames, and directed sends through resolutions that go stale. Run under
 *        TSan in CI; the post-condition here is that nothing wedged and every block was freed.
 */
void test_producers_race_close_and_reclaim() {
    std::printf("producers racing close and reclaim:\n");
    auto link = make_link();
    static constexpr int kFds[] = {640, 641, 642, 643};
    for (const int fd : kFds) claim(fd);
    drain();
    fake_httpd::instance().set_send_script(kFds[0], {send_result_t::TIMEOUT});
    fake_httpd::instance().set_send_script(kFds[1], {send_result_t::FULL, send_result_t::SHORT});
    const tr::graph::value_ref_t v = retained_value(300);

    std::atomic<bool> stop{false};
    httpd_ws_link_t* const raw = link.get();
    const tr::graph::value_t* const val = v.get();
    std::vector<std::thread> producers;
    for (int t = 0; t < 3; ++t)
        producers.emplace_back([raw, val, t] {
            const std::span<const std::byte> head[] = {std::span<const std::byte>(kBody)};
            for (int i = 0; i < 400; ++i) {
                if ((i + t) % 3 == 0) {
                    raw->send(std::span<const std::span<const std::byte>>(head), *val,
                              tr::graph::delivery_lane_t{});
                } else if ((i + t) % 3 == 1) {
                    raw->send(std::span<const std::byte>(kBody));
                } else if (tr::net::transport_t* const to = raw->peer_link("p2")) {
                    to->send(std::span<const std::byte>(kBody));
                }
            }
        });
    std::thread httpd_task([&stop] {
        for (unsigned i = 0; !stop.load(std::memory_order_relaxed); ++i) {
            (void)fake_httpd::instance().run_pending();
            if (i % 7 == 0) {
                // A departure and a fresh client on the same descriptor: free_ctx runs the
                // reclaim on this task, exactly where httpd runs it.
                const int fd = kFds[(i / 7) % 4];
                fake_httpd::instance().close_session(fd);
                claim(fd);
            }
        }
    });
    for (auto& p : producers) p.join();
    stop.store(true, std::memory_order_relaxed);
    httpd_task.join();
    drain();

    // Every socket drains now. A peer whose socket took nothing through the race may still
    // hold a full queue (a full queue never strikes it, #2195), so its held frames go out
    // first, as the application's flush_held() sends them; then the final frame has room.
    for (const int fd : kFds) fake_httpd::instance().set_send_script(fd, {send_result_t::FULL});
    link->flush_held();
    drain();
    fake_httpd::instance().clear_sent_frames();
    const auto last = tagged(0x6B);
    link->send(std::span<const std::byte>(last));
    drain();
    std::size_t served = 0;
    for (const int fd : kFds) {
        const auto got = frames_to(fd);
        if (!got.empty() && got.back().size() == 1 && got.back()[0] == last[0]) ++served;
    }
    std::size_t open_peers = 0;
    link->enumerate_peers([&open_peers](std::string_view) { ++open_peers; });
    check_eq(served, open_peers, "after the race, the final frame reaches every open peer");
    reset(link);
    check(v->use_count() == 1, "every retained frame released its value reference");
}

// ---------------------------------------------------------------------------
// 12 — latest wins per path, never across paths; a full queue is a counted refusal (#2195).
// ---------------------------------------------------------------------------
/** @brief Subscription edges of one session: what a delivery lane's key names. Compared,
 *         never dereferenced, so any distinct addresses serve. */
const int kPaths[16] = {};

/** @brief Deliver the one-byte value @p tag to every peer on @p lane, the way a subscription
 *         delivery does (the retained send). */
void vertex_value(httpd_ws_link_t& link, std::size_t tag, const tr::graph::delivery_lane_t& lane) {
    const auto b = tagged(tag);
    const tr::graph::value_ref_t v =
        tr::graph::value_ref_t::adopt(tr::graph::value_t::make_copy(b, tr::mem::heap_source()));
    link.send(std::span<const std::span<const std::byte>>(), *v, lane);
}

/** @brief A SAMPLE of path @p i: lane depth 1, latest wins. */
tr::graph::delivery_lane_t sample_of(std::size_t i) {
    return tr::graph::delivery_lane_t{.key = &kPaths[i], .depth = 1};
}

/** @brief The one-byte payloads of the retained frames on @p fd's wire, in wire order. */
std::vector<std::byte> values_on_wire(int fd) {
    std::vector<std::byte> out;
    const auto w = fake_httpd::instance().wire(fd);
    for (std::size_t i = 0; i + 2 < w.size() && w[i] == std::byte{0x82} && w[i + 1] == std::byte{1};
         i += 3)
        out.push_back(w[i + 2]);
    return out;
}

/**
 * @brief The acceptance case: one session subscribed to two vertices (two SAMPLE paths), both
 *        written back to back while its wire is busy, and BOTH deliver. Before the fix the
 *        frame for B replaced the one for A (on the waiting slot, or on the wire before a byte
 *        of it went out), and the peer never saw A's value at all.
 */
void test_two_vertices_both_deliver() {
    std::printf("one session, two vertices, written back to back on a busy wire:\n");
    auto link = make_link();
    claim(kStalled);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_wire(kStalled);
    vertex_value(*link, 0xA1, sample_of(0));
    drain();
    vertex_value(*link, 0xB1, sample_of(1));
    drain();
    check_eq(fake_httpd::instance().wire(kStalled).size(), 0, "the wire is busy: nothing out");
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL});
    link->flush_held();
    drain();
    const auto got = values_on_wire(kStalled);
    check(got.size() == 2 && got[0] == std::byte{0xA1} && got[1] == std::byte{0xB1},
          "both values delivered, A then B");
    check_eq(link->stats().tx_conflated, 0, "neither was counted as conflated");
    check_eq(link->stats().enqueue_drops, 0, "nor dropped");

    reset(link);
}

/** @brief Writes to the SAME path still conflate to the newest value, on the waiting frame and
 *         on a wire frame not one byte of which went out, and each replaced one is counted in
 *         `tx_conflated`, per peer and link-wide; a second path in between is untouched. */
void test_one_path_still_conflates() {
    std::printf("one session, the same path written three times on a busy wire:\n");
    auto link = make_link();
    claim(kStalled);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_wire(kStalled);
    for (const auto& [tag, path] : {std::pair{0xA1, 0}, {0xB1, 1}, {0xA2, 0}, {0xA3, 0}}) {
        vertex_value(*link, tag, sample_of(path));
        drain();
    }
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL});
    link->flush_held();
    drain();
    const auto got = values_on_wire(kStalled);
    check(got.size() == 2 && got[0] == std::byte{0xA3} && got[1] == std::byte{0xB1},
          "A's newest value and B's value went out, nothing else");
    check_eq(conflated(*link, 0), 2, "the two older A values are counted on the peer");
    check_eq(link->stats().tx_conflated, 2, "and in the link's tx_conflated");
    check_eq(link->stats().enqueue_drops, 0, "and neither is a refusal");

    reset(link);
}

/** @brief Frames from no vertex (a reply, an event, an ANNOUNCE) are one-shots: never
 *         replaced, each delivered in order. */
void test_one_shots_are_never_replaced() {
    std::printf("one-shot frames on a busy wire:\n");
    auto link = make_link();
    claim(kStalled);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_sent_frames();
    for (std::size_t i = 0; i < 3; ++i) {
        const auto frame = tagged(0x10 + i);
        link->send(std::span<const std::byte>(frame));
        drain();
    }
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL});
    link->flush_held();
    drain();
    const auto got = frames_to(kStalled);
    bool in_order = got.size() == 3;
    for (std::size_t i = 0; in_order && i < 3; ++i)
        in_order = got[i].size() == 1 && got[i][0] == tagged(0x10 + i)[0];
    check(in_order, "all three delivered, in order");
    check_eq(conflated(*link, 0), 0, "none conflated");

    reset(link);
}

/** @brief A frame offered to a full queue is refused and counted in `enqueue_drops`, never
 *         dropped silently and never put in another frame's place; the held ones go out
 *         whole and in order once the socket drains. */
void test_a_full_queue_is_a_counted_refusal() {
    std::printf("a peer whose queue is full:\n");
    auto link = make_link();
    claim(kStalled);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_sent_frames();
    const std::uint32_t drops_before = link->stats().enqueue_drops;
    constexpr std::size_t kHeld = 1 + httpd_ws_link_t::kTxWaitDepth;  // the wire + the queue
    constexpr std::size_t kOver = 3;
    for (std::size_t i = 0; i < kHeld + kOver; ++i) {
        const auto frame = tagged(0x20 + i);
        link->send(std::span<const std::byte>(frame));
        drain();
    }
    check_eq(link->stats().enqueue_drops - drops_before, kOver,
             "every frame past the queue's depth is refused, and counted");
    check_eq(link->stats().tx_conflated, 0, "none replaced a held frame");
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL});
    link->flush_held();
    drain();
    const auto got = frames_to(kStalled);
    bool in_order = got.size() == kHeld;
    for (std::size_t i = 0; in_order && i < kHeld; ++i)
        in_order = got[i].size() == 1 && got[i][0] == tagged(0x20 + i)[0];
    check(in_order, "the held frames all went out, oldest first");

    reset(link);
}

// ---------------------------------------------------------------------------
// 13 — the lanes' refusal paths (#2195).
// ---------------------------------------------------------------------------
/** @brief Open a stalled peer's socket and return the one-byte values that went out. */
std::vector<std::byte> release_held(httpd_ws_link_t& link) {
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::FULL});
    link.flush_held();
    drain();
    return values_on_wire(kStalled);
}

/** @brief The per-peer cap is kTxWaitDepth across every lane: a ninth path waiting is
 *         refused, while a newer SAMPLE of a path already waiting still replaces it (it takes
 *         no node). */
void test_a_sample_replaces_in_a_full_queue() {
    std::printf("a full queue of eight paths:\n");
    static_assert(httpd_ws_link_t::kTxWaitDepth == 8, "the per-peer cap");
    auto link = make_link();
    claim(kStalled);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_wire(kStalled);
    for (std::size_t i = 0; i <= httpd_ws_link_t::kTxWaitDepth + 1; ++i) {  // wire + 8 + one
        vertex_value(*link, 0x30 + i, sample_of(i));
        drain();
    }
    check_eq(link->stats().enqueue_drops, 1, "a ninth path waiting is refused, counted");
    vertex_value(*link, 0x53, sample_of(3));
    drain();
    check_eq(link->stats().enqueue_drops, 1, "a newer value of a waiting path is not refused");
    check_eq(link->stats().tx_conflated, 1, "it replaced the older one, counted");
    const auto got = release_held(*link);
    bool ok = got.size() == 1 + httpd_ws_link_t::kTxWaitDepth;
    for (std::size_t i = 0; ok && i < got.size(); ++i)
        ok = got[i] == std::byte(i == 3 ? 0x53 : 0x30 + i);
    check(ok, "every held path went out in order, path 3 with its newest value");

    reset(link);
}

/** @brief A STREAM of depth 3 on a stalled peer: best-effort sheds the oldest waiting frame
 *         (counted in `tx_conflated`), reliable refuses the newest (counted in
 *         `enqueue_drops`); the control lane's ninth waiting frame is refused like a reliable
 *         one (kEgressControlDepth). */
void test_a_full_stream_sheds_or_refuses() {
    std::printf("full STREAM lanes and the control lane:\n");
    for (const bool reliable : {false, true}) {
        auto link = make_link();
        claim(kStalled);
        drain();
        fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
        fake_httpd::instance().clear_wire(kStalled);
        const tr::graph::delivery_lane_t lane{.key = &kPaths[0], .depth = 3, .reliable = reliable};
        for (std::size_t i = 0; i < 5; ++i) {  // the wire, three waiting, and one more
            vertex_value(*link, 0x60 + i, lane);
            drain();
        }
        const auto got = release_held(*link);
        if (!reliable) {
            check_eq(link->stats().tx_conflated, 1, "best-effort: the oldest waiting is shed");
            check_eq(link->stats().enqueue_drops, 0, "    and nothing refused");
            check(got == std::vector<std::byte>{std::byte{0x60}, std::byte{0x62}, std::byte{0x63},
                                                std::byte{0x64}},
                  "    the wire frame, then the newest three, in order");
        } else {
            check_eq(link->stats().enqueue_drops, 1, "reliable: the newest is refused, counted");
            check_eq(link->stats().tx_conflated, 0, "    and nothing waiting is lost");
            check(got == std::vector<std::byte>{std::byte{0x60}, std::byte{0x61}, std::byte{0x62},
                                                std::byte{0x63}},
                  "    the four held frames, in order");
        }
        reset(link);
    }
    // A STREAM deeper than the peer's cap meets the cap first: best-effort still sheds its
    // own oldest (its §4.4 arm, and no node drawn), reliable still refuses the newest.
    constexpr std::size_t kCap = httpd_ws_link_t::kTxWaitDepth;
    for (const bool reliable : {false, true}) {
        auto link = make_link();
        claim(kStalled);
        drain();
        fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
        fake_httpd::instance().clear_wire(kStalled);
        const tr::graph::delivery_lane_t deep{.key = &kPaths[0], .depth = 16, .reliable = reliable};
        for (std::size_t i = 0; i <= kCap + 1; ++i) {  // the wire, the cap, and one more
            vertex_value(*link, 0x90 + i, deep);
            drain();
        }
        const auto got = release_held(*link);
        std::vector<std::byte> want{std::byte{0x90}};
        for (std::size_t i = reliable ? 1 : 2; want.size() < 1 + kCap; ++i)
            want.push_back(std::byte(0x90 + i));
        check(got == want, reliable ? "a deep reliable STREAM at the cap keeps its held frames"
                                    : "a deep best-effort STREAM at the cap sheds its oldest");
        check_eq(link->stats().tx_conflated, reliable ? 0 : 1, "    counted as conflated");
        check_eq(link->stats().enqueue_drops, reliable ? 1 : 0, "    or as refused");
        reset(link);
    }
    auto link = make_link();
    claim(kStalled);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_wire(kStalled);
    vertex_value(*link, 0x70, sample_of(0));  // a path's frame on the wire, so the control
    drain();                                  // lane's own depth is what fills
    for (std::size_t i = 0; i <= tr::net::kEgressControlDepth; ++i) {
        vertex_value(*link, 0x71 + i, tr::graph::delivery_lane_t{});
        drain();
    }
    check_eq(link->stats().enqueue_drops, 1, "the control lane's ninth waiting frame is refused");
    check_eq(link->stats().tx_conflated, 0, "and no control frame is replaced");
    check_eq(release_held(*link).size(), 1 + tr::net::kEgressControlDepth,
             "the path's frame and eight control frames went out");

    reset(link);
}

/** @brief A store that refuses every draw of one size once armed: the lever that refuses a
 *         waiting node and nothing else. */
class node_refusing_source_t final : public tr::mem::block_source_t {
   public:
    node_refusing_source_t() noexcept : tr::mem::block_source_t("node_refusing") {}
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        draws.push_back(bytes);
        if (bytes == refuse) return nullptr;
        return tr::mem::heap_source().try_alloc(bytes, align);
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::heap_source().release(p, bytes, align);
    }
    std::vector<std::size_t> draws; /**< @brief The size of every draw, in order. */
    std::size_t refuse = 0;         /**< @brief Draws of this size are refused; 0 none. */
};

/** @brief A waiting frame whose node `memory.io` refuses is the link's counted refusal: the
 *         frames already held are untouched and go out in order. */
void test_a_refused_node_draw_is_a_counted_refusal() {
    std::printf("a refused waiting-node draw:\n");
    node_refusing_source_t io;
    tr::net::httpd_ws_config_t config{};
    config.memory.io = &io;
    auto link = make_link(config);
    claim(kStalled);
    drain();
    fake_httpd::instance().set_send_script(kStalled, {send_result_t::TIMEOUT});
    fake_httpd::instance().clear_wire(kStalled);
    for (std::size_t i = 0; i < 2; ++i) {  // the wire, then the in-slot spare: no node drawn
        vertex_value(*link, 0x80 + i, sample_of(i));
        drain();
    }
    io.draws.clear();
    vertex_value(*link, 0x82, sample_of(2));  // its block, then a drawn node
    drain();
    check(io.draws.size() == 2 && io.draws[0] != io.draws[1],
          "a third waiting frame draws its block and then its node");
    io.refuse = io.draws.back();
    vertex_value(*link, 0x83, sample_of(3));
    drain();
    io.refuse = 0;
    check_eq(link->stats().enqueue_drops, 1, "the frame whose node was refused is counted");
    check_eq(link->stats().tx_conflated, 0, "and nothing was replaced");
    const auto got = release_held(*link);
    check(got == std::vector<std::byte>{std::byte{0x80}, std::byte{0x81}, std::byte{0x82}},
          "the held frames went out, in order");

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
    test_a_quiet_producer_still_delivers_the_newest_frame();
    test_a_full_queue_never_strikes();
    test_a_peer_pinning_the_last_large_block_is_struck();
    test_producers_race_close_and_reclaim();
    test_two_vertices_both_deliver();
    test_one_path_still_conflates();
    test_one_shots_are_never_replaced();
    test_a_full_queue_is_a_counted_refusal();
    test_a_sample_replaces_in_a_full_queue();
    test_a_full_stream_sheds_or_refuses();
    test_a_refused_node_draw_is_a_counted_refusal();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
