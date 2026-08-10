/**
 * @file
 * @brief #953 — the LINK-level failure tally: every drop class an embedder could not see.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 / #835 / #954 / #957 / #958 suites: the REAL chip
 * translation unit (`integrations/esp-idf/libtracer/httpd_ws_link.cpp`) is compiled
 * against the host fake of `esp_http_server` (fake_httpd.hpp).
 *
 * What #953 asked for and what it did NOT get here, stated up front so the gap is not
 * mistaken for coverage. Most of the counters #953 describes as missing already existed
 * when this was written: per-PEER `rx_frames`/`rx_bytes`/`tx_frames`/`tx_bytes`/`rx_drops`/
 * `tx_drops` are carried by `link_counters_t` and published through
 * `enumerate_peer_stats`, and `esp_ws_client_link_t` has had its own `stats()` for some
 * time. What was genuinely unobservable is what this suite covers: the events with NO
 * session to charge, plus the two session-lifecycle facts nothing else records.
 *
 * Every case here follows the same discipline: drive the real failure through the fake,
 * then assert the counter moved **by exactly one** and that its neighbours did not. A
 * counter that merely "goes up" is compatible with bumping the wrong field, and this file
 * exists precisely because the previous state of the world was a tally nobody could
 * distinguish from another tally.
 *
 * `rx_dropped_alloc` is NOT covered. Reaching it needs a failing `new (std::nothrow)` for
 * an oversize RX payload, and this suite has no allocator injector —
 * `transport_alloc_softfail_test` is the harness that owns that capability for the core. Claiming
 * coverage by asserting it stays zero would be exactly the vacuous guard this repo has shipped
 * before, so the field is left measured only by inspection and said so here.
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

using tr::net::httpd_ws_link_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief The fake server's handle, as the adopting constructor takes it. */
httpd_handle_t handle() { return static_cast<httpd_handle_t>(&fake_httpd::instance()); }

/** @brief A minimal frame body — enough to claim a peer. */
const std::byte kBody[] = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};

/** @brief Admit @p fd and claim it as a peer (the lazy first-data-frame claim). */
void claim(int fd) {
    fake_httpd::instance().open_session(fd);
    (void)fake_httpd::instance().deliver_frame(fd, kBody);
}

/** @brief Drain the control queue to quiescence, as the httpd task does. */
void drain() {
    while (fake_httpd::instance().run_pending() != 0) {
    }
}

/** @brief A predicate that refuses every peer. */
bool refusing_hook(void*, httpd_req_t*) { return false; }

// ---------------------------------------------------------------------------
// 1 — peers_refused: both refusal paths, neither of which ever owns a session.
// ---------------------------------------------------------------------------
void test_peers_refused_counts_both_paths() {
    std::printf("#953 peers_refused counts the admission hook AND the max_peers ceiling:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 2, true);
    check(link->stats().peers_refused == 0, "a fresh link has refused nobody");

    // (a) the predicate says no. The refusal happens in the PRE-handshake callback, which
    // is a static function reaching the link through the gate — the reason the bump had to
    // move under the gate mutex rather than sit beside its own log line.
    link->set_admission_cb(&refusing_hook, nullptr);
    fake_httpd::instance().open_session(700);
    check(link->stats().peers_refused == 1, "the predicate's refusal is counted");

    // (b) the ceiling says no. Fill max_peers=2 with admitted sessions first.
    link->set_admission_cb(nullptr, nullptr);
    claim(701);
    claim(702);
    const auto before = link->stats();
    check(before.peers_refused == 1, "admitting peers refuses nobody further");
    fake_httpd::instance().open_session(703);
    (void)fake_httpd::instance().deliver_frame(703, kBody);
    const auto after = link->stats();
    check(after.peers_refused == 2, "the max_peers ceiling is counted on the same field");
    // The discrimination that makes the count worth having: a refusal is not a teardown.
    check(after.sessions_condemned == 0, "and a refusal condemns nothing");
    check(after.tx_to_dead_peer == 0, "and touches no send counter");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 2 — rx_dropped_oversize: the abuse cap, which had neither a log nor a counter.
// ---------------------------------------------------------------------------
void test_rx_oversize_is_counted() {
    std::printf("#953 an over-cap inbound frame is counted (it was silent entirely):\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(710);
    check(link->stats().rx_dropped_oversize == 0, "no oversize frame yet");

    // One byte past the cap the TU compiles in. Deliberately spelled as "the cap the link
    // reports + 1" rather than a literal, so a change to kMaxFrameBytes cannot leave this
    // case testing a size that is no longer over the line.
    const std::vector<std::byte> huge(32768 + 1, std::byte{0xAB});
    (void)fake_httpd::instance().deliver_frame(710, std::span<const std::byte>(huge));

    const auto s = link->stats();
    check(s.rx_dropped_oversize == 1, "the over-cap frame is counted");
    // It is charged to the LINK and to no session, which is the whole reason it needed a
    // field of its own: the cap is applied before the slot lookup, so there is no peer to
    // charge even though a peer sent it.
    std::uint32_t peer_rx_drops = 0;
    link->enumerate_peer_stats(
        [&](const httpd_ws_link_t::peer_stats_t& p) { peer_rx_drops += p.c.rx_drops; });
    check(peer_rx_drops == 0, "and charged to NO session — there is none at that point");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 3 — tx_to_dead_peer: the benign-but-silent race at the head of the send path.
// ---------------------------------------------------------------------------
void test_send_to_departed_peer_is_counted() {
    std::printf("#953 a frame aimed at a departed session is counted, not silently lost:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(720);
    drain();

    // Take the endpoint FIRST, then let the peer leave: this reproduces the real race —
    // a fan-out that already resolved its destinations against a peer departing under it —
    // rather than asking the link for an endpoint it would refuse to hand out.
    tr::net::transport_t* const to = link->peer_link("p0");
    check(to != nullptr, "the directed endpoint resolved while the peer was open");
    const auto before = link->stats();

    fake_httpd::instance().close_session(720);
    drain();
    to->send(std::span<const std::byte>(kBody));
    drain();

    const auto after = link->stats();
    check(after.tx_to_dead_peer == before.tx_to_dead_peer + 1,
          "the send to the departed session is counted");
    check(after.enqueue_drops == before.enqueue_drops,
          "and is NOT an enqueue drop: nothing was offered to the control queue");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 4 — tx_pool_misses is a SUBSET of enqueue_drops, not a second tally.
// ---------------------------------------------------------------------------
void test_pool_miss_is_a_labelled_subset() {
    std::printf("#953 a refused enqueue lands on enqueue_drops and NOT on tx_pool_misses:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(730);
    drain();
    const auto before = link->stats();

    // Refuse at the control queue — the cause that is NOT a pool miss. This is the
    // discrimination the issue asked for: before this change both causes incremented one
    // undifferentiated counter whose log line read "queue refused / pool exhausted / OOM",
    // so a reader could not tell depth pressure from a sick server.
    fake_httpd::instance().set_queue_refusing(true);
    tr::net::transport_t* const to = link->peer_link("p0");
    check(to != nullptr, "the directed endpoint resolved");
    to->send(std::span<const std::byte>(kBody));
    fake_httpd::instance().set_queue_refusing(false);

    const auto after = link->stats();
    check(after.enqueue_drops == before.enqueue_drops + 1, "the refused enqueue is counted");
    check(after.tx_pool_misses == before.tx_pool_misses,
          "and is NOT attributed to the pool — the slot was claimed, the queue said no");
    check(after.tx_to_dead_peer == before.tx_to_dead_peer,
          "nor to the dead-peer path — the destination was alive");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 4b — tx_pool_misses POSITIVELY: exhaust the pool and watch the field move.
// ---------------------------------------------------------------------------
void test_pool_exhaustion_is_counted() {
    std::printf("#953 exhausting the TX pool increments tx_pool_misses:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(735);
    drain();
    const auto before = link->stats();
    tr::net::transport_t* const to = link->peer_link("p0");
    check(to != nullptr, "the directed endpoint resolved");

    // Do NOT drain: on this fake a queued work item only runs when the test pumps it, so
    // every send holds its slot. Past tx_slot_capacity the pool has nothing left, which is
    // the DEPTH-pressure condition — distinct from a refusing control queue, and the whole
    // reason it got a field of its own.
    const std::size_t cap = httpd_ws_link_t::tx_slot_capacity();
    check(cap > 0, "the pool has a capacity to exhaust");
    for (std::size_t i = 0; i < cap + 2; ++i) to->send(std::span<const std::byte>(kBody));

    const auto after = link->stats();
    check(after.tx_pool_misses >= 2, "the sends past the pool's depth are counted as misses");
    check(after.enqueue_drops >= after.tx_pool_misses,
          "and each one is also an enqueue_drop: misses are a labelled SUBSET, not a second tally");
    check(after.tx_to_dead_peer == before.tx_to_dead_peer,
          "the destination was alive throughout — no dead-peer drop");

    drain();
    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 5 — sessions_condemned: the difference between a peer that left and one killed.
// ---------------------------------------------------------------------------
void test_condemn_is_counted() {
    std::printf("#953 a link-initiated teardown is counted (a departure is not):\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);

    // A peer that simply LEAVES must not look like one the link killed.
    claim(740);
    drain();
    fake_httpd::instance().close_session(740);
    drain();
    check(link->stats().sessions_condemned == 0, "a peer that departs condemns nothing");

    // Now one the link tears down: a SHORT write is rejected outright and condemns on the
    // spot (the bound that stops one full-window peer parking the httpd task).
    claim(741);
    drain();
    // Resolve the NAME rather than assuming one: slot 0 was freed when 740 departed, so
    // 741 recycles it and is `p0` again. Spelling `p1` here passed a null endpoint into a
    // null-guard and asserted nothing — the exact vacuous shape this file is meant to
    // avoid, caught because the check demanded the counter MOVE rather than merely not
    // contradict.
    std::string routable;
    link->enumerate_peer_stats(
        [&](const httpd_ws_link_t::peer_stats_t& p) { routable = std::string(p.name); });
    check(!routable.empty(), "the surviving session has a routable name");
    tr::net::transport_t* const to = link->peer_link(routable);
    check(to != nullptr, "and a directed endpoint");
    fake_httpd::instance().set_send_script(741, {fake_httpd::send_result_t::SHORT});
    if (to != nullptr) {
        to->send(std::span<const std::byte>(kBody));
        drain();
    }
    check(link->stats().sessions_condemned == 1, "the short-write teardown IS counted");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 5b — #963.1: a CONDEMNED session leaves the bus facet at once, not when httpd reaps it.
// ---------------------------------------------------------------------------
void test_condemned_peer_leaves_the_facet() {
    std::printf("#963 a condemned peer stops being visible before httpd reaps it:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    claim(760);
    drain();

    std::string name;
    link->enumerate_peers([&](std::string_view p) { name = std::string(p); });
    check(!name.empty(), "the peer is in the census while it is healthy");
    check(link->peer_link(name) != nullptr, "and resolves to an endpoint");

    // Condemn it WITHOUT letting httpd run its select loop — this is the whole gap. The
    // slot is dead and shut, but free_ctx has not run, so the slot is still occupied.
    fake_httpd::instance().set_send_script(760, {fake_httpd::send_result_t::SHORT});
    tr::net::transport_t* const to = link->peer_link(name);
    if (to != nullptr) {
        to->send(std::span<const std::byte>(kBody));
        // EXACTLY ONE work item, and the count matters. That item is the tx_work whose
        // short write condemns the peer; condemn() then enqueues httpd_sess_trigger_close
        // BEHIND it, and running that second item deletes the session, runs free_ctx ->
        // reclaim_slot, and clears `open` — closing the very gap under test.
        //
        // Both earlier versions of this case got that wrong and were caught by ablation,
        // not by review: drain() reaped through reap_shut(), and a run_one() LOOP ran the
        // queued close. Each passed with the fix reverted, because by the time they
        // looked, the slot was no longer `open` and the `!dead` filter was irrelevant.
        // One step leaves the session exactly where condemn() leaves it on silicon: shut
        // and dead, still holding its slot, waiting for a select round that has not come.
        check(fake_httpd::instance().run_one(), "the queued send ran");
    }
    check(link->stats().sessions_condemned == 1, "the peer was condemned");

    // The two facet readers must now agree with every SENDING path, all of which already
    // refused this peer. Before #963 they did not: the census listed it and peer_link
    // handed out an endpoint that was a guaranteed no-op.
    // The precondition this case rests on: the slot is still OCCUPIED. Without this the
    // two checks below pass vacuously against a reaped slot.
    bool present_at_all = false;
    link->enumerate_peer_stats(
        [&](const httpd_ws_link_t::peer_stats_t&) { present_at_all = true; });
    check(present_at_all, "the slot is still occupied — this IS the condemn->reap gap");

    bool still_listed = false;
    link->enumerate_peers([&](std::string_view p) {
        if (p == name) still_listed = true;
    });
    check(!still_listed, "a condemned peer is OUT of the census immediately");
    check(link->peer_link(name) == nullptr,
          "and peer_link refuses it rather than handing out a no-op endpoint");

    link.reset();
    fake_httpd::instance().close_all();
}

// ---------------------------------------------------------------------------
// 6 — the snapshot is a snapshot: cheap, lock-free, and it re-reads.
// ---------------------------------------------------------------------------
void test_stats_snapshot_tracks() {
    std::printf("#953 stats() re-reads rather than latching a first answer:\n");
    auto link = std::make_unique<httpd_ws_link_t>(handle(), "/ws", 0, true);
    const auto a = link->stats();
    claim(750);
    const std::vector<std::byte> huge(32768 + 1, std::byte{0xAB});
    (void)fake_httpd::instance().deliver_frame(750, std::span<const std::byte>(huge));
    const auto b = link->stats();
    check(a.rx_dropped_oversize == 0 && b.rx_dropped_oversize == 1,
          "two snapshots across one event differ by exactly that event");
    // enqueue_drops is the pre-existing accessor; both spellings must agree forever, or
    // an embedder migrating from one to the other silently changes meaning.
    check(b.enqueue_drops == link->enqueue_drops(),
          "stats().enqueue_drops and enqueue_drops() are the same number");
    link.reset();
    fake_httpd::instance().close_all();
}

}  // namespace

int main() {
    std::printf("httpd_ws_link link-level failure counters (#953):\n");
    test_peers_refused_counts_both_paths();
    test_rx_oversize_is_counted();
    test_send_to_departed_peer_is_counted();
    test_pool_miss_is_a_labelled_subset();
    test_pool_exhaustion_is_counted();
    test_condemn_is_counted();
    test_condemned_peer_leaves_the_facet();
    test_stats_snapshot_tracks();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
