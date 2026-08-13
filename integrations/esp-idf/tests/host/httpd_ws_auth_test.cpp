/**
 * @file
 * @brief #1184 — the post-handshake authentication frame: an unauthenticated session is
 *        admitted and served NOTHING, and its deadline actually closes it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #958 admission suite next to it: the REAL chip translation unit
 * (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) compiled against the host fakes of
 * `esp_http_server` (fake_httpd.hpp) and `esp_timer` (fake_idf/esp_timer.h). Two seams make
 * this measurable rather than a code reading:
 *   - the fake records every frame the link puts on the wire WITH ITS TYPE AND PAYLOAD, so a
 *     close CODE is an observable — and the code is the entire difference between "your
 *     credential was refused" and "you were too slow";
 *   - the fake timer never fires on its own. A test calls `fake_esp_timer_fire()` when it
 *     wants a tick, so a deadline test is deterministic instead of a sleep against a
 *     wall clock, and the tick lands on the TEST thread — which is the split the link is
 *     written against (the sweep must marshal itself onto the httpd task, and the fake's
 *     control queue is what proves it did).
 *
 * What is pinned here:
 *   1. no hook installed = nothing changes, and no timer is armed;
 *   2. a session that has not authenticated is invisible to `enumerate_peers`,
 *      unresolvable through `peer_link`, skipped by a broadcast, and its frames reach the
 *      HOOK rather than the graph — the four gates that make "admitted, serving nothing"
 *      true in every direction;
 *   3. ACCEPT turns it into an ordinary peer and binds the subject;
 *   4. CONTINUE keeps it pending and carries the hook's reply back — the multi-round-trip
 *      shape a Noise handshake needs from the carrier;
 *   5. REJECT closes it with `kCloseAuthFailed`, code on the wire BEFORE the shutdown;
 *   6. the deadline closes a squatter with `kCloseAuthTimeout`, and only after it expires;
 *   7. an expired squatter cannot consume `max_peers` — the reap at the claim edge runs
 *      whether or not a tick has fired;
 *   8. a session that departs unauthenticated owes the routing plane no departure;
 *   9. the timer is retired by the destructor.
 *
 * What this suite deliberately does NOT claim: anything about a particular credential
 * scheme. The payload is opaque to the link by design, so what is provable here is the
 * SESSION behaviour around the hook — never that a token, a cookie or a Noise handshake
 * verifies correctly, which is the hook's business and belongs to whoever writes one.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "esp_timer.h"
#include "fake_httpd.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"

namespace {

using tr::net::httpd_ws_link_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief A credential blob — opaque to the link, which is the whole point. */
const std::byte kCredential[] = {std::byte{'t'}, std::byte{'o'}, std::byte{'k'}};
/** @brief A graph frame: whatever a served session would send after authenticating. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
/** @brief What a CONTINUE verdict hands back to the peer. */
const std::byte kChallenge[] = {std::byte{0xAA}, std::byte{0xBB}};

/** @brief Every payload the hook was handed since @ref reset_hook. */
std::vector<std::vector<std::byte>> g_seen;
/** @brief The verdict the hook answers; the LAST entry repeats once exhausted. */
std::vector<httpd_ws_link_t::auth_verdict_t> g_verdicts;
/** @brief Reply payload the hook attaches (empty = none). */
std::span<const std::byte> g_reply;
/** @brief Subject the hook binds on ACCEPT. */
std::string_view g_subject;
/** @brief The ctx the hook was handed, for the registration check. */
void* g_ctx = nullptr;

void reset_hook(std::vector<httpd_ws_link_t::auth_verdict_t> verdicts) {
    g_seen.clear();
    g_verdicts = std::move(verdicts);
    g_reply = {};
    g_subject = {};
    g_ctx = nullptr;
}

/** @brief The hook under test: record the payload, answer the scripted verdict. */
httpd_ws_link_t::auth_result_t recording_auth(void* ctx, std::span<const std::byte> payload) {
    g_ctx = ctx;
    g_seen.emplace_back(payload.begin(), payload.end());
    httpd_ws_link_t::auth_result_t res;
    const std::size_t at = g_seen.size() - 1;
    res.verdict = g_verdicts.empty()
                      ? httpd_ws_link_t::auth_verdict_t::REJECT
                      : g_verdicts[at < g_verdicts.size() ? at : g_verdicts.size() - 1];
    res.reply = g_reply;
    res.subject = g_subject;
    return res;
}

/** @brief How many peers the link currently exposes through the bus facet. */
std::size_t peer_count(const httpd_ws_link_t& link) {
    std::size_t n = 0;
    link.enumerate_peers([&n](std::string_view) { ++n; });
    return n;
}

/** @brief The frames the fake has recorded for @p fd since the last clear. */
std::vector<fake_httpd::server_t::sent_frame_t> frames_for(int fd) {
    std::vector<fake_httpd::server_t::sent_frame_t> out;
    for (auto& f : fake_httpd::instance().sent_frames())
        if (f.fd == fd) out.push_back(f);
    return out;
}

/** @brief Decode an RFC 6455 close payload's 2-byte status code (0 when there is none). */
std::uint16_t close_code(const fake_httpd::server_t::sent_frame_t& f) {
    if (f.payload.size() < 2) return 0;
    return static_cast<std::uint16_t>((static_cast<unsigned>(f.payload[0]) << 8) |
                                      static_cast<unsigned>(f.payload[1]));
}

/** @brief `peer_down_fn_t` thunk: count the routing plane's eviction calls. */
void count_peer_down(void* ctx, std::string_view) { ++*static_cast<int*>(ctx); }

/** @brief Drain the fake's control queue — the httpd task's own loop. */
void run_server() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief Let the monotonic clock advance past a deadline. The link dates its sessions from
 *         `esp_timer_get_time`, which the host fake maps to `steady_clock` — so a deadline
 *         test waits for real microseconds, briefly, rather than pretending to. */
void advance_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/** @brief Start every test from a known-empty fake. */
void reset_server() {
    fake_httpd::instance().close_all();
    run_server();
    fake_httpd::instance().clear_sent_frames();
}

/** @brief With no hook, the link behaves exactly as it did before this feature existed. */
void test_no_hook_serves_immediately() {
    std::printf("#1184 an unconfigured link is unchanged:\n");
    reset_server();
    const std::size_t timers_before = fake_esp_timer_live();
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    check(link->ok(), "the adopting link registered its URI");
    constexpr int kFd = 610;
    check(fake_httpd::instance().open_session(kFd), "the handshake was answered");
    check(fake_httpd::instance().deliver_frame(kFd, kBody) == ESP_OK, "its first frame dispatched");
    check(peer_count(*link) == 1, "and it was served at once — no credential asked for");
    check(fake_esp_timer_live() == timers_before,
          "no deadline sweep is armed on a link with no hook");
    fake_httpd::instance().close_session(kFd);
    run_server();
}

/** @brief The four gates: an unauthenticated session is admitted and served NOTHING. */
void test_pending_session_is_served_nothing() {
    std::printf("#1184 an unauthenticated session gets nothing:\n");
    reset_server();
    int ctx_object = 0;
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    link->set_auth_cb(&recording_auth, &ctx_object);
    reset_hook({httpd_ws_link_t::auth_verdict_t::CONTINUE});
    constexpr int kFd = 620;

    check(fake_httpd::instance().open_session(kFd), "the handshake was answered");
    check(fake_httpd::instance().deliver_frame(kFd, kCredential) == ESP_OK,
          "the first frame dispatched");
    // Gate 1: the frame went to the HOOK, not the graph.
    check(g_seen.size() == 1, "the hook was handed the first frame");
    if (g_seen.size() == 1)
        check(g_seen[0].size() == sizeof(kCredential) &&
                  std::memcmp(g_seen[0].data(), kCredential, sizeof(kCredential)) == 0,
              "with the credential bytes verbatim — opaque, undecoded");
    check(g_ctx == &ctx_object, "and the ctx registered alongside it");
    // Gates 2-4: invisible, unresolvable, and skipped by a broadcast.
    check(peer_count(*link) == 0, "it appears in NO peer census");
    check(link->peer_link("p0") == nullptr, "it cannot be resolved to a sending endpoint");
    fake_httpd::instance().clear_sent_frames();
    link->send(kBody);
    run_server();
    check(frames_for(kFd).empty(), "and a broadcast — a subscription push — skips it entirely");

    fake_httpd::instance().close_session(kFd);
    run_server();
}

/** @brief ACCEPT promotes the session and binds its subject. */
void test_accept_promotes_and_binds_subject() {
    std::printf("#1184 ACCEPT turns the session into a peer:\n");
    reset_server();
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    link->set_auth_cb(&recording_auth, nullptr);
    reset_hook({httpd_ws_link_t::auth_verdict_t::ACCEPT});
    g_subject = "ed25519:abcdef";
    constexpr int kFd = 630;

    check(fake_httpd::instance().open_session(kFd), "the handshake was answered");
    check(fake_httpd::instance().deliver_frame(kFd, kCredential) == ESP_OK,
          "the credential frame dispatched");
    check(peer_count(*link) == 1, "the session is now an enumerable peer");
    check(link->peer_link("p0") != nullptr, "and resolvable to a sending endpoint");

    std::string bound;
    link->enumerate_peer_stats(
        [&bound](const httpd_ws_link_t::peer_stats_t& s) { bound = std::string(s.subject); });
    check(bound == "ed25519:abcdef", "with the subject the hook bound published on it");

    // A SECOND frame is graph traffic now, not another credential.
    const std::size_t seen_before = g_seen.size();
    check(fake_httpd::instance().deliver_frame(kFd, kBody) == ESP_OK, "a later frame dispatched");
    check(g_seen.size() == seen_before, "and it went to the graph, never back to the hook");

    fake_httpd::instance().close_session(kFd);
    run_server();
}

/** @brief CONTINUE keeps the session pending and carries the hook's reply to the peer —
 *         the multi-round-trip shape the frame exists to be a carrier for. */
void test_continue_carries_a_reply() {
    std::printf("#1184 CONTINUE is a round trip, not a verdict:\n");
    reset_server();
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    link->set_auth_cb(&recording_auth, nullptr);
    reset_hook(
        {httpd_ws_link_t::auth_verdict_t::CONTINUE, httpd_ws_link_t::auth_verdict_t::ACCEPT});
    g_reply = std::span<const std::byte>(kChallenge);
    constexpr int kFd = 640;

    check(fake_httpd::instance().open_session(kFd), "the handshake was answered");
    fake_httpd::instance().clear_sent_frames();
    check(fake_httpd::instance().deliver_frame(kFd, kCredential) == ESP_OK,
          "the first handshake frame dispatched");
    const auto after_first = frames_for(kFd);
    check(after_first.size() == 1 && after_first[0].type == HTTPD_WS_TYPE_BINARY,
          "the hook's reply went back as one BINARY frame");
    if (after_first.size() == 1)
        check(after_first[0].payload.size() == sizeof(kChallenge) &&
                  std::memcmp(after_first[0].payload.data(), kChallenge, sizeof(kChallenge)) == 0,
              "carrying the challenge bytes verbatim");
    check(peer_count(*link) == 0, "and the session is STILL unauthenticated");

    check(fake_httpd::instance().deliver_frame(kFd, kCredential) == ESP_OK,
          "the second handshake frame dispatched");
    check(g_seen.size() == 2, "the hook saw both frames of the exchange");
    check(peer_count(*link) == 1, "and the ACCEPT on the second one promoted the session");

    fake_httpd::instance().close_session(kFd);
    run_server();
}

/** @brief REJECT closes the session, and the peer is TOLD which failure it was. */
void test_reject_closes_with_its_own_code() {
    std::printf("#1184 REJECT closes with kCloseAuthFailed:\n");
    reset_server();
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    link->set_auth_cb(&recording_auth, nullptr);
    reset_hook({httpd_ws_link_t::auth_verdict_t::REJECT});
    constexpr int kFd = 650;

    check(fake_httpd::instance().open_session(kFd), "the handshake was answered");
    fake_httpd::instance().clear_sent_frames();
    check(fake_httpd::instance().deliver_frame(kFd, kCredential) == ESP_OK,
          "the credential frame dispatched");

    const auto sent = frames_for(kFd);
    check(sent.size() == 1 && sent[0].type == HTTPD_WS_TYPE_CLOSE,
          "a CLOSE frame was written to the peer");
    if (sent.size() == 1)
        check(close_code(sent[0]) == httpd_ws_link_t::kCloseAuthFailed,
              "carrying kCloseAuthFailed — 'your credential was refused', not 'you were slow'");
    // The teeth: the code has to reach the wire BEFORE the shutdown, because after it every
    // write on the socket fails at once and the peer would learn nothing.
    check(fake_httpd::instance().is_shut(kFd), "and only then was the socket shut down");
    check(link->stats().auth_rejected == 1, "the refusal is counted as a REJECT, not a timeout");
    check(link->stats().auth_expired == 0, "and not as an expiry");
    check(peer_count(*link) == 0, "the session never became a peer");
    run_server();
}

/** @brief The deadline closes a squatter — and not before it expires. */
void test_deadline_closes_a_squatter() {
    std::printf("#1184 the auth deadline closes a silent session:\n");
    reset_server();
    // A deadline long enough that the first tick lands INSIDE it, so "not before it expires"
    // is a real observation rather than a race won by luck.
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true, 0, 400);
    link->set_auth_cb(&recording_auth, nullptr);
    reset_hook({httpd_ws_link_t::auth_verdict_t::CONTINUE});
    constexpr int kFd = 660;

    check(fake_httpd::instance().open_session(kFd), "the handshake was answered");
    check(fake_httpd::instance().deliver_frame(kFd, kCredential) == ESP_OK,
          "and the session claimed its slot, unauthenticated");
    fake_httpd::instance().clear_sent_frames();

    // A tick well inside the window must do nothing.
    check(fake_esp_timer_fire() == 1, "the sweep timer is armed");
    run_server();
    check(frames_for(kFd).empty(), "a tick inside the window closes nothing");
    check(link->stats().auth_expired == 0, "and counts nothing");

    // Past the deadline it must close, with its own code.
    advance_ms(450);
    check(fake_esp_timer_fire() == 1, "the timer fires again past the deadline");
    // ONE queued item — the sweep the tick posted — and no reap, so the socket's shut state
    // is still observable. run_pending() would close and drop the session in the same call,
    // which is httpd's own select arm doing its job and would hide the evidence.
    check(fake_httpd::instance().run_one(), "the tick marshalled a sweep onto the httpd task");
    const auto sent = frames_for(kFd);
    check(sent.size() == 1 && sent[0].type == HTTPD_WS_TYPE_CLOSE,
          "the expired session was sent a CLOSE");
    if (sent.size() == 1)
        check(close_code(sent[0]) == httpd_ws_link_t::kCloseAuthTimeout,
              "carrying kCloseAuthTimeout — distinguishable from a refused credential");
    check(fake_httpd::instance().is_shut(kFd), "and the socket was shut down");
    check(link->stats().auth_expired == 1, "counted as an expiry");
    check(link->stats().auth_rejected == 0, "and not as a refusal");
    run_server();
}

/** @brief An expired squatter must not be able to deny a real peer its slot — the whole
 *         reason the deadline exists, and the one defence that must not wait for a tick. */
void test_expired_squatter_cannot_consume_the_cap() {
    std::printf("#1184 an expired squatter does not hold the peer cap:\n");
    reset_server();
    // max_peers = 1: the squatter below is the ONLY slot, so a second peer gets in exactly
    // when the reap has freed it.
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 1, true, 0, 200);
    link->set_auth_cb(&recording_auth, nullptr);
    reset_hook({httpd_ws_link_t::auth_verdict_t::CONTINUE});
    constexpr int kSquatter = 670;
    constexpr int kReal = 671;

    check(fake_httpd::instance().open_session(kSquatter), "the squatter's handshake was answered");
    check(fake_httpd::instance().deliver_frame(kSquatter, kCredential) == ESP_OK,
          "and it claimed the only slot without authenticating");

    // Before the deadline the cap is genuinely spent — a real peer IS refused. That is the
    // exposure this feature introduces, and it is bounded rather than absent.
    check(fake_httpd::instance().open_session(kReal), "a real peer's handshake was answered");
    check(fake_httpd::instance().deliver_frame(kReal, kCredential) == ESP_FAIL,
          "but it is refused while the squatter's window is still open");

    advance_ms(250);
    // NO timer tick here. The reap at the claim edge is what has to run, because a peer
    // arriving between ticks must not be turned away by a session that is already expired.
    check(fake_httpd::instance().open_session(kReal + 1), "a later peer's handshake was answered");
    check(fake_httpd::instance().deliver_frame(kReal + 1, kCredential) == ESP_OK,
          "and past the deadline it is admitted — with no tick, by the claim-edge reap");
    check(link->stats().auth_expired == 1, "the squatter was reaped and counted");
    run_server();
}

/** @brief A session that leaves unauthenticated owes the routing plane no departure: it was
 *         never announced to it. */
void test_pending_departure_notifies_nobody() {
    std::printf("#1184 an unauthenticated departure notifies nobody:\n");
    reset_server();
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    link->set_auth_cb(&recording_auth, nullptr);
    reset_hook({httpd_ws_link_t::auth_verdict_t::CONTINUE});
    int downs = 0;
    link->set_peer_down_notifier(&count_peer_down, &downs);
    constexpr int kFd = 680;

    check(fake_httpd::instance().open_session(kFd), "the handshake was answered");
    check(fake_httpd::instance().deliver_frame(kFd, kCredential) == ESP_OK,
          "the session claimed a slot, unauthenticated");
    fake_httpd::instance().close_session(kFd);
    run_server();
    check(downs == 0, "its departure fired no eviction — the plane held no edge to evict");
}

/** @brief The two-valued predicate, for the overload-exclusivity check. */
bool admit_all(void*, httpd_req_t*) { return true; }

/** @brief The verdict @ref preauth_admission answers, and the ctx it was handed. */
httpd_ws_link_t::admission_verdict_t g_admission_verdict =
    httpd_ws_link_t::admission_verdict_t::ADMIT;
int g_admission_calls = 0;

/** @brief A three-valued admission predicate answering whatever the case scripted. */
httpd_ws_link_t::admission_verdict_t preauth_admission(void*, httpd_req_t*) {
    ++g_admission_calls;
    return g_admission_verdict;
}

/**
 * @brief #1245 — a handshake that authenticated its peer must not then be asked for a
 *        credential frame, because the peers that can present a header (a native dialer)
 *        are exactly the ones that cannot send a frame.
 */
void test_handshake_authentication_skips_the_frame() {
    std::printf("#1245 ADMIT_AUTHENTICATED serves a session with no credential frame:\n");
    reset_server();
    // A SHORT deadline, so "no deadline is armed for this session" is proven by a sweep the
    // session outlives rather than by reading a flag.
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true, 0, 200);
    link->set_admission_verdict_cb(&preauth_admission, nullptr);
    link->set_auth_cb(&recording_auth, nullptr);
    reset_hook({httpd_ws_link_t::auth_verdict_t::REJECT});
    g_admission_calls = 0;
    constexpr int kHeader = 690;  // authenticated at the handshake
    constexpr int kFramed = 691;  // admitted, must present a credential

    g_admission_verdict = httpd_ws_link_t::admission_verdict_t::ADMIT_AUTHENTICATED;
    check(fake_httpd::instance().open_session(kHeader), "the handshake was answered");
    check(g_admission_calls == 1, "the three-valued predicate was consulted once");
    check(fake_httpd::instance().deliver_frame(kHeader, kBody) == ESP_OK,
          "its first frame dispatched");
    check(g_seen.empty(), "and went to the GRAPH, not to the credential hook");
    check(peer_count(*link) == 1, "the session was served from its first frame");

    // The other verdict on the SAME link, so this is a per-session property and not a
    // link-wide off switch for the hook.
    g_admission_verdict = httpd_ws_link_t::admission_verdict_t::ADMIT;
    check(fake_httpd::instance().open_session(kFramed), "a plain ADMIT handshake was answered");
    check(fake_httpd::instance().deliver_frame(kFramed, kCredential) == ESP_OK,
          "its first frame dispatched");
    check(g_seen.size() == 1, "and went to the credential hook");
    check(link->stats().auth_rejected == 1, "which refused it");
    check(peer_count(*link) == 1, "leaving only the handshake-authenticated session served");

    // The deadline must not reach the pre-authenticated session.
    advance_ms(250);
    check(fake_esp_timer_fire() == 1, "the sweep timer fires past the deadline");
    run_server();
    check(link->stats().auth_expired == 0, "and expires nothing — no deadline was armed for it");
    check(peer_count(*link) == 1, "the handshake-authenticated session is still a peer");
    fake_httpd::instance().close_session(kHeader);
    run_server();
}

/** @brief A REFUSE verdict is the `false` of the two-valued predicate, and the two forms of
 *         the predicate are alternatives rather than a pair. */
void test_verdict_refuse_and_overload_exclusivity() {
    std::printf("#1245 REFUSE refuses, and the two predicate forms are exclusive:\n");
    reset_server();
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    link->set_admission_verdict_cb(&preauth_admission, nullptr);
    g_admission_calls = 0;
    g_admission_verdict = httpd_ws_link_t::admission_verdict_t::REFUSE;
    constexpr int kRefused = 692;
    check(!fake_httpd::instance().open_session(kRefused), "REFUSE abandoned the handshake");
    check(!fake_httpd::instance().has_session(kRefused), "no session was admitted");
    check(link->stats().peers_refused == 1, "and the refusal was counted");

    // Installing the two-valued form must CLEAR the three-valued one, or the link would
    // hold two predicates with no rule about which wins.
    link->set_admission_cb(&admit_all, nullptr);
    g_admission_calls = 0;
    constexpr int kAdmitted = 693;
    check(fake_httpd::instance().open_session(kAdmitted), "the bool predicate now decides");
    check(g_admission_calls == 0, "and the verdict predicate was not consulted at all");
    fake_httpd::instance().close_session(kAdmitted);
    run_server();
}

/** @brief The destructor retires the sweep timer. */
void test_teardown_retires_the_timer() {
    std::printf("#1184 teardown retires the sweep:\n");
    reset_server();
    const std::size_t before = fake_esp_timer_live();
    {
        auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
        link->set_auth_cb(&recording_auth, nullptr);
        check(fake_esp_timer_live() == before + 1, "installing a hook armed exactly one timer");
        link->set_auth_cb(&recording_auth, nullptr);
        check(fake_esp_timer_live() == before + 1, "a second install re-arms nothing");
    }
    check(fake_esp_timer_live() == before, "and the destructor deleted it");
}

}  // namespace

int main() {
    std::printf("httpd_ws_link post-handshake auth suite (#1184):\n");
    test_no_hook_serves_immediately();
    test_pending_session_is_served_nothing();
    test_accept_promotes_and_binds_subject();
    test_continue_carries_a_reply();
    test_reject_closes_with_its_own_code();
    test_deadline_closes_a_squatter();
    test_expired_squatter_cannot_consume_the_cap();
    test_pending_departure_notifies_nobody();
    test_teardown_retires_the_timer();
    test_handshake_authentication_skips_the_frame();
    test_verdict_refuse_and_overload_exclusivity();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
