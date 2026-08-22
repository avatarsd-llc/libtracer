/**
 * @file
 * @brief #900 + #901 + #1102 — the ESP-IDF WebSocket *client* link's RECEIVE PATH, on
 *        the host.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as the #816 / #835 httpd suites: the REAL chip translation unit
 * (`integrations/esp-idf/libtracer/esp_ws_client_link.cpp`) is compiled here against a
 * host fake of the one IDF facility each contract rests on — `esp_transport_ws`'s frame
 * stream and `esp_pthread`'s stack-sizing seam (fake_esp_transport.hpp). No socket is
 * opened at any layer: the ESP-IDF WebSocket plane must never use POSIX sockets (#947),
 * and a fake that opened one would prove the wrong thing.
 *
 * What is pinned here:
 *
 *   1. #900 — the `recv_stack` knob REACHES the thread. On IDF a `std::thread`'s stack
 *      comes from the global `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` unless
 *      `esp_pthread_set_cfg` armed the next `pthread_create`; the ctor used to
 *      `(void)recv_stack` and spawn a plain `std::thread`, so a caller who sized the
 *      stack for its in-call delivery path got the default and a stack-overflow reboot.
 *      The observable is the arming call, plus its restore (or the size would leak onto
 *      every later thread spawned from the same caller).
 *
 *   2. #901 — drop-don't-truncate ACTUALLY HOLDS. An oversized message is dropped
 *      WHOLE: its remainder is consumed and discarded, never handed up as a bogus
 *      standalone frame. This is not fragmentation-specific — `esp_transport_read` caps
 *      each read at the space offered, so a single unfragmented oversized frame splits
 *      across reads and IDF re-reports that frame's `fin` on each of them, which is what
 *      made the tail look like a complete small message.
 *
 *   3. The two adjacent gaps: a stray CONTINUATION with no message open is dropped (the
 *      server sibling's rule), and an EXACT-FIT message delivers instead of being swept
 *      into the overflow branch.
 *
 *   4. #1102 — `defer_recv` HOLDS THE FIRST DIAL until `start_receiving()` (ADR-0081's
 *      defer-the-dial arm). An unarmed link never connects, so a peer's push-on-connect
 *      cannot beat the sink install; the message is delivered — never lost — once the
 *      owner arms the link, and the latch is ONE-SHOT: a peer CLOSE re-dials and keeps
 *      delivering with no re-arm (pinning the triage refutation that a reconnect
 *      re-opens the window).
 *
 * Each drop case also proves RECOVERY — a well-formed message sent right after parses
 * correctly — and that is what makes the assertions ordering-based rather than
 * sleep-based: a bogus tail would reach the receiver BEFORE the good message, so
 * counting deliveries at the moment the good one lands catches it without a timing
 * guess.
 *
 * What this suite deliberately does NOT claim: that 12288 bytes is enough stack for a
 * given graph's delivery depth. That is a per-application high-water-mark measurement
 * and belongs on the HIL, not on a host fake.
 */

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "fake_esp_transport.hpp"
#include "libtracer_esp/esp_ws_client_link.hpp"

namespace {

using namespace std::chrono_literals;

/** @brief Failed-check counter; main() turns it into the exit status. */
int g_failures = 0;

/** @brief Record one assertion. */
void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL: %s\n", what);
    } else {
        std::printf("  ok:   %s\n", what);
    }
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
 * @brief Assert that the fake holds nothing any more — every case that destroyed a link
 *        ends here (#1456).
 *
 * A teardown that lands mid-dial detaches the link's recv thread, so the orphan releases
 * its transport pair up to a dial bound after the link is gone. Returning from a case (and
 * ultimately from `main`) before that leaves a live thread inside the fake while the fake's
 * static state is being destroyed — the TSan race #1456 reported. A timeout is a real
 * release defect, so it is scored as a failed check rather than skipped.
 */
void check_drained() {
    check(fake_ws::wait_drained(), "the fake drained: no live handle, no parked dialer");
}

/** @brief Collects every message the link delivers, in order, as owned copies. */
class sink_t {
   public:
    /** @brief The receiver callable the link's span tier invokes in-call. */
    void operator()(std::span<const std::byte> frame) {
        const std::lock_guard<std::mutex> lk(m_);
        got_.emplace_back(frame.begin(), frame.end());
    }

    /** @brief How many messages have been delivered so far. */
    [[nodiscard]] std::size_t count() const {
        const std::lock_guard<std::mutex> lk(m_);
        return got_.size();
    }

    /** @brief A copy of delivery @p i (empty when there is none). */
    [[nodiscard]] std::vector<std::byte> at(std::size_t i) const {
        const std::lock_guard<std::mutex> lk(m_);
        return i < got_.size() ? got_[i] : std::vector<std::byte>{};
    }

   private:
    mutable std::mutex m_;
    std::vector<std::vector<std::byte>> got_;
};

/** @brief The rx buffer every receive-path case sizes its frames against. */
constexpr std::size_t kRxBytes = 64;

/** @brief A distinctive well-formed message used as the "did the assembler recover?"
 *         probe after every drop. */
fake_ws::frame_t good_message() {
    return fake_ws::make_frame(WS_TRANSPORT_OPCODES_BINARY, /*fin=*/true, /*len=*/8, /*seed=*/0xA0);
}

/** @brief True iff @p got is byte-for-byte the payload of @p want. */
bool same_bytes(const std::vector<std::byte>& got, const fake_ws::frame_t& want) {
    return got == want.payload;
}

/**
 * @brief #900 — a non-zero `recv_stack` arms the pthread config, and the config is
 *        restored so the size does not leak onto later spawns.
 */
void test_recv_stack_reaches_the_thread() {
    std::printf("#900 recv_stack is honored:\n");
    fake_ws::reset();
    {
        // 12288 is the depth the sibling SERVER link already sizes this workload at
        // (httpd_ws_link_t's recv stack) — the in-call graph delivery is the same one.
        tr::net::esp_ws_client_link_t link("127.0.0.1", 8080, "/ws", /*handshake_headers=*/{},
                                           kRxBytes, kRxBytes, 12288);
        check(fake_ws::armed_stack() == 12288, "the requested stack size was armed");
        check(fake_ws::armed_name() == "ws_cli_rx", "the recv thread was named");
        check(fake_ws::cfg_restored(), "the surrounding pthread config was restored");
        check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s), "the link dialed");
    }
    check_drained();
}

/** @brief #900 — `recv_stack == 0` keeps the historical behaviour: nothing is armed. */
void test_zero_recv_stack_arms_nothing() {
    std::printf("#900 recv_stack=0 is the platform default:\n");
    fake_ws::reset();
    {
        tr::net::esp_ws_client_link_t link("127.0.0.1", 8080, "/ws", /*handshake_headers=*/{},
                                           kRxBytes, kRxBytes, 0);
        check(fake_ws::armed_stack() == 0, "no pthread config was armed");
        check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s), "the link dialed");
    }
    check_drained();
}

/**
 * @brief Build a link, wait for it to dial, and install @p sink — the common prologue.
 *
 * This prologue steps AROUND the pre-sink window by construction: the fake's `poll_read`
 * reports "no data" until `push_frames`, so the receiver is always installed before any
 * frame exists to deliver. The window itself — a peer that pushes before the sink is
 * installed — is what `test_defer_recv_holds_the_dial_until_armed` pins (#1102).
 */
std::unique_ptr<tr::net::esp_ws_client_link_t> dialed_link(sink_t& sink) {
    auto link = std::make_unique<tr::net::esp_ws_client_link_t>(
        "127.0.0.1", 8080, "/ws", /*handshake_headers=*/std::string{}, kRxBytes, kRxBytes, 0);
    check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s), "the link dialed");
    link->set_receiver(sink);
    return link;
}

/**
 * @brief #901 — ONE unfragmented oversized frame: the tail must not be delivered.
 *
 * The verifier's case. The frame is 100 bytes against a 64-byte buffer, so
 * `esp_transport_read` serves 64 then 36, both reporting BINARY/fin — the second used to
 * land as a complete 36-byte message.
 */
void test_oversized_single_frame_tail_is_not_delivered() {
    std::printf("#901 oversized UNFRAGMENTED frame is dropped whole:\n");
    fake_ws::reset();
    sink_t sink;
    auto link = dialed_link(sink);
    const fake_ws::frame_t good = good_message();
    fake_ws::push_frames(
        {fake_ws::make_frame(WS_TRANSPORT_OPCODES_BINARY, true, kRxBytes + 36, 1), good});
    check(wait_until([&] { return sink.count() >= 1; }, 5s), "a message was delivered");
    check(sink.count() == 1, "EXACTLY one message — the dropped frame's tail was not delivered");
    check(same_bytes(sink.at(0), good), "and it is the well-formed message that followed");
    check(link->dropped_rx() == 1, "the oversized message was counted on dropped_rx()");
    // Release the link HERE, so the drain below is waited for inside the case rather
    // than raced at process exit (#1456).
    link.reset();
    check_drained();
}

/** @brief #901 — a FRAGMENTED oversized message: same contract, the issue's own case. */
void test_oversized_fragmented_message_tail_is_not_delivered() {
    std::printf("#901 oversized FRAGMENTED message is dropped whole:\n");
    fake_ws::reset();
    sink_t sink;
    auto link = dialed_link(sink);
    const fake_ws::frame_t good = good_message();
    fake_ws::push_frames({
        fake_ws::make_frame(WS_TRANSPORT_OPCODES_BINARY, /*fin=*/false, 40, 1),
        fake_ws::make_frame(WS_TRANSPORT_OPCODES_CONT, /*fin=*/false, 40, 2),
        fake_ws::make_frame(WS_TRANSPORT_OPCODES_CONT, /*fin=*/true, 12, 3),
        good,
    });
    check(wait_until([&] { return sink.count() >= 1; }, 5s), "a message was delivered");
    check(sink.count() == 1, "EXACTLY one message — the remaining fragments were discarded");
    check(same_bytes(sink.at(0), good), "and it is the well-formed message that followed");
    check(link->dropped_rx() == 1, "the oversized message was counted on dropped_rx()");
    // Release the link HERE, so the drain below is waited for inside the case rather
    // than raced at process exit (#1456).
    link.reset();
    check_drained();
}

/** @brief #901 — an EXACT-FIT message delivers; it used to take the overflow branch. */
void test_exact_fit_message_delivers() {
    std::printf("#901 an exact-fit message delivers:\n");
    fake_ws::reset();
    sink_t sink;
    auto link = dialed_link(sink);
    const fake_ws::frame_t exact =
        fake_ws::make_frame(WS_TRANSPORT_OPCODES_BINARY, true, kRxBytes, 0x40);
    fake_ws::push_frames({exact});
    check(wait_until([&] { return sink.count() >= 1; }, 5s), "the exact-fit message delivered");
    check(sink.count() == 1, "exactly once");
    check(same_bytes(sink.at(0), exact), "with all of its bytes");
    check(link->dropped_rx() == 0, "and nothing was counted as dropped");
    // Release the link HERE, so the drain below is waited for inside the case rather
    // than raced at process exit (#1456).
    link.reset();
    check_drained();
}

/** @brief #901 — a stray CONTINUATION with no message open is dropped, not started. */
void test_stray_continuation_is_dropped() {
    std::printf("#901 a stray CONT with no message open is dropped:\n");
    fake_ws::reset();
    sink_t sink;
    auto link = dialed_link(sink);
    const fake_ws::frame_t good = good_message();
    fake_ws::push_frames({fake_ws::make_frame(WS_TRANSPORT_OPCODES_CONT, true, 16, 0x70), good});
    check(wait_until([&] { return sink.count() >= 1; }, 5s), "a message was delivered");
    check(sink.count() == 1, "EXACTLY one — the stray CONT was not delivered");
    check(same_bytes(sink.at(0), good), "and it is the well-formed message that followed");
    check(link->dropped_rx() == 1, "the stray CONT was counted on dropped_rx()");
    // Release the link HERE, so the drain below is waited for inside the case rather
    // than raced at process exit (#1456).
    link.reset();
    check_drained();
}

/** @brief A message that fits and is FRAGMENTED still reassembles — the case the discard
 *         state must not break. */
void test_fragmented_fitting_message_reassembles() {
    std::printf("#901 a fitting fragmented message still reassembles:\n");
    fake_ws::reset();
    sink_t sink;
    auto link = dialed_link(sink);
    const fake_ws::frame_t a = fake_ws::make_frame(WS_TRANSPORT_OPCODES_BINARY, false, 20, 0x10);
    const fake_ws::frame_t b = fake_ws::make_frame(WS_TRANSPORT_OPCODES_CONT, true, 12, 0x30);
    fake_ws::push_frames({a, b});
    check(wait_until([&] { return sink.count() >= 1; }, 5s), "the message delivered");
    check(sink.count() == 1, "exactly once");
    std::vector<std::byte> want = a.payload;
    want.insert(want.end(), b.payload.begin(), b.payload.end());
    check(sink.at(0) == want, "with both fragments, in order");
    check(link->dropped_rx() == 0, "and nothing was counted as dropped");
    // Release the link HERE, so the drain below is waited for inside the case rather
    // than raced at process exit (#1456).
    link.reset();
    check_drained();
}

/**
 * @brief #1102 — `defer_recv` holds the FIRST dial until `start_receiving()`, and the
 *        latch is one-shot across reconnects (ADR-0081's defer-the-dial arm).
 *
 * The scripted window is the documented recipe's: the peer's push-on-connect (a frame
 * queued before the link is armed) against a sink installed after construction. With the
 * latch, the unarmed link never dials, so the message WAITS at the peer and arrives
 * intact once the owner arms the link — never lost, never counted. Without the
 * production hunk the ctor dials at once and the pushed frame is decoded into the empty
 * slot: the "did not dial" check and the "arrived after arming" check both redden, which
 * is the non-vacuity this guard was specified with. The reconnect leg pins the triage
 * refutation of the original title: a peer CLOSE re-dials (no re-arm — the second dial
 * is the latch NOT re-closing) and the next message is delivered.
 */
void test_defer_recv_holds_the_dial_until_armed() {
    std::printf("#1102 defer_recv holds the dial until start_receiving():\n");
    fake_ws::reset();
    sink_t sink;
    // The link lives in a scope of its own so it is DESTROYED before the drain below,
    // rather than at process exit under the fake's dying static state (#1456).
    {
        tr::net::esp_ws_client_link_t link("127.0.0.1", 8080, "/ws", /*handshake_headers=*/{},
                                           kRxBytes, kRxBytes, /*recv_stack=*/0,
                                           /*defer_recv=*/true);
        // The peer's push-on-connect, queued while the link is UNARMED and the sink absent.
        const fake_ws::frame_t good = good_message();
        fake_ws::push_frames({good});
        check(!wait_until([] { return fake_ws::connect_count() >= 1; }, 250ms),
              "an unarmed link did NOT dial");
        check(sink.count() == 0, "and nothing reached the sink before it was installed");
        link.set_receiver(sink);
        link.start_receiving();
        link.start_receiving();  // idempotent — make_connection arms every link unconditionally
        check(wait_until([] { return fake_ws::connect_count() >= 1; }, 2s),
              "arming released the first dial");
        check(wait_until([&] { return sink.count() >= 1; }, 5s),
              "the pre-arm message ARRIVED after arming — held, never lost");
        check(same_bytes(sink.at(0), good), "byte-for-byte");
        check(link.dropped_rx() == 0, "and nothing was counted as dropped");

        // Reconnect leg: the latch is ONE-SHOT. A peer CLOSE tears the connection down; the
        // re-dial happens with no re-arm, and a message on the NEW connection is delivered —
        // a future latch that wrongly re-closed per connection reddens here.
        fake_ws::push_frames({fake_ws::make_frame(WS_TRANSPORT_OPCODES_CLOSE, /*fin=*/true,
                                                  /*len=*/2, /*seed=*/0x11)});
        check(wait_until([] { return fake_ws::connect_count() >= 2; }, 5s),
              "the link re-dialed after the peer CLOSE with no re-arm");
        const fake_ws::frame_t after = fake_ws::make_frame(WS_TRANSPORT_OPCODES_BINARY,
                                                           /*fin=*/true, /*len=*/8, /*seed=*/0xB0);
        fake_ws::push_frames({after});
        check(wait_until([&] { return sink.count() >= 2; }, 5s),
              "a message on the new connection was delivered — the latch did not re-close");
        check(same_bytes(sink.at(1), after), "byte-for-byte");
        check(link.dropped_rx() == 0, "and still nothing was counted as dropped");
    }
    check_drained();
}

}  // namespace

/**
 * @brief #1503 step 4 / #932 — the #932 drop seam answers through a generic `transport_t&`.
 *
 * The asymmetry this closes (finding 5 of #1503): this link shipped the RICH half of the
 * #932 contract (`stats()`) without the GENERIC half, so a consumer holding only a
 * `transport_t&` — which is all `fwd_router_t` and any kind-agnostic host hold — saw ZERO
 * drops from a link that was counting them, and saw them again the moment it swapped in
 * `httpd_ws_link_t`. Read through the BASE reference on purpose: an override reachable only
 * from the concrete type would pass a `link->drop_stats()` assertion and still leave the
 * seam broken for its only real caller.
 *
 * A PROJECTION, never a second tally, so the case asserts agreement with the counter that
 * owns the truth rather than a number of its own.
 */
void test_drop_stats_projects_through_the_base_interface() {
    std::printf("#932/#1503 drop_stats() answers through a generic transport_t&:\n");
    fake_ws::reset();
    sink_t sink;
    auto link = dialed_link(sink);
    const tr::net::transport_t& generic = *link;

    // The positive control comes FIRST: a link that has dropped nothing must report
    // nothing. A projection wired to the wrong field would fail here before it ever
    // reached the armed arm below.
    const tr::net::transport_drop_stats_t clean = generic.drop_stats();
    check(clean.dropped_rx == 0 && clean.malformed_rx == 0 && clean.dropped_tx == 0,
          "a link that dropped nothing reports nothing through the base seam");

    const fake_ws::frame_t good = good_message();
    fake_ws::push_frames(
        {fake_ws::make_frame(WS_TRANSPORT_OPCODES_BINARY, true, kRxBytes + 36, 1), good});
    check(wait_until([&] { return sink.count() >= 1; }, 5s), "a message was delivered");

    const tr::net::transport_drop_stats_t d = generic.drop_stats();
    check(d.dropped_rx == 1, "the oversize drop is visible through the BASE interface");
    check(d.dropped_rx == link->dropped_rx(),
          "and it is the SAME number dropped_rx() reports — a projection, not a second tally");
    check(d.malformed_rx == 0,
          "malformed_rx stays 0 — this link never classifies a message as protocol-malformed, "
          "and the #932 default exists so it need not fabricate one");
    check(d.dropped_tx == link->stats().c.tx_drops,
          "dropped_tx reads straight off the counter that owns it");

    // The ceiling beside the drop (#1160), in the vocabulary's own spelling.
    check(link->rx_capacity() == link->rx_bytes() && link->rx_capacity() == kRxBytes,
          "rx_capacity() names the ceiling that produced the drop");
    check(link->tx_capacity() == link->tx_bytes(), "and tx_capacity() its outbound twin");

    link.reset();
    check_drained();
}

int main() {
    std::printf("esp_ws_client_link receive-path host suite (#900, #901, #1102):\n");
    test_recv_stack_reaches_the_thread();
    test_zero_recv_stack_arms_nothing();
    test_oversized_single_frame_tail_is_not_delivered();
    test_oversized_fragmented_message_tail_is_not_delivered();
    test_exact_fit_message_delivers();
    test_stray_continuation_is_dropped();
    test_fragmented_fitting_message_reassembles();
    test_defer_recv_holds_the_dial_until_armed();
    test_drop_stats_projects_through_the_base_interface();
    // Defensive: `main` must never return under a detached orphan still inside the fake,
    // whose static state is destroyed on the way out (#1456).
    check_drained();
    if (g_failures != 0) {
        std::printf("FAILED: %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
