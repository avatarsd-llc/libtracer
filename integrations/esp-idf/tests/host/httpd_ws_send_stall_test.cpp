/**
 * @file
 * @brief #835 — the bounded WebSocket send and the AIM of the brokenness detector, on
 *        the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 teardown suite: the REAL chip translation unit
 * (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) is compiled against the host fake
 * of `esp_http_server` (fake_httpd.hpp). What this suite adds to the fake's model is the
 * socket write itself — a session's send function, its RAW return value, and httpd's own
 * "any non-negative return is a delivered frame" rule.
 *
 * The three shipped behaviours pinned here, each of which #835 found aimed wrong:
 *   1. a jammed CONTROL QUEUE is evidence about the server, not about the peer whose
 *      frame happened to be enqueued next — it must strike nobody (the misattribution
 *      that closed HEALTHY sessions on silicon while the stalled peer never accrued a
 *      strike);
 *   2. a SEND that keeps failing IS evidence about its destination — three consecutive
 *      failed sends to one peer, with no success in between, tear that peer's session
 *      down, and only that peer's;
 *   3. a SHORT write is not a drop. Half a frame is on the wire, so the stream is
 *      desynchronised and the session closes at once, bypassing the streak — the one
 *      case where "drop the frame, keep the socket" (#481) is unsound.
 *
 * What this suite deliberately does NOT claim: the starvation itself. That the httpd
 * task stops blocking for seconds per frame is a property of `SO_SNDTIMEO` on a real
 * lwIP socket under a real fan-out, and the watchdog trip that motivated #835 is only
 * observable on silicon. It is handed to the HIL bench, not faked here.
 */

#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "fake_httpd.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

namespace {

using fake_httpd::send_result_t;
using tr::net::httpd_ws_link_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief A minimal frame body — the link only has to accept and route it. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/**
 * @brief Drain the control queue to quiescence, as the httpd task does.
 *
 * The TEST thread is the server task in this suite: every claim, send, drain and
 * teardown happens on it, so the whole suite is deterministic and the destructor takes
 * its documented on-the-server-task path.
 */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief Broadcast one frame to every open peer and let the httpd task drain it. */
void broadcast(httpd_ws_link_t& link) {
    link.send(std::span<const std::byte>(kBody));
    drain();
}

// ---------------------------------------------------------------------------
// 1 — a jammed control queue must strike NOBODY.
// ---------------------------------------------------------------------------
void test_queue_jam_strikes_nobody() {
    std::printf("control-queue jam, two live peers:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");
    claim(300);
    claim(301);

    // The jam: httpd_queue_work refuses every enqueue. On silicon this is what a stalled
    // peer's multi-second sends DO to the shared queue — the culprit is the peer that is
    // blocking the task, never the peer whose frame is being enqueued at that instant.
    fake_httpd::instance().set_queue_refusing(true);
    for (int i = 0; i < 6; ++i) broadcast(*link);
    fake_httpd::instance().set_queue_refusing(false);
    drain();

    check(fake_httpd::instance().has_session(300), "the jam did not close peer 300");
    check(fake_httpd::instance().has_session(301), "the jam did not close peer 301");
    check(link->enqueue_drops() == 12, "the jam is counted at the LINK (6 frames x 2 peers)");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 2 — a failing SEND strikes its destination, and only its destination.
// ---------------------------------------------------------------------------
void test_send_timeout_strikes_the_stalled_peer() {
    std::printf("one stalled peer under fan-out:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(400);
    claim(401);
    // 400's window is full and stays full; 401 drains normally.
    fake_httpd::instance().set_send_script(400, {send_result_t::TIMEOUT});
    fake_httpd::instance().set_send_script(401, {send_result_t::FULL});

    broadcast(*link);
    broadcast(*link);
    check(fake_httpd::instance().has_session(400),
          "two failed sends are not yet brokenness: the peer is still up");
    broadcast(*link);
    drain();

    check(!fake_httpd::instance().has_session(400),
          "three consecutive failed sends tear the STALLED peer down");
    check(fake_httpd::instance().has_session(401), "the healthy peer is untouched");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 3 — #481: a single failed send on an otherwise-live socket never closes it.
// ---------------------------------------------------------------------------
void test_interleaved_success_never_closes() {
    std::printf("an oversized reply failing between small frames (#481):\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(500);
    // The shipped #481 shape: the big reply times out, the small frames around it go out
    // fine. A drop streak is CONSECUTIVE, so this peer must never reach the cap.
    fake_httpd::instance().set_send_script(
        500, {send_result_t::TIMEOUT, send_result_t::TIMEOUT, send_result_t::FULL,
              send_result_t::TIMEOUT, send_result_t::TIMEOUT, send_result_t::FULL,
              send_result_t::TIMEOUT, send_result_t::TIMEOUT, send_result_t::FULL});
    for (int i = 0; i < 9; ++i) broadcast(*link);

    check(fake_httpd::instance().has_session(500),
          "interleaved successes reset the streak: the session survives");
    check(fake_httpd::instance().writes(500) == 9, "every frame was still attempted");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 4 — a SHORT write desynchronises the stream: close at once, no streak.
// ---------------------------------------------------------------------------
void test_short_write_closes_immediately() {
    std::printf("a write that expires MID-frame:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(600);
    fake_httpd::instance().set_send_script(600, {send_result_t::SHORT});

    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    broadcast(*link);
    drain();

    check(fake_httpd::instance().frames_sent() == sent_before,
          "a half-written frame is NOT reported as delivered");
    check(!fake_httpd::instance().has_session(600),
          "ONE short write closes the session — framing is destroyed, not a lost frame");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 5 — the send bound is derived, injectable, and clamped.
// ---------------------------------------------------------------------------
void test_send_bound_derivation() {
    std::printf("the per-socket send bound:\n");
    const httpd_ws_link_t derived(handle(), "/ws", 4, true);
    check(derived.send_timeout_ms() == 1250,
          "default = task-watchdog period / peer cap (5000 ms / 4 peers)");
    const httpd_ws_link_t capless(handle(), "/ws", 0, true);
    check(capless.send_timeout_ms() == 1250, "an unbounded cap derives from the socket budget");
    const httpd_ws_link_t injected(handle(), "/ws", 4, true, 200);
    check(injected.send_timeout_ms() == 200, "an injected bound is honoured verbatim");
    const httpd_ws_link_t clamped(handle(), "/ws", 4, true, 60000);
    check(clamped.send_timeout_ms() == 5000,
          "an injected bound is clamped to the server's own send_wait_timeout");
    fake_httpd::instance().close_all();
}

}  // namespace

int main() {
    std::printf("httpd_ws_link bounded-send host suite (#835):\n");
    test_queue_jam_strikes_nobody();
    test_send_timeout_strikes_the_stalled_peer();
    test_interleaved_success_never_closes();
    test_short_write_closes_immediately();
    test_send_bound_derivation();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
