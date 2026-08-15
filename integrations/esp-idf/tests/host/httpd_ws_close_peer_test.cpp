/**
 * @file
 * @brief #1146 — `close_peer` on the ESP-IDF WebSocket server link: it must close exactly
 *        the named session, from an arbitrary task, and answer `true` only when the
 *        teardown really was initiated.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #954 identity and #835 send-stall suites: the REAL chip
 * translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against
 * the host fake of `esp_http_server` (fake_httpd.hpp). What this suite adds is the
 * CONTROL QUEUE as a first-class subject rather than as background — the whole issue
 * turns on what a refused enqueue does to the return value.
 *
 * The base contract (`transport_t::close_peer`) says `true` means "teardown was
 * initiated", and #1146 filed this as blocked on a transport fact: the only channel from
 * an application task to the httpd task is `httpd_queue_work`, and below the component's
 * ESP-IDF floor an enqueue past the control mbox was binned inside lwIP while still
 * reporting success. A `true` built on that would have been a lie told precisely when the
 * queue is fullest — i.e. when a stalling peer most deserves revoking. At the floor this
 * component requires (>=5.5.5) the mbox slot is reserved through a counting semaphore
 * BEFORE the `sendto`, so a full queue is a visible `ESP_FAIL`; the fake models exactly
 * that, and case 2 is what turns "the floor makes the return honest" from a code reading
 * into a measurement.
 *
 * The four properties pinned here:
 *   1. a named, served peer is closed — with a CLOSE frame carrying `kCloseRevoked`, so
 *      the peer can tell revocation from a network fault — and its departure reaches the
 *      routing plane through the ordinary free_ctx seam;
 *   2. a refused enqueue answers `false` and leaves the session UNTOUCHED: no half-close,
 *      and no `true` for a teardown that never happened;
 *   3. the close does not follow the SLOT: a peer that departs on its own, whose slot is
 *      then reclaimed by a stranger that inherits the very same positional name, must not
 *      be closed by the queued item (#954's rule on the close path — `p<slot>` is a pure
 *      function of the slot index, so the NAME cannot be the token);
 *   4. the guard is not vacuous — an unknown name, and a FLAT link, both answer `false`
 *      without touching anything.
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

/** @brief The socket number the peers of a reuse case occupy, one after the other. */
constexpr int kFd = 500;

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief The name of the single peer currently open, or empty. */
std::string only_peer_name(httpd_ws_link_t& link) {
    std::string name;
    link.enumerate_peers([&name](std::string_view p) { name = std::string(p); });
    return name;
}

/** @brief Peers the link currently reports. */
std::size_t peer_count(httpd_ws_link_t& link) {
    std::size_t n = 0;
    link.enumerate_peers([&n](std::string_view) { ++n; });
    return n;
}

/** @brief Whether a CLOSE frame carrying @p code was put on the wire to @p fd. */
bool sent_close_code(int fd, std::uint16_t code) {
    for (const auto& f : fake_httpd::instance().sent_frames()) {
        if (f.fd != fd || f.type != HTTPD_WS_TYPE_CLOSE || f.payload.size() < 2) continue;
        const auto hi = static_cast<std::uint16_t>(f.payload[0]);
        const auto lo = static_cast<std::uint16_t>(f.payload[1]);
        if (static_cast<std::uint16_t>((hi << 8) | lo) == code) return true;
    }
    return false;
}

/** @brief The departure names the routing plane was told about, in order. */
std::vector<std::string> g_departed;

/** @brief The peer-down notifier `fwd_router_t::add_child` would install. */
void note_departed(void* ctx, tr::net::peer_handle_t, std::string_view peer) {
    (void)ctx;
    g_departed.emplace_back(peer);
}

/** @brief Retire the link and the fake's sessions between cases. */
void reset(std::unique_ptr<httpd_ws_link_t>& link) {
    link.reset();
    fake_httpd::instance().close_all();
    drain();
    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().set_queue_refusing(false);
    fake_httpd::instance().clear_sent_frames();
    g_departed.clear();
}

/** @brief An adopting, peer-named link with the departure seam wired, as a node builds it. */
std::unique_ptr<httpd_ws_link_t> make_link(bool peer_named = true) {
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, peer_named);
    if (link->bus() != nullptr) link->bus()->set_peer_down_notifier(&note_departed, nullptr);
    return link;
}

// ---------------------------------------------------------------------------
// 1 — the named peer is closed, told why, and reported departed.
// ---------------------------------------------------------------------------
/**
 * @brief The capability itself: a listener drops one inbound session by name.
 *
 * The close cannot run in the caller's call — it is marshalled onto the httpd task, which
 * is the task that owns the descriptor's lifetime — so `true` is followed by a drain, and
 * the session's absence afterwards is the observable. The CLOSE code is checked because a
 * revoked controller that reads its teardown as a network fault reconnects forever, which
 * is the whole reason `kCloseRevoked` is distinct from the two auth codes.
 */
void test_named_peer_is_closed() {
    std::printf("close_peer on a served session:\n");
    auto link = make_link();
    check(link->ok(), "the adopting link registered its URI");
    claim(kFd);
    const std::string peer = only_peer_name(*link);
    check(!peer.empty(), "the peer is enumerable before the close");

    const bool initiated = link->bus()->close_peer(peer);
    check(initiated, "close_peer reported the teardown initiated");
    drain();

    check(!fake_httpd::instance().has_session(kFd), "the session is gone after the drain");
    check(peer_count(*link) == 0, "the link enumerates no peers");
    check(sent_close_code(kFd, httpd_ws_link_t::kCloseRevoked),
          "the peer was sent a CLOSE carrying kCloseRevoked");
    check(g_departed.size() == 1 && g_departed.front() == peer,
          "the routing plane was told the peer departed, by name");
    check(link->stats().sessions_condemned == 1, "the teardown is counted as a condemnation");

    reset(link);
}

// ---------------------------------------------------------------------------
// 2 — a refused enqueue is a FALSE, and nothing happens.
// ---------------------------------------------------------------------------
/**
 * @brief The case the whole issue turns on: the control queue is FULL.
 *
 * This is the state in which a `true` would be a security-relevant lie — the queue is
 * fullest exactly when a peer is stalling the httpd task, i.e. when the revoke matters
 * most. Two refusal routes are staged because both exist and only `queue_drops` tells
 * them apart: the full mbox (`set_queue_capacity`, the counting semaphore that cannot be
 * taken) and the socket-level refusal (`set_queue_refusing`). Either way the session must
 * survive intact — a `false` that had already half-closed the peer would be its own
 * defect.
 */
void test_refused_enqueue_answers_false() {
    std::printf("close_peer with the control queue full:\n");
    auto link = make_link();
    claim(kFd);
    const std::string peer = only_peer_name(*link);

    // Cap the mbox at one entry and spend it, so the close's enqueue is the one refused.
    fake_httpd::instance().set_queue_capacity(1);
    fake_httpd::instance().post([]() {});
    check(fake_httpd::instance().queue_depth() == 1, "the control queue is full");

    const std::size_t drops_before = fake_httpd::instance().queue_drops();
    const bool initiated = link->bus()->close_peer(peer);
    check(!initiated, "close_peer reported the teardown NOT initiated");
    check(fake_httpd::instance().queue_drops() > drops_before,
          "the refusal came from the mbox, not from the link declining to try");
    drain();
    check(fake_httpd::instance().has_session(kFd), "the session is untouched");
    check(peer_count(*link) == 1, "the peer is still enumerable");
    check(g_departed.empty(), "the routing plane was told nothing");

    // The socket-level refusal reaches the same verdict by the other route.
    fake_httpd::instance().set_queue_capacity(0);
    fake_httpd::instance().set_queue_refusing(true);
    check(!link->bus()->close_peer(peer), "a refusing control socket answers false too");
    fake_httpd::instance().set_queue_refusing(false);
    drain();
    check(peer_count(*link) == 1, "and still leaves the peer serving");

    // Not vacuous: with the queue clear, the same call succeeds.
    check(link->bus()->close_peer(peer), "with the queue drained the same close is accepted");
    drain();
    check(peer_count(*link) == 0, "and takes effect");

    reset(link);
}

// ---------------------------------------------------------------------------
// 3 — the queued close does not follow the SLOT to a stranger.
// ---------------------------------------------------------------------------
/**
 * @brief The identity half, and the reason the work item cannot carry a name or an fd.
 *
 * `p<slot>` is a pure function of the slot index and slots are recycled in place, so a
 * peer that departs and is replaced hands its successor the SAME name AND the same slot
 * address. Only the generation separates them. Here A is named for closure, departs on
 * its own before the item drains, and B lands in its slot: draining must close nothing.
 * B's presence afterwards — and the absence of any CLOSE frame to it — is the observable.
 */
void test_close_does_not_follow_the_slot() {
    std::printf("a queued close whose peer departed and whose slot was reclaimed:\n");
    auto link = make_link();
    claim(kFd);
    const std::string peer_a = only_peer_name(*link);

    // Queue the close and leave it sitting there — one control message per server pass.
    check(link->bus()->close_peer(peer_a), "A's close was accepted onto the queue");
    check(fake_httpd::instance().queue_depth() == 1, "and is still queued");

    // A hangs up on its own; B is accepted and claims the freed slot, inheriting the name.
    fake_httpd::instance().close_session(kFd);
    claim(kFd);
    const std::string peer_b = only_peer_name(*link);
    check(peer_b == peer_a, "B inherited A's positional name — the precondition of this case");

    fake_httpd::instance().clear_sent_frames();
    drain();
    check(fake_httpd::instance().has_session(kFd), "B's session survived A's close");
    check(peer_count(*link) == 1, "B is still enumerable");
    check(!sent_close_code(kFd, httpd_ws_link_t::kCloseRevoked),
          "B was sent no CLOSE frame of A's");

    reset(link);
}

// ---------------------------------------------------------------------------
// 4 — the refusals that cost nothing.
// ---------------------------------------------------------------------------
/**
 * @brief An unknown name and a FLAT link, both `false`.
 *
 * The flat arm is not a formality: `bus_link_t` is a public base, so `close_peer` is
 * reachable by an upcast past the null `bus()`, and a flat link has ONE routing identity
 * for every tab it carries — closing "a peer" there could only mean closing an arbitrary
 * one. The mode authority answers instead, the same fork the departure seam takes (#889).
 */
void test_unknown_and_flat_are_refused() {
    std::printf("the refusals that touch nothing:\n");
    auto link = make_link();
    claim(kFd);
    check(!link->bus()->close_peer("p99"), "an unknown name is refused");
    check(!link->bus()->close_peer(""), "an empty name is refused");
    check(peer_count(*link) == 1, "and the open peer is untouched");
    reset(link);

    auto flat = make_link(/*peer_named=*/false);
    check(flat->bus() == nullptr, "a flat link exposes no bus facet");
    claim(kFd);
    // Past the null bus(), as a caller holding a `bus_link_t*` upcast could.
    auto* const as_bus = static_cast<tr::net::bus_link_t*>(flat.get());
    check(!as_bus->close_peer("p0"), "a FLAT link refuses close_peer outright");
    check(fake_httpd::instance().has_session(kFd), "and its session is untouched");
    reset(flat);
}

}  // namespace

int main() {
    std::printf("httpd_ws_link_t close_peer (#1146)\n");
    test_named_peer_is_closed();
    test_refused_enqueue_answers_false();
    test_close_does_not_follow_the_slot();
    test_unknown_and_flat_are_refused();
    std::printf("%s\n", g_failures == 0 ? "OK" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
