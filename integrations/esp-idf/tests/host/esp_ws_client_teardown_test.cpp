/**
 * @file
 * @brief #952 + #1058 — the WS *client* link's BOUNDED-BLOCKING discipline: what a send
 *        may cost, what a teardown may cost, and who may still be inside the transport
 *        when the handles are destroyed.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #900/#901 receive-path suite next to it: the REAL chip
 * translation unit (`integrations/esp-idf/libtracer/esp_ws_client_link.cpp`) compiled
 * against a host fake of `esp_transport_ws` (fake_esp_transport.hpp). That suite scripts
 * the fake's inbound frames; this one scripts its BLOCKING — a dial that hangs, a dial
 * that fails, a write that parks — because every defect here is a wait nobody bounded.
 * No socket is opened at any layer (#947).
 *
 * The three defects, all on one seam (`write_m_` and `stop_` were treated as if the
 * operations under them were prompt):
 *
 *   1. `send()` held `write_m_` across up to 3 x 4000 ms of transport I/O. IDF's
 *      `_ws_write` spends the caller's timeout on a poll, then the header write, then
 *      the payload write, so the literal was a 12 s stall against a 5 s task watchdog —
 *      a panic, not a dropped frame. `write_m_` is also the recv thread's read
 *      serializer, so a stalled link could not even re-dial itself.
 *   2. `stop_` was read once per loop turn and by nothing inside the turn: the 1.5 s
 *      reconnect backoff was a plain `sleep_for`, so the destructor's join waited out
 *      the full backoff of exactly the unreachable peer a re-dial exists for.
 *   3. The destructor destroyed the transport handles with `write_m_` held NOWHERE, on
 *      the premise that the joined recv thread was the only handle user — which the
 *      header's own contract contradicts ("`send()` may be called from ANY task"). A
 *      sender queued behind a stalled write woke up owning a destroyed handle.
 *
 * What makes defect 3 observable rather than a code reading: the fake tracks handle
 * LIVENESS, and its parked write re-checks it on the way out. The real
 * `esp_transport_write` dereferences the handle throughout the call, so a handle
 * destroyed while a writer is inside it is a use-after-free even though the pointer was
 * live on entry — `fake_ws::handle_misuse()` counts exactly that, plus every call made
 * on a null or already-destroyed handle.
 *
 * A fourth case guards the seam the FIX itself has to respect: the transport handles are
 * published by the `connected_` release store, not by the send serializer, because the
 * recv thread rebuilds them on every re-dial holding no lock. A sender that reads a
 * handle ahead of that acquire gate races the rebuild whatever lock it holds — so that
 * case drives sends straight through a failing-dial cycle, for the `tsan` leg to judge.
 *
 * #1058 adds the residual #952 deliberately left standing: a teardown that lands MID-DIAL.
 * `esp_transport_connect` takes no cancellation — that is IDF's documented surface, not a
 * guess — so the dial cannot be stopped, and the ruling is to stop CHARGING the destroying
 * task for it. Four cases hold the resulting contract, all on the fake's now-resolvable
 * hung dial (`fake_ws::release_connects`) and its per-handle liveness registry: the
 * destroying task pays no dial bound and a fresh link is admitted at once while the orphan
 * is still in flight; an orphan that resolves SUCCESS closes and releases; one that
 * resolves FAILURE releases; and neither side of a teardown/resolution race releases twice.
 * `fake_ws::live_handles()` is the release oracle and `fake_ws::handle_misuse()` the
 * double-release one. Every case DRAINS before it returns, which is not tidiness: a
 * condemned dial's recv thread is DETACHED, so one still running at process exit is a
 * crash under ASan/TSan and a leak under LSan.
 *
 * What this suite does NOT claim: that a `send()` STARTING after the destructor has
 * returned is safe, nor that one which entered `send()` but has not yet raised the
 * in-flight tally is. The tally is the boundary the destructor drains, and no barrier
 * inside an object can draw it earlier — the embedder owns the link's lifetime against
 * its own callers.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_esp_transport.hpp"
#include "libtracer_esp/esp_ws_client_link.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::esp_ws_client_link_t;

/** @brief Failed-check counter; main() turns it into the exit status. */
int g_failures = 0;

/** @brief Record one assertion's verdict. */
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief Record a verdict that also reports the two numbers that decided it. */
void check_le(long long got, long long ceiling, std::string_view what) {
    const bool ok = got <= ceiling;
    std::printf("  [%s] %.*s (got %lld, ceiling %lld)\n", ok ? "PASS" : "FAIL",
                static_cast<int>(what.size()), what.data(), got, ceiling);
    if (!ok) ++g_failures;
}

/** @brief Record a verdict that reports the millisecond reading behind it. */
void check_under_ms(long long got, long long ceiling, std::string_view what) {
    const bool ok = got < ceiling;
    std::printf("  [%s] %.*s (took %lld ms, ceiling %lld ms)\n", ok ? "PASS" : "FAIL",
                static_cast<int>(what.size()), what.data(), got, ceiling);
    if (!ok) ++g_failures;
}

/** @brief Milliseconds since @p start, as a plain integer. */
long long ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                 start)
        .count();
}

/** @brief Spin until @p pred holds or @p limit elapses. @return whether it held. */
template <typename pred_t>
bool wait_until(pred_t pred, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

/**
 * @brief The watchdog window the HOST build derives its bounds from, milliseconds.
 *
 * `esp_ws_client_link.cpp` reads `CONFIG_ESP_TASK_WDT_TIMEOUT_S` when sdkconfig.h
 * defines it and falls back to IDF's own Kconfig default otherwise. There is no
 * sdkconfig on the host, so this build takes the fallback — five seconds. The same
 * fallback `twai_link.cpp` and `httpd_ws_link.cpp` take in their host suites.
 */
constexpr long long kHostWdtMs = 5000;

/** @brief Rx/tx buffer size every case here builds its link with. */
constexpr std::size_t kBufBytes = 256;

/** @brief IDF spends the write timeout on three legs inside ONE esp_transport_write
 *         (poll_write, the WS header write, the payload write — transport_ws.c). */
constexpr long long kIdfWriteLegs = 3;

/** @brief A small outbound frame — the size of the control TLVs this link carries. */
std::vector<std::byte> payload() {
    std::vector<std::byte> f;
    for (int i = 0; i < 8; ++i) f.push_back(static_cast<std::byte>(0xB0 + i));
    return f;
}

/** @brief Build a link on the fake, WITHOUT asserting anything — the quiet half of
 *         @ref dialing_link, for the 50-iteration race case whose per-turn verdicts
 *         would otherwise bury the report. */
std::unique_ptr<esp_ws_client_link_t> quiet_link() {
    return std::make_unique<esp_ws_client_link_t>(
        "127.0.0.1", 8080, "/ws", /*handshake_headers=*/std::string{}, kBufBytes, kBufBytes, 0);
}

/** @brief Build a link on the fake and wait for its first dial. */
std::unique_ptr<esp_ws_client_link_t> dialing_link() {
    auto link = quiet_link();
    check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s), "the link dialed");
    return link;
}

/**
 * @brief Wait for the fake to hold NO transport handle and NO parked dialer.
 *
 * Every case that orphans a dial must end here, and not as tidiness: the orphaned recv
 * thread is DETACHED, so a case that returns while it is still inside the transport
 * leaves a running thread at process exit (a crash under ASan/TSan) and an unreleased
 * transport pair (a leak under LSan). It is also the release oracle itself — "the bound
 * was released exactly once" is `live_handles() == 0` with `handle_misuse() == 0`.
 *
 * The wait itself now lives on the fake (`fake_ws::wait_drained`, #1456) so the other
 * client suites — which had the same exposure and none of the drain — share it.
 */
bool drained_fake(std::chrono::milliseconds limit = 5s) { return fake_ws::wait_drained(limit); }

/**
 * @brief The headline (defect 3): teardown must not destroy the handles under a sender.
 *
 * Two senders are staged against a peer that has stopped accepting bytes: one INSIDE
 * `esp_transport_write` (holding the send serializer for the whole call) and one queued
 * behind it on that serializer. Pre-fix the destructor joined the recv thread and then
 * destroyed both handles with the serializer held nowhere, so it returned while the
 * first sender was still inside the transport and the second woke up to write through a
 * destroyed (or freshly nulled) handle.
 */
void test_teardown_does_not_destroy_handles_under_a_sender() {
    std::printf("teardown does not destroy the handles under a sender:\n");
    fake_ws::reset();
    fake_ws::hold_writes(true);

    auto link = dialing_link();
    check(wait_until([&] { return link->link_up(); }, 2s), "and came up connected");
    const std::vector<std::byte> frame = payload();

    // The senders hold the LINK, never the owning unique_ptr: `link.reset()` writes that
    // smart pointer's stored address while a `link->` in a sender thread would be reading
    // it, which is a race on the test's own bookkeeping (the #962 suite hit exactly this
    // under TSan). The link object itself is safe to hold — that is the contract staged.
    esp_ws_client_link_t* const under_test = link.get();

    std::atomic<int> seq{0};
    std::atomic<int> a_at{0};  // sequence position at which sender A returned
    std::atomic<int> b_at{0};  // ... B ...
    std::atomic<int> d_at{0};  // ... and at which the destructor returned

    std::thread a([&] {
        under_test->send(frame);
        a_at.store(++seq, std::memory_order_release);
    });
    check(wait_until([] { return fake_ws::writers_inside() >= 1; }, 2s),
          "sender A is parked INSIDE the transport, holding the send serializer");

    std::thread b([&] {
        under_test->send(frame);
        b_at.store(++seq, std::memory_order_release);
    });
    // B cannot announce itself from inside the link, so it is placed by time and then
    // pinned by what the fake did NOT see: A holds the serializer, so a B that has
    // reached the transport would show up as a second entered write. Short, because
    // every millisecond spent here is a millisecond of A's park already gone.
    std::this_thread::sleep_for(50ms);
    check(fake_ws::writes_started() == 1,
          "sender B is queued on the serializer, not in the transport");

    // Nothing releases the peer: A leaves when the LINK's OWN write bound expires, which
    // is the whole point. Post-fix that is ~416 ms (case 4 pins the derivation) and the
    // destructor must sit through the remainder of it; pre-fix it was 4000 ms, and the
    // destructor sailed past it at the join (a ~200 ms poll turn) and destroyed the
    // handles with A still inside the transport.
    const auto start = std::chrono::steady_clock::now();
    link.reset();
    d_at.store(++seq, std::memory_order_release);
    const long long elapsed = ms_since(start);
    a.join();
    b.join();

    const int a_pos = a_at.load(std::memory_order_acquire);
    const int b_pos = b_at.load(std::memory_order_acquire);
    const int d_pos = d_at.load(std::memory_order_acquire);
    std::printf("       return order: A=%d B=%d destructor=%d, teardown %lld ms\n", a_pos, b_pos,
                d_pos, elapsed);
    check(a_pos != 0 && a_pos < d_pos,
          "the destructor did not return while a sender was inside the transport");
    check(b_pos != 0 && b_pos < d_pos, "nor while one was queued on the serializer");
    check(fake_ws::handle_misuse() == 0,
          "and no operation touched a destroyed, foreign or null handle");
    check(fake_ws::writes_started() == 1,
          "the queued sender left without writing through the torn-down handle");
    // Waiting for a sender is not licence to wait forever: what the destructor inherits
    // is one write bound, and that bound is now a quarter of a watchdog window.
    check_under_ms(elapsed, kHostWdtMs / 2,
                   "and the wait it inherited was one write bound, not an open one");
}

/**
 * @brief Defect 2: the reconnect backoff is a bound, not a sentence.
 *
 * With every dial failing, the recv thread spends its life in the backoff. Pre-fix that
 * was `sleep_for(1500)`, which the destructor's join inherited whole — on exactly the
 * unreachable peer a re-dial exists to recover from.
 */
void test_teardown_does_not_wait_out_the_reconnect_backoff() {
    std::printf("teardown does not wait out the reconnect backoff:\n");
    fake_ws::reset();
    fake_ws::fail_connects(true);

    auto link = dialing_link();
    check(!link->link_up(), "the dial failed, so the link is down and backing off");
    check(!link->ok(), "and it never came up (#1203: ok() is the came-up latch)");

    const auto start = std::chrono::steady_clock::now();
    link.reset();
    const long long elapsed = ms_since(start);

    // The backoff is 1500 ms and the recv thread enters it immediately after the failed
    // dial, so pre-fix this reading is the whole of it.
    check_under_ms(elapsed, 500, "the destructor cut the 1500 ms backoff short");
    check(fake_ws::handle_misuse() == 0, "and nothing touched a destroyed handle on the way out");
}

/**
 * @brief #1058 (a): a teardown that lands mid-dial charges the destroying task NOTHING,
 *        and the next link's dial is admitted while the orphan is still in flight.
 *
 * The residual #952 left, and the one this case now denies. `esp_transport_connect` takes
 * no cancellation — that is the documented IDF contract, not a guess — so the dial itself
 * cannot be stopped. What CAN be stopped is charging the destroying task for it: the
 * destructor condemns the in-flight dial and DETACHES the recv thread instead of joining
 * it, and the orphaned dial releases its own transport pair when it eventually resolves.
 *
 * The ceiling is DERIVED from the link's own published policy (`timing().poll_ms`, the
 * knob that genuinely bounds a teardown now) plus slack for a loaded runner — never a
 * literal, and never the dial bound. Against the pre-fix TU this case reads one whole
 * `timing().dial_timeout_ms` (2500 ms at the host's 5 s watchdog fallback) and FAILS.
 *
 * The second half is invariant 2 of the ruling: dial admission is per-link and lives in
 * the link's own slot, so nothing anywhere holds a bound on the orphan's account — a link
 * constructed during the orphan's wait dials AT ONCE (`connect_count()` 1 -> 2 while
 * `dialers_inside()` still counts the orphan).
 */
void test_a_teardown_that_lands_mid_dial_costs_no_dial_bound() {
    std::printf("a teardown that lands mid-dial costs no dial bound:\n");
    fake_ws::reset();
    fake_ws::hang_connects(true);

    auto link = dialing_link();
    const esp_ws_client_link_t::timing_t t = esp_ws_client_link_t::timing();
    std::printf("       the dial asked for %d ms\n", fake_ws::last_connect_timeout_ms());

    const auto start = std::chrono::steady_clock::now();
    link.reset();
    const long long elapsed = ms_since(start);

    // Derived, both of them: the dominant teardown term is now one poll turn (the recv
    // thread's stop-observation cadence), and 50 ms is runner slack, not policy.
    check_under_ms(elapsed, t.poll_ms + 50,
                   "the destroying task paid a poll turn, not a dial bound");
    check_under_ms(elapsed, t.dial_timeout_ms / 4,
                   "not a quarter of the dial bound it used to inherit either");
    check(fake_ws::dialers_inside() >= 1,
          "while the orphaned dial is still inside esp_transport_connect, as it must be");

    // Invariant 2: a NEW link dials immediately — nothing is held on the orphan's account.
    auto second = quiet_link();
    check(wait_until([] { return fake_ws::connect_count() >= 2; }, 2s),
          "a link constructed during the orphan's wait was admitted at once");
    std::printf("       %d dial(s) entered, %d still parked inside the transport\n",
                fake_ws::connect_count(), fake_ws::dialers_inside());
    check(fake_ws::dialers_inside() >= 1, "with the orphan's dial still unresolved");

    // The two numbers this case exists to produce, in the form #183's readiness checklist
    // carries them. Derived from the link's own published policy, so they follow the
    // watchdog rather than a literal written here.
    const long long write_budget = static_cast<long long>(t.write_timeout_ms) * kIdfWriteLegs;
    std::printf(
        "       #183: worst-case teardown %lld ms -> %lld ms (dial+write -> poll+write);"
        " mid-dial %d ms -> %lld ms\n",
        t.dial_timeout_ms + write_budget, t.poll_ms + write_budget, t.dial_timeout_ms, elapsed);

    fake_ws::release_connects(/*succeed=*/true);
    second.reset();
    check(drained_fake(), "and both the orphan and the live link released their pairs");
    check(fake_ws::handle_misuse() == 0, "with no operation on a destroyed or foreign handle");
}

/**
 * @brief #1058 (b): an orphaned dial that resolves SUCCESS closes and releases, once.
 *
 * Invariant 1's first arm. The blocking call still runs to completion — we only fix who
 * pays — so when it comes back holding a connected transport pair belonging to a link
 * that no longer exists, that pair must be CLOSED (there is a socket on the peer's side
 * of it) and destroyed, exactly once. `close_count()` is the "closed, not merely
 * destroyed" oracle; `live_handles()` is the release oracle; `handle_misuse()` is the
 * double-release oracle.
 */
void test_an_orphaned_dial_that_succeeds_closes_and_releases_once() {
    std::printf("an orphaned dial that resolves SUCCESS closes and releases once:\n");
    fake_ws::reset();
    fake_ws::hang_connects(true);

    auto link = dialing_link();
    const int closes_before = fake_ws::close_count();
    link.reset();

    check(fake_ws::dialers_inside() == 1, "the dial outlived its link, still in flight");
    check(fake_ws::live_handles() == 2, "still holding the transport pair it built");

    fake_ws::release_connects(/*succeed=*/true);
    check(drained_fake(), "and released it the moment the handshake resolved");
    std::printf("       %d close(s) on the way out, %d handle(s) still live\n",
                fake_ws::close_count() - closes_before, fake_ws::live_handles());
    check(fake_ws::close_count() > closes_before, "closing the connection it had just made");
    check(fake_ws::handle_misuse() == 0, "exactly once — nothing was released twice");
}

/**
 * @brief #1058 (c): an orphaned dial that resolves FAILURE releases, once.
 *
 * Invariant 1's second arm, and the common case on silicon: the peer was unreachable all
 * along and the dial comes back with the bound spent. Same release, same oracle.
 */
void test_an_orphaned_dial_that_fails_releases_once() {
    std::printf("an orphaned dial that resolves FAILURE releases once:\n");
    fake_ws::reset();
    fake_ws::hang_connects(true);

    auto link = dialing_link();
    link.reset();
    check(fake_ws::live_handles() == 2, "the orphaned dial holds the transport pair");

    fake_ws::release_connects(/*succeed=*/false);
    check(drained_fake(), "and released it when the dial failed");
    check(fake_ws::handle_misuse() == 0, "exactly once — nothing was released twice");
}

/**
 * @brief #1058 (d): teardown RACING the dial's resolution releases exactly once, always.
 *
 * The two sides decide the same thing — who owns the transport pair — from two threads,
 * so the case that matters is the one where they decide it at the same instant. Either
 * the condemn wins (the orphan releases the pair) or the resolve wins (the link adopts it
 * and the ordinary destructor releases it), and there is no interleaving in which both do
 * or neither does. `handle_misuse()` counts the "both" (a second destroy on a dead
 * handle); the drain catches the "neither".
 *
 * Fifty turns, and BOTH arms are covered on purpose rather than left to the scheduler:
 * a free-running race is won by the condemn essentially every time (the destructor needs
 * two uncontended mutex acquires; the resolve needs a parked thread to be woken first), so
 * the odd turns settle the resolution FIRST — waited for on an observable, never slept
 * for — and tear down a link that adopted its pair the ordinary way. The even turns are the
 * genuine race. The outcome the dial resolves to alternates too, since success and failure
 * release along different paths.
 */
void test_teardown_racing_the_dials_resolution_releases_once() {
    std::printf("teardown racing the dial's resolution releases exactly once:\n");
    int no_dial = 0, not_drained = 0, misused = 0, adopted = 0, raced = 0;
    constexpr int kTurns = 50;
    for (int i = 0; i < kTurns; ++i) {
        fake_ws::reset();
        fake_ws::hang_connects(true);
        auto link = quiet_link();
        if (!wait_until([] { return fake_ws::connect_count() >= 1; }, 2s)) ++no_dial;
        if (i % 2 == 0) {
            // The resolution is issued from a FOREIGN thread while the destructor runs, so
            // the interleaving is the scheduler's, not the test's.
            std::thread resolver([i] { fake_ws::release_connects(i % 4 == 0); });
            link.reset();
            resolver.join();
            ++raced;
        } else {
            // The other arm: the dial resolves BEFORE the teardown, so the link adopts the
            // pair and the ordinary destructor releases it. Waited for on the link's own
            // came-up latch — the fact that says the resolve landed — because a sleep here
            // would be a guess at the same thing.
            fake_ws::release_connects(/*succeed=*/true);
            if (wait_until([&] { return link->ok(); }, 2s)) ++adopted;
            link.reset();
        }
        if (!drained_fake()) ++not_drained;
        if (fake_ws::handle_misuse() != 0) ++misused;
    }
    std::printf(
        "       %d turns (%d raced, %d resolved-then-torn-down): %d without a dial, %d that"
        " did not drain, %d with misuse\n",
        kTurns, raced, adopted, no_dial, not_drained, misused);
    check(no_dial == 0, "every turn got its dial in flight before the teardown");
    check(adopted == kTurns / 2, "every resolve-first turn was adopted by its link");
    check(not_drained == 0, "every turn released the transport pair");
    check(misused == 0, "and no turn released one twice, on either side of the race");
}

/**
 * @brief The handle read is behind the `connected_` GATE, not merely behind the mutex.
 *
 * With every dial failing, the recv thread lives in `connect_once()`: it destroys and
 * rewrites `ws_`/`tcp_` on every turn holding NO lock, and `connected_` stays false the
 * whole time — so no release/acquire edge ever forms between that rewrite and a sender.
 * A `send()` that reads the handle AHEAD of the `connected_` acquire gate is therefore an
 * unsynchronised read of a live rewrite no matter what it holds `write_m_` for, because
 * the rewriting thread does not take `write_m_`. That is the same unsynchronised handle
 * read #952 is about, seen from the sender's side.
 *
 * This case drives exactly the interleaving a subscription push produces against an
 * unreachable peer: senders on foreign tasks calling into a link that is cycling failed
 * dials. TSan is the oracle for the ordering itself (this suite is in the ctest set the
 * `tsan` leg runs, and the read is reported against `connect_once`'s previous write even
 * when the two do not overlap in time — there is no edge either way). What the case pins
 * WITHOUT a sanitizer: the gate turns every sender back, so a down link admits nothing to
 * the transport at all, and nothing reaches a null or destroyed handle.
 */
void test_send_during_a_redial_reads_no_handle() {
    std::printf("a send during a re-dial reads no handle:\n");
    fake_ws::reset();
    fake_ws::fail_connects(true);  // the recv thread cycles connect_once() + backoff

    auto link = dialing_link();
    check(!link->link_up(), "the link is down and re-dialing");
    const int dials_before = fake_ws::connect_count();

    // Two foreign tasks pushing at a link that is rebuilding its handles underneath them.
    std::atomic<bool> run{true};
    std::atomic<int> sent{0};
    const std::vector<std::byte> frame = payload();
    esp_ws_client_link_t* const under_test = link.get();
    std::vector<std::thread> senders;
    for (int i = 0; i < 2; ++i) {
        senders.emplace_back([&] {
            while (run.load(std::memory_order_acquire)) {
                under_test->send(frame);
                sent.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    // Run until the recv thread has rebuilt the handles at least twice MORE, so the
    // senders straddle real rewrites rather than a single quiet window. The backoff is
    // 1500 ms, so this is a few seconds; the ceiling only stops a wedged run.
    const bool redialed =
        wait_until([&] { return fake_ws::connect_count() >= dials_before + 2; }, 12s);
    run.store(false, std::memory_order_release);
    for (std::thread& s : senders) s.join();

    const int dials = fake_ws::connect_count() - dials_before;
    std::printf("       %d sends across %d further dials\n", sent.load(std::memory_order_relaxed),
                dials);
    check(redialed && dials >= 2, "the handles were rebuilt under the senders, repeatedly");
    check(sent.load(std::memory_order_relaxed) > 0, "and the senders actually called send()");
    check(fake_ws::writes_started() == 0,
          "no send reached the transport while the link was down — the gate turned them back");
    check(fake_ws::handle_misuse() == 0, "and none touched a null or destroyed handle");
    link.reset();
}

/**
 * @brief Defect 1: both blocking bounds are derived from the task-watchdog period.
 *
 * The numbers the link hands IDF are the whole observable — no wait has to be spent to
 * read them. Pre-fix they were bare literals: 4000 ms per write leg (12 s of stall on a
 * closed TCP window) and 5000 ms per dial, against a 5 s watchdog.
 *
 * The ordinary send is the control: bounding the write must not stop the bytes getting
 * out, and the re-check the fix added after the lock (`stop_`, then the `connected_`
 * gate) must not swallow a frame on a live link.
 */
void test_the_blocking_bounds_are_derived_from_the_watchdog() {
    std::printf("the blocking bounds are derived from the watchdog period:\n");
    fake_ws::reset();

    auto link = dialing_link();
    check(wait_until([&] { return link->link_up(); }, 2s), "and came up connected");

    const std::vector<std::byte> frame = payload();
    link->send(frame);

    const long long write_ms = fake_ws::last_write_timeout_ms();
    const long long dial_ms = fake_ws::last_connect_timeout_ms();
    std::printf("       write %lld ms x %lld legs, dial %lld ms, watchdog %lld ms\n", write_ms,
                kIdfWriteLegs, dial_ms, kHostWdtMs);

    check(write_ms > 0 && dial_ms > 0, "both bounds are real waits, not zero");
    check_le(write_ms * kIdfWriteLegs, kHostWdtMs / 2,
             "one send's TOTAL write bound is at most half a watchdog window");
    check_le(dial_ms, kHostWdtMs / 2, "one dial is at most half a watchdog window");
    check_le(dial_ms + write_ms * kIdfWriteLegs, kHostWdtMs,
             "and a teardown that pays a dial AND a stalled write still fits in one");

    check(fake_ws::last_write_payload() == frame,
          "the ordinary send still reached the transport, byte for byte");
    check(fake_ws::handle_misuse() == 0, "with no handle misuse anywhere in the case");
}

}  // namespace

int main() {
    std::printf("esp_ws_client_link bounded-blocking host suite (#952, #1058):\n");
    test_teardown_does_not_destroy_handles_under_a_sender();
    test_teardown_does_not_wait_out_the_reconnect_backoff();
    test_a_teardown_that_lands_mid_dial_costs_no_dial_bound();
    test_an_orphaned_dial_that_succeeds_closes_and_releases_once();
    test_an_orphaned_dial_that_fails_releases_once();
    test_teardown_racing_the_dials_resolution_releases_once();
    test_send_during_a_redial_reads_no_handle();
    test_the_blocking_bounds_are_derived_from_the_watchdog();
    // Nothing detached may still be running here: a condemned dial's recv thread outlives
    // its link by design, and one still inside the transport at process exit is a crash
    // under ASan/TSan and a leak under LSan. Every case drains, and this is the backstop.
    check(drained_fake(), "no orphaned dial or transport handle outlived the suite");
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
