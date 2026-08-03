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
 * The fourth was added after the first on-silicon gate, which passed 1-3 and still failed:
 *   4. deciding to close a session is not closing it. The decision must take effect in the
 *      link's OWN state at the instant it is reached — refusing new frames to that fd and
 *      skipping the ones already queued — because everything that would carry the close
 *      itself (`httpd_sess_trigger_close`) rides the one control socket the backlog is
 *      starving, and can be delayed behind that backlog or dropped by lwIP with success
 *      reported. The bench measured the fd failing every 2 s for two minutes after the
 *      close was logged; that is what cases 6 and 7 pin.
 *
 * What this suite deliberately does NOT claim: the starvation itself. That the httpd
 * task stops blocking for seconds per frame is a property of `SO_SNDTIMEO` on a real
 * lwIP socket under a real fan-out, and the watchdog trip that motivated #835 is only
 * observable on silicon. It is handed to the HIL bench, not faked here.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
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

// ---------------------------------------------------------------------------
// 6 — the close must take effect while the control queue is jammed.
// ---------------------------------------------------------------------------
/**
 * @brief The shape the on-silicon gate failed on: a peer is found broken while frames to
 *        it are ALREADY queued, and the control queue is full.
 *
 * Three facts about `esp_http_server` make the obvious close a no-op here, and all three
 * are in release/v5.5's own sources:
 *   - `httpd_sess_trigger_close` IS `httpd_queue_work(httpd_sess_close, sd)`
 *     (httpd_sess.c:476-481), so the close is strictly FIFO behind the very backlog it
 *     exists to clear, on the one task that drains it;
 *   - each backlog item ahead of it spends the full derived send bound on a stalled
 *     socket, so "behind the backlog" is seconds per entry, not microseconds;
 *   - `httpd_queue_work` is a bare `sendto` on a loopback UDP socket (httpd_main.c), and
 *     an enqueue past that socket's mbox is dropped by lwIP with success returned — so
 *     `ESP_OK` is not evidence the close was queued at all.
 *
 * So the close cannot be something the link ASKS the jammed queue for. What this pins is
 * the alternative: the fd is marked dead in the link's own state at the moment of the
 * decision, the queued backlog to it drains at queue speed instead of at socket speed,
 * new frames to it are refused, and the socket is shut so httpd's select arm — the one
 * arm with no control message on it — reaps the session.
 */
void test_jammed_queue_still_closes_the_doomed_fd() {
    std::printf("a peer found broken with its backlog already queued, control queue full:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(700);
    claim(701);
    // The mbox depth behind the control socket (CONFIG_LWIP_UDP_RECVMBOX_SIZE default).
    constexpr std::size_t kCtrlDepth = 6;
    fake_httpd::instance().set_queue_capacity(kCtrlDepth);
    fake_httpd::instance().set_send_script(700, {send_result_t::SHORT});
    fake_httpd::instance().set_send_script(701, {send_result_t::FULL});

    // Fill the control queue with frames for the peer that is about to be found
    // desynchronised — the fan-out backlog, undrained.
    auto* const doomed = link->peer_link("fd700");
    check(doomed != nullptr, "the doomed peer resolves through the bus facet");
    for (std::size_t i = 0; i < kCtrlDepth; ++i) doomed->send(std::span<const std::byte>(kBody));
    check(fake_httpd::instance().queue_depth() == kCtrlDepth, "the control queue is full");

    // From here the control socket takes NOTHING. That is not a contrived extreme: the
    // backlog regenerates from the delivery plane as fast as the one httpd task drains
    // it, so every close message the link asks for is offered to a socket that is full,
    // and a full one either reports the refusal or — worse, and invisibly — lets lwIP bin
    // the datagram while reporting success. Either way the close cannot be something the
    // link REQUESTS of this queue.
    fake_httpd::instance().set_queue_refusing(true);

    // ONE step: the head of the backlog short-writes, and the link decides to close.
    check(fake_httpd::instance().run_one(), "the first queued send ran");
    check(fake_httpd::instance().writes(700) == 1, "it reached the socket and short-wrote");

    // (a) a NEW frame to that fd must be refused AT THE LINK, before the control socket
    // is ever asked. An enqueue that reaches the socket and is refused there is counted
    // as a link-level enqueue drop, so that counter tells the two apart.
    const std::uint32_t offered_before = link->enqueue_drops();
    doomed->send(std::span<const std::byte>(kBody));
    check(link->enqueue_drops() == offered_before,
          "queue_send refuses the frame at the link: the control socket is never asked");

    // (b) the frames already queued must drain WITHOUT touching the socket — each one
    // would otherwise spend the whole send bound on a peer that is already doomed, which
    // is exactly the two-minute stall the on-silicon gate measured.
    drain();
    check(fake_httpd::instance().writes(700) == 1,
          "only the send that PROVED the peer broken reaches the socket; the backlog is skipped");

    // (c) and the session is actually gone, with no control message anywhere in the path.
    check(!fake_httpd::instance().has_session(700),
          "the doomed session is closed although the control queue never took a thing");
    check(fake_httpd::instance().has_session(701), "the healthy peer is untouched");

    fake_httpd::instance().set_queue_refusing(false);
    fake_httpd::instance().set_queue_capacity(0);
    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 7 — the dead mark is keyed to the SESSION, not to the descriptor number.
// ---------------------------------------------------------------------------
/**
 * @brief A dead-marked fd number, recycled onto a fresh session, must send normally.
 *
 * The hazard the whole file already documents for the teardown snapshot, now applying to
 * the dead mark: `httpd_sess_get` resolves by fd alone and lwIP hands the descriptor
 * straight back, so a mark that outlived its session would mute an unrelated peer for as
 * long as it held that number. The mark is cleared where the slot is recycled, on the
 * httpd task, before any new session can claim the number.
 */
void test_dead_mark_does_not_outlive_its_session() {
    std::printf("the same fd number, reused by a new peer:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(800);
    fake_httpd::instance().set_send_script(800, {send_result_t::TIMEOUT});
    for (int i = 0; i < 3; ++i) broadcast(*link);
    drain();
    check(!fake_httpd::instance().has_session(800), "the struck peer is torn down");

    // The same descriptor number comes back as an unrelated, healthy client.
    claim(800);
    fake_httpd::instance().set_send_script(800, {send_result_t::FULL});
    const std::size_t before = fake_httpd::instance().writes(800);
    broadcast(*link);
    check(fake_httpd::instance().writes(800) == before + 1,
          "the recycled fd is not muted by the previous session's dead mark");
    check(fake_httpd::instance().has_session(800), "and the new session survives");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 8 — the peer NAME, over a real AF_INET6 socket.
// ---------------------------------------------------------------------------
/**
 * @brief A peer on an IPv6 socket must be named by its address, not by zeroes.
 *
 * With `CONFIG_LWIP_IPV6` on (the default here) `esp_http_server` binds `PF_INET6`, so
 * every accepted WS socket is AF_INET6 and `getpeername` returns a `sockaddr_in6`.
 * Decoding that as a `sockaddr_in` reads `sin6_flowinfo` — always zero — as the address,
 * which is why the on-silicon strike log named its peer `0.0.0.0`: the one line that had
 * to identify the stalled peer identified nothing. This is the only case in the suite
 * that needs a REAL socket, because the defect is entirely in what the kernel writes.
 *
 * Skipped (not failed) where the host has no IPv6 loopback — that is an absent
 * instrument, not a finding.
 */
void test_peer_name_on_an_ipv6_socket() {
    std::printf("naming a peer on an AF_INET6 socket:\n");
    const int listener = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (listener < 0) {
        std::printf("  [SKIP] no AF_INET6 socket on this host\n");
        return;
    }
    sockaddr_in6 addr = {};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    socklen_t addr_len = sizeof(addr);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 1) != 0 ||
        ::getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len) != 0) {
        ::close(listener);
        std::printf("  [SKIP] no IPv6 loopback on this host\n");
        return;
    }
    const int client = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (client < 0 || ::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        if (client >= 0) ::close(client);
        ::close(listener);
        std::printf("  [SKIP] no IPv6 loopback on this host\n");
        return;
    }
    const int peer_fd = ::accept(listener, nullptr, nullptr);
    if (peer_fd < 0) {
        ::close(client);
        ::close(listener);
        std::printf("  [SKIP] no IPv6 loopback on this host\n");
        return;
    }

    // Admit that REAL descriptor as the peer's socket: the link names it at admission,
    // through the same getpeername the strike log's name comes from.
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(peer_fd);
    std::string named;
    link->enumerate_peers([&named](std::string_view p) { named = std::string(p); });
    std::printf("       peer named \"%s\"\n", named.c_str());
    check(named.rfind("0.0.0.0", 0) != 0, "an IPv6 peer is NOT named 0.0.0.0");
    check(named.rfind("fd", 0) != 0 && !named.empty(), "getpeername was decoded, not given up on");
    check(named.rfind("::1:", 0) == 0, "the name carries the loopback address it connected from");

    link.reset();
    fake_httpd::instance().close_all();
    ::close(peer_fd);
    ::close(client);
    ::close(listener);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link bounded-send host suite (#835):\n");
    test_queue_jam_strikes_nobody();
    test_send_timeout_strikes_the_stalled_peer();
    test_interleaved_success_never_closes();
    test_short_write_closes_immediately();
    test_send_bound_derivation();
    test_jammed_queue_still_closes_the_doomed_fd();
    test_dead_mark_does_not_outlive_its_session();
    test_peer_name_on_an_ipv6_socket();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
