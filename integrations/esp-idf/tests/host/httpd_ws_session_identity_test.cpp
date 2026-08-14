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
 *
 * Cases 6-9 stage the OTHER half of that window (#1013). The five above all queue the
 * frame BEFORE the reuse, so the identity it carries was minted while its peer was still
 * there; what they cannot see is a caller that RESOLVED before the reuse and sent after
 * it, because the send then minted its identity from the slot's current generation — a
 * generation describing the stranger, which made every check downstream self-satisfying.
 * The properties they add:
 *   6. a send on a handle resolved before the reuse reaches nobody, and is counted;
 *   7. the handle is per RESOLUTION — shared within one session, never across two;
 *   8. re-resolving after the reuse still reaches the successor (not vacuous);
 *   9. sustained churn neither exhausts the handle pool nor stops resolution.
 */

#include <cstddef>
#include <cstdint>
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

// ---------------------------------------------------------------------------
// 6 — the [resolve -> mint] window (#1013).
// ---------------------------------------------------------------------------
/**
 * @brief A send on a handle resolved BEFORE the reuse must not reach the successor.
 *
 * Case 1's window is `[mint -> drain]`: the frame was already queued when A departed, so
 * the identity it carries was minted while A was still there. This one is the half that
 * came before it. The caller resolves A, is preempted — `fwd_router` resolves the peer at
 * one end of a forward hop and sends at the other, with label and gather work in between —
 * and only then does A hang up and B arrive on the same descriptor and the same recycled
 * slot. The send that follows used to mint its identity from the slot's CURRENT
 * generation, which describes B, so every downstream check passed and A's directed reply
 * was written into B's socket.
 *
 * B's socket is scripted healthy on purpose, exactly as in case 1: a frame that reaches it
 * is delivered and indistinguishable to B from one addressed to it.
 */
void test_a_handle_resolved_before_the_reuse_does_not_reach_the_successor() {
    std::printf("a send on a handle resolved BEFORE the fd was reused (#1013):\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(kFd);

    // Resolve A — and send NOTHING yet. This is the whole difference from case 1.
    tr::net::transport_t* const to_a = only_peer(*link);
    check(to_a != nullptr, "peer A resolved to a directed handle");
    if (to_a == nullptr) return;

    fake_httpd::instance().close_session(kFd);
    drain();  // the departure is processed, as it is on the httpd task
    claim(kFd);
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    const std::size_t writes_before = fake_httpd::instance().writes(kFd);
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    const std::uint32_t dead_before = link->stats().tx_to_dead_peer;
    to_a->send(std::span<const std::byte>(kBody));
    check(fake_httpd::instance().queue_depth() == 0,
          "the send was refused at the resolving end: nothing was even queued");
    drain();
    check(fake_httpd::instance().writes(kFd) == writes_before,
          "B's socket took NO write from a reply resolved for A");
    check(fake_httpd::instance().frames_sent() == sent_before, "A's reply was delivered to nobody");
    check(link->stats().tx_to_dead_peer > dead_before, "the refusal was COUNTED, not silent");
    check(fake_httpd::instance().has_session(kFd), "B's session is untouched");

    reset(link);
}

/**
 * @brief The handle is per RESOLUTION, and a new session never inherits the old one's.
 *
 * Two properties in one case, because they are two halves of the same rule. Within ONE
 * session, two resolutions answer with the SAME object — they saw the same generation, so
 * sharing is safe and the pool stays bounded by the peer population rather than by
 * traffic. ACROSS a session boundary they must NOT: a handle stamped for A is retired when
 * A departs and quarantined behind the link's other retirements, so B's resolution comes
 * back as a different object. That inequality is what the shared per-slot endpoint could
 * never provide — it was one object per slot for the link's life, and equality here is
 * precisely how a stale holder used to reach a stranger.
 */
void test_the_handle_is_per_resolution_not_per_slot() {
    std::printf("the identity of the handle peer_link hands out:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(kFd);

    tr::net::transport_t* const first = only_peer(*link);
    tr::net::transport_t* const again = only_peer(*link);
    check(first != nullptr && first == again,
          "two resolutions of the SAME live session share one handle");

    fake_httpd::instance().close_session(kFd);
    drain();
    claim(kFd);
    tr::net::transport_t* const after = only_peer(*link);
    check(after != nullptr, "the successor resolves");
    check(after != first, "the successor got a DIFFERENT handle: A's was retired, not re-pointed");

    reset(link);
}

/**
 * @brief The guard is not vacuous on the resolve path either: re-resolving reaches B.
 *
 * Without this, the case above is satisfied by a link whose `peer_link` simply stopped
 * working after a reuse. The resolve-per-use contract is what the routing plane actually
 * does, and it must still deliver.
 */
void test_re_resolving_after_the_reuse_reaches_the_successor() {
    std::printf("re-resolving after the reuse, as the routing plane does:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(kFd);
    (void)only_peer(*link);  // A's resolution, abandoned unspent

    fake_httpd::instance().close_session(kFd);
    drain();
    claim(kFd);
    fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});

    tr::net::transport_t* const to_b = only_peer(*link);
    check(to_b != nullptr, "B resolved to a directed handle");
    if (to_b == nullptr) return;
    const std::size_t sent_before = fake_httpd::instance().frames_sent();
    to_b->send(std::span<const std::byte>(kBody));
    drain();
    check(fake_httpd::instance().frames_sent() == sent_before + 1,
          "B's own reply was delivered to B");

    reset(link);
}

/**
 * @brief Repeated resolve/depart cycles neither leak the pool nor stop resolving.
 *
 * The pool is grown on demand and recycled through a quarantined free list, so the failure
 * this pins is the one a bounded pool invites: enough churn to exhaust it, after which
 * `peer_link` would fail closed forever and the link would resolve nothing. Many more
 * cycles than the quarantine is deep, on a link whose peer population never exceeds one.
 */
void test_churn_does_not_exhaust_the_handle_pool() {
    std::printf("resolution handles across sustained session churn:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    bool all_resolved = true;
    bool all_delivered = true;
    for (int i = 0; i < 64; ++i) {
        claim(kFd);
        fake_httpd::instance().set_send_script(kFd, {send_result_t::FULL});
        tr::net::transport_t* const to = only_peer(*link);
        if (to == nullptr) {
            all_resolved = false;
            break;
        }
        const std::size_t sent_before = fake_httpd::instance().frames_sent();
        to->send(std::span<const std::byte>(kBody));
        drain();
        if (fake_httpd::instance().frames_sent() != sent_before + 1) all_delivered = false;
        fake_httpd::instance().close_session(kFd);
        drain();
    }
    check(all_resolved, "every one of 64 successive sessions resolved");
    check(all_delivered, "every one of them received its own directed frame");

    reset(link);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link session-identity host suite (#954, #1013):\n");
    test_directed_frame_does_not_follow_the_descriptor();
    test_broadcast_frame_does_not_follow_the_descriptor();
    test_inherited_failures_do_not_condemn_the_successor();
    test_the_session_ctx_pointer_aliases_across_the_reuse();
    test_a_live_peer_still_receives_its_queued_frame();
    test_a_handle_resolved_before_the_reuse_does_not_reach_the_successor();
    test_the_handle_is_per_resolution_not_per_slot();
    test_re_resolving_after_the_reuse_reaches_the_successor();
    test_churn_does_not_exhaust_the_handle_pool();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
