/**
 * @file
 * @brief #954 — a queued WebSocket send must address the SESSION it was gathered for,
 *        never whichever peer inherited that descriptor.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 teardown and #835 send-stall suites: the REAL chip
 * translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against
 * the host fake of `esp_http_server` (fake_httpd.hpp). What this suite adds is the one
 * event the other two never stage — a DESCRIPTOR being reused: peer A hangs up, and an
 * unrelated peer B is accepted onto the same socket number while A's frames are still
 * sitting in the control queue.
 *
 * That window is not exotic. `httpd_server` processes exactly one control message per
 * `select()` pass while session close, accept and handshake all proceed in that same
 * pass, so queued sends drain one per loop iteration; lwIP then hands the lowest free
 * descriptor straight back, which a browser reload reliably takes. Everything the old
 * TX path could ask at drain time answered a different question than the one that
 * mattered: `httpd_ws_get_fd_info` reports "some websocket lives at this number", and a
 * `slots_` scan for `s->fd == fd` finds whoever holds it now.
 *
 * The three properties pinned here:
 *   1. a frame gathered for a departed peer is NOT written to its successor's socket —
 *      the cross-session misdelivery, which on a peer-named server means one
 *      authenticated session's directed reply landing in another's stream;
 *   2. a send failure incurred draining that backlog does NOT strike the successor —
 *      the accounting that condemned a session which had failed nothing, the same
 *      false-positive shape #835 existed to remove;
 *   3. the guard is not vacuous: a frame queued for a peer that is STILL THERE is
 *      delivered exactly as before.
 *
 * Case 4 is the refutation that fixed the DESIGN rather than the code: it shows the
 * server's session ctx pointer — the identity token the teardown detach compares, and
 * the first remedy proposed for this bug — is the SAME pointer for A and for B, because
 * slots are recycled in place and the new peer lands in the departed one's slot. A ctx
 * comparison would have passed and misdelivered anyway; the generation is what makes the
 * pair unique.
 */

#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief A minimal frame body — the link only has to accept and route it. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/**
 * @brief The socket number both peers of a reuse case occupy, one after the other.
 *
 * One number, two unrelated sessions: that IS the defect's precondition, and lwIP
 * produces it by handing back the lowest free descriptor.
 */
constexpr int kFd = 400;

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/**
 * @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim).
 *
 * The claim is what mints the session, so calling it twice on one descriptor — with a
 * close in between — is how this suite stages a descriptor reuse.
 */
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

/** @brief Retire the link and the fake's sessions between cases. */
void reset(std::unique_ptr<httpd_ws_link_t>& link) {
    link.reset();
    fake_httpd::instance().close_all();
    drain();
}

// ---------------------------------------------------------------------------
// 1 — a directed frame gathered for A is never written to B's socket.
// ---------------------------------------------------------------------------
/**
 * @brief The cross-session misdelivery, on the DIRECTED path `peer_link` hands out.
 *
 * A's reply is queued and left in the control queue; A departs; B is accepted onto the
 * same descriptor. Draining must write NOTHING, because the only destination the item
 * names no longer exists. B's socket is scripted healthy on purpose — so a frame that
 * reaches it is delivered, counted, and indistinguishable to B from one addressed to it.
 */
void test_directed_frame_does_not_follow_the_descriptor() {
    std::printf("a directed frame queued for a departed peer, after the fd is reused:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");
    claim(kFd);

    tr::net::transport_t* const to_a = only_peer(*link);
    check(to_a != nullptr, "peer A resolved to a directed endpoint");
    if (to_a == nullptr) return;
    // Queue A's reply and leave it sitting there — the backlog the httpd task works
    // through one control message per select pass.
    to_a->send(std::span<const std::byte>(kBody));
    check(fake_httpd::instance().queue_depth() == 1, "A's frame is queued, not yet drained");

    // A hangs up and B is accepted onto the very same descriptor, both while the frame
    // waits. This is the whole event; everything after it is observation.
    fake_httpd::instance().close_session(kFd);
    claim(kFd);
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    const std::size_t writes_before = fake_httpd::instance().writes(kFd);
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    drain();
    check(fake_httpd::instance().writes(kFd) == writes_before,
          "B's socket took NO write from A's queued frame");
    check(fake_httpd::instance().frames_sent() == sent_before,
          "the frame was not delivered to anyone");
    check(fake_httpd::instance().has_session(kFd), "B's session is untouched");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — the broadcast producer resolves the same way.
// ---------------------------------------------------------------------------
/**
 * @brief The same reuse, on the BROADCAST path.
 *
 * `send()` snapshots its destinations under `peers_m_` and releases the lock before the
 * first enqueue, so it has the identical exposure and needs the identical token — a
 * separate producer, and therefore a separate case.
 */
void test_broadcast_frame_does_not_follow_the_descriptor() {
    std::printf("a broadcast frame queued for a departed peer, after the fd is reused:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(kFd);

    link->send(std::span<const std::byte>(kBody));
    check(fake_httpd::instance().queue_depth() == 1, "the broadcast is queued, not yet drained");

    fake_httpd::instance().close_session(kFd);
    claim(kFd);
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    const std::size_t writes_before = fake_httpd::instance().writes(kFd);
    drain();
    check(fake_httpd::instance().writes(kFd) == writes_before,
          "B's socket took NO write from the broadcast A was in");

    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — a stranger is never charged for the backlog it inherited.
// ---------------------------------------------------------------------------
/**
 * @brief The accounting half: B must not be condemned by A's failures.
 *
 * B's socket is scripted to fail — but only AFTER B exists, which is load-bearing: a
 * script set during A's session dies with A's entry in the session table, and a peer
 * whose writes all succeed accrues no strikes at all, so the assertion below would hold
 * for the wrong reason and pin nothing. B never had a frame of its own offered to it, so
 * the ONLY way its strike counter can reach kMaxConsecutiveTxDrops is by inheriting A's,
 * and the ONLY way its session can close here is that misattribution. A condemned session
 * is `shutdown` and reaped by the server's select arm, so its absence after the drain is
 * the observable.
 */
void test_inherited_failures_do_not_condemn_the_successor() {
    std::printf("a failing backlog gathered for a departed peer:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(kFd);

    tr::net::transport_t* const to_a = only_peer(*link);
    check(to_a != nullptr, "peer A resolved to a directed endpoint");
    if (to_a == nullptr) return;
    // Enough queued frames to trip the streak cap on whoever ends up being charged.
    for (int i = 0; i < 4; ++i) to_a->send(std::span<const std::byte>(kBody));
    check(fake_httpd::instance().queue_depth() == 4, "A's four frames are queued");

    fake_httpd::instance().close_session(kFd);
    claim(kFd);
    // Script B's socket to fail, now that B's session exists to carry the script. Any
    // write the drain performs here is a failed one, and every such failure is evidence
    // about a send B never asked for.
    fake_httpd::instance().set_send_script(kFd, {send_result_t::TIMEOUT});

    const std::size_t writes_before = fake_httpd::instance().writes(kFd);
    drain();
    check(fake_httpd::instance().writes(kFd) == writes_before,
          "B's socket was never written to on A's behalf");
    // has_session is the discriminator, and deliberately the only one: a condemned peer is
    // `shutdown` and then REAPED within the same drain, and once the entry is gone
    // `is_shut` reports false as well — so asking it here would be an assertion that
    // cannot fail in either direction.
    check(fake_httpd::instance().has_session(kFd),
          "B's session SURVIVED: it failed nothing, so it was struck for nothing");

    reset(link);
}

// ---------------------------------------------------------------------------
// 4 — why the session ctx POINTER is not the token (the design refutation).
// ---------------------------------------------------------------------------
/**
 * @brief The slot pointer alone cannot tell A from B, so neither can a ctx comparison.
 *
 * `detach_req_t` pairs each fd with the ctx pointer it carried and compares them at
 * drain — correct there, because a teardown admits no new peers, and the first remedy
 * proposed for this bug. On the LIVE path it is not enough: slots are recycled in place
 * and both claim sites take the first slot with `fd < 0`, so B is handed the object A
 * just released and registers it as its own session ctx. This case measures that
 * equality directly. It is the reason the identity carried through the TX path is
 * (slot, generation) and not the pointer.
 */
void test_the_session_ctx_pointer_aliases_across_the_reuse() {
    std::printf("the server's session ctx pointer, across a descriptor reuse:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(kFd);
    void* const ctx_a = fake_httpd::instance().session_ctx(kFd);
    check(ctx_a != nullptr, "A registered a session ctx (its peer slot)");

    fake_httpd::instance().close_session(kFd);
    claim(kFd);
    void* const ctx_b = fake_httpd::instance().session_ctx(kFd);
    check(ctx_b != nullptr, "B registered a session ctx");
    check(ctx_a == ctx_b, "B was handed A's RECYCLED slot: a ctx-pointer check would have passed");

    reset(link);
}

// ---------------------------------------------------------------------------
// 5 — the guard is not vacuous.
// ---------------------------------------------------------------------------
/**
 * @brief A frame queued for a peer that is still there is still delivered.
 *
 * Without this, every assertion above is satisfied by a link that simply stopped
 * sending. It pins the delivery the fix must leave alone: same queue, same drain, no
 * departure in between.
 */
void test_a_live_peer_still_receives_its_queued_frame() {
    std::printf("the ordinary path, with no departure in between:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(kFd);
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    tr::net::transport_t* const to_a = only_peer(*link);
    check(to_a != nullptr, "the peer resolved to a directed endpoint");
    if (to_a == nullptr) return;

    const std::size_t writes_before = fake_httpd::instance().writes(kFd);
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    to_a->send(std::span<const std::byte>(kBody));
    drain();
    check(fake_httpd::instance().writes(kFd) == writes_before + 2,
          "the live peer's socket took the frame's two writes (header, payload)");
    check(fake_httpd::instance().frames_sent() == sent_before + 1, "the frame was delivered");

    // And the broadcast producer, on the same live peer.
    link->send(std::span<const std::byte>(kBody));
    drain();
    check(fake_httpd::instance().frames_sent() == sent_before + 2,
          "a broadcast to the live peer was delivered too");

    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link session-identity host suite (#954):\n");
    test_directed_frame_does_not_follow_the_descriptor();
    test_broadcast_frame_does_not_follow_the_descriptor();
    test_inherited_failures_do_not_condemn_the_successor();
    test_the_session_ctx_pointer_aliases_across_the_reuse();
    test_a_live_peer_still_receives_its_queued_frame();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
