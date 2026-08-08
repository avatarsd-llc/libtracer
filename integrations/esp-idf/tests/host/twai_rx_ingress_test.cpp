/**
 * @file
 * @brief #1029 — the TWAI link's INBOUND path on the host: the seam's ingress
 *        rule, the DLC decode and its classic clamp, and the ISR→dispatch handoff.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Same construction as `twai_backpressure_test.cpp` next door: the REAL chip
 * translation unit (`integrations/esp-idf/libtracer/twai_link.cpp`) compiled
 * against the host fake of `esp_driver_twai`'s node API and the FreeRTOS
 * primitives under it (fake_twai.hpp). That suite scripts the TRANSMIT direction;
 * this one scripts the RECEIVE direction, which the fake could not express at all
 * until #1029 gave it `script_rx` / `deliver_one_rx` — the mirror of the tx side's
 * `complete_one_tx`. Before that, `twai_link_t::on_rx_done_isr` ran only on
 * silicon.
 *
 * The decisions pinned here are the ones that body makes. The first two are SHARED with
 * the SocketCAN sibling rather than local to this port; the third is local, and is listed
 * because it is the handoff the ISR context forces:
 *
 *   - `tr::net::can_rx_admissible` — 29-bit data frames only. This is the rule the
 *     two platform links have already diverged on once: `twai_link_t` filtered RTR
 *     while `socketcan_link_t` did not, and a remote-request frame reached the
 *     reassembler as a data slice whose DLC promised bytes it never carried (#931,
 *     docs/reference/14-can-transport.md);
 *   - `twaifd_dlc2len(header.dlc)` capped at `tr::net::can_max_len(false)`. The cap
 *     is load-bearing, not defensive tidying. Checked in ESP-IDF's `esp32/` and
 *     `esp32c3/` `hal/twai_ll.h` (not every part — those two): the v1
 *     `twai_ll_parse_frame_header` sets `header->dlc = rx_frame->dlc`, the raw
 *     4-bit field, and clamps only the separate DATA copy — so a bus frame coding
 *     9..15 hands the link a code that expands to 12..64 bytes, and the link's own
 *     cap is what keeps `len` inside the 8 bytes the ISR actually copied;
 *   - the ISR→dispatch handoff: a FULL `rx_queue_` DROPS rather than blocking in
 *     ISR context. LOCAL to this port, not shared: `socketcan_link_t` has neither an ISR
 *     nor a queue, so there is no sibling decision to diverge from here.
 *
 * What is NOT claimed here: nothing in this file exercises a real controller,
 * arbitration, bus-off/error state, or CAN-FD (this controller has none, and the
 * error leg of the admissibility rule is false by construction on this path). The
 * dispatch thread's copy of the registered callback under `m_` is also not pinned
 * by any case below — see the PR for why.
 *
 * The negative cases use a BARRIER rather than a sleep: each scripts the frame
 * that must be refused, then an admissible sentinel behind it. Both traverse the
 * same ISR path in order, so if the refused frame had been admitted it would sit
 * AHEAD of the sentinel in `rx_queue_` and be delivered first. Seeing the sentinel
 * arrive, alone and first, is therefore proof the other one was dropped — not
 * merely proof that it had not arrived yet.
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

#include "fake_twai.hpp"
#include "libtracer_esp/twai_link.hpp"

namespace {

using tr::net::can_frame_data_t;
using tr::net::twai_link_config_t;
using tr::net::twai_link_t;

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

/**
 * @brief Everything the link handed to the registered receive callback, in order.
 *
 * Written from the link's dispatch thread and read from the test thread, so it
 * carries its own lock; `snapshot` hands back a copy rather than a reference into
 * a container the dispatch thread may still be appending to.
 */
class recorder_t {
   public:
    /** @brief Record one delivered frame (runs on the link's dispatch thread). */
    void record(const can_frame_data_t& frame) {
        const std::lock_guard lock(m_);
        frames_.push_back(frame);
    }

    /** @brief How many frames have reached the callback so far. */
    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(m_);
        return frames_.size();
    }

    /** @brief A copy of what has been delivered so far, in delivery order. */
    [[nodiscard]] std::vector<can_frame_data_t> snapshot() const {
        const std::lock_guard lock(m_);
        return frames_;
    }

   private:
    mutable std::mutex m_;                 /**< @brief Guards @ref frames_. */
    std::vector<can_frame_data_t> frames_; /**< @brief Delivered frames, in order. */
};

/** @brief Poll until @p rec holds @p want frames, or @p budget_ms elapses. */
bool wait_for_count(const recorder_t& rec, std::size_t want, long long budget_ms) {
    const auto start = std::chrono::steady_clock::now();
    while (ms_since(start) < budget_ms) {
        if (rec.count() >= want) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return rec.count() >= want;
}

/**
 * @brief A link config whose ISR→dispatch queue is @p rx_depth frames deep.
 *
 * The TX side is left at its defaults and never exercised: nothing here writes.
 */
twai_link_config_t config_with_rx_depth(std::uint32_t rx_depth) {
    twai_link_config_t config;
    config.tx_gpio = 4;
    config.rx_gpio = 5;
    config.tx_queue_depth = 1;
    config.rx_queue_depth = rx_depth;
    return config;
}

/** @brief The identifier the sentinel frame every negative case queues behind. */
constexpr std::uint32_t kSentinelId = 0x0155AA33u;

/** @brief An admissible extended data frame, used as the negative cases' barrier. */
fake_twai::scripted_rx_frame_t sentinel() {
    fake_twai::scripted_rx_frame_t f;
    f.id = kSentinelId;
    f.extended = true;
    f.remote = false;
    f.dlc = 2;
    f.data = {0x5Au, 0xA5u};
    return f;
}

/** @brief Whether @p frame's live bytes equal @p want, byte for byte. */
bool bytes_equal(const can_frame_data_t& frame, const std::vector<std::uint8_t>& want) {
    if (frame.len != want.size()) return false;
    for (std::size_t i = 0; i < want.size(); ++i) {
        if (frame.data[i] != static_cast<std::byte>(want[i])) return false;
    }
    return true;
}

/**
 * @brief The shape both refusal cases share: X, then the sentinel, then the verdict.
 *
 * @param refused The frame the ingress rule must drop.
 * @param label   What to call it in the printed verdicts.
 */
void run_refusal_case(const fake_twai::scripted_rx_frame_t& refused, std::string_view label) {
    fake_twai::reset();

    // Declared BEFORE the link: the callback captures it by reference and the
    // link's destructor joins the dispatch thread, so the recorder has to outlive
    // the link rather than the other way round.
    recorder_t rec;
    auto link = std::make_unique<twai_link_t>(config_with_rx_depth(8));
    check(link->ok(), "the link came up on the fake controller");
    if (!link->ok()) return;
    link->on_receive([&rec](const can_frame_data_t& frame) { rec.record(frame); });

    fake_twai::script_rx(refused);
    fake_twai::script_rx(sentinel());
    check_eq(fake_twai::rx_scripted(), 2, "both frames are queued at the controller");
    check(fake_twai::deliver_one_rx(), "the driver reported the first frame");
    check(fake_twai::deliver_one_rx(), "the driver reported the sentinel behind it");

    check(wait_for_count(rec, 1, 2000), "the sentinel reached the receive callback");
    const auto got = rec.snapshot();
    check_eq(got.size(), 1, "exactly one frame reached it");
    const bool only_sentinel = got.size() == 1 && got[0].id == kSentinelId;
    std::printf("       refused frame: %.*s\n", static_cast<int>(label.size()), label.data());
    check(only_sentinel,
          "and it is the sentinel — the refused frame would have been delivered FIRST");
}

/**
 * @brief A standard 11-bit identifier is refused: it carries no decodable path.
 *
 * The binding is header-elided — the 29-bit extended identifier IS the path
 * (ADR-0022) — so an 11-bit frame has nothing the graph could address.
 */
void test_a_standard_identifier_frame_is_refused() {
    std::printf("a standard-identifier frame is refused:\n");
    fake_twai::scripted_rx_frame_t f;
    f.id = 0x123u;
    f.extended = false;
    f.remote = false;
    f.dlc = 4;
    f.data = {0x11u, 0x22u, 0x33u, 0x44u};
    run_refusal_case(f, "an 11-bit standard-ID data frame");
}

/**
 * @brief A remote-transmission request is refused — the divergence that shipped.
 *
 * An RTR carries a DLC but NO data field. Admitting one hands the reassembler a
 * slice whose declared length is a lie, which is exactly what happened on the
 * SocketCAN side before the rule moved to the seam (#931).
 */
void test_a_remote_request_frame_is_refused() {
    std::printf("a remote-transmission-request frame is refused:\n");
    fake_twai::scripted_rx_frame_t f;
    f.id = 0x0100ABCDu;
    f.extended = true;
    f.remote = true;
    f.dlc = 4;
    // Deliberately empty: an RTR promises four bytes and carries none, which is
    // the whole reason it must not reach the reassembler as a data slice.
    run_refusal_case(f, "a 29-bit RTR frame promising four bytes it does not carry");
}

/**
 * @brief The control: an ordinary extended data frame IS delivered, intact.
 *
 * Without this, both refusals above would pass just as well against a link that
 * delivered nothing at all.
 */
void test_an_extended_data_frame_is_delivered() {
    std::printf("an ordinary extended data frame is delivered intact:\n");
    fake_twai::reset();

    recorder_t rec;
    auto link = std::make_unique<twai_link_t>(config_with_rx_depth(8));
    check(link->ok(), "the link came up on the fake controller");
    if (!link->ok()) return;
    link->on_receive([&rec](const can_frame_data_t& frame) { rec.record(frame); });

    const std::vector<std::uint8_t> payload = {0xDEu, 0xADu, 0xBEu, 0xEFu, 0x42u};
    fake_twai::scripted_rx_frame_t f;
    // Bits above 28 are set on purpose: the link masks the identifier to 29 bits
    // on the way out, and a controller-supplied id could never show that.
    f.id = 0xE1234567u;
    f.extended = true;
    f.remote = false;
    f.dlc = 5;
    f.data = payload;
    fake_twai::script_rx(f);
    check(fake_twai::deliver_one_rx(), "the driver reported the frame");

    check(wait_for_count(rec, 1, 2000), "it reached the receive callback");
    const auto got = rec.snapshot();
    check_eq(got.size(), 1, "exactly one frame reached it");
    if (got.empty()) return;
    check_eq(got[0].id, 0x01234567u, "the identifier is masked to 29 bits");
    check(!got[0].fd, "it is a classic frame, never FD");
    check_eq(got[0].len, payload.size(), "the length is the decoded DLC");
    check(bytes_equal(got[0], payload), "and the data field is byte-exact");
}

/**
 * @brief A DLC coding more than the classic width is TRUNCATED, not trusted.
 *
 * DLC 12 is the FD coding for 24 bytes. Classic CAN carries at most 8, so the
 * link's `can_max_len(false)` cap must win over the decoded number — otherwise
 * `out.len` would declare 24 live bytes in a carrier the ISR filled with 8.
 */
void test_a_dlc_above_the_classic_width_is_truncated() {
    std::printf("a DLC above the classic width is truncated to it:\n");
    fake_twai::reset();

    recorder_t rec;
    auto link = std::make_unique<twai_link_t>(config_with_rx_depth(8));
    check(link->ok(), "the link came up on the fake controller");
    if (!link->ok()) return;
    link->on_receive([&rec](const can_frame_data_t& frame) { rec.record(frame); });

    // Twenty-four scripted bytes, so the truncation observed below is the link's
    // clamp and not an artifact of a script that ran out of data.
    std::vector<std::uint8_t> payload;
    payload.reserve(24);
    for (std::uint8_t i = 0; i < 24; ++i) payload.push_back(static_cast<std::uint8_t>(0xA0u + i));

    fake_twai::scripted_rx_frame_t f;
    f.id = 0x0200BEEFu;
    f.extended = true;
    f.remote = false;
    f.dlc = 12;  // twaifd_dlc2len(12) == 24
    f.data = payload;
    fake_twai::script_rx(f);
    check(fake_twai::deliver_one_rx(), "the driver reported the over-wide frame");

    check(wait_for_count(rec, 1, 2000), "it reached the receive callback");
    const auto got = rec.snapshot();
    check_eq(got.size(), 1, "exactly one frame reached it");
    if (got.empty()) return;
    check_eq(got[0].len, 8, "the length is the classic width, not the DLC's 24");
    check_eq(got[0].bytes().size(), 8, "and the live span is eight bytes");
    const std::vector<std::uint8_t> first_eight(payload.begin(), payload.begin() + 8);
    check(bytes_equal(got[0], first_eight), "carrying the first eight bytes, byte-exact");
}

/**
 * @brief A full ISR→dispatch queue DROPS the excess and never blocks the ISR.
 *
 * Staged deterministically rather than by racing the dispatch thread: the receive
 * callback parks on its FIRST invocation, so the dispatch thread is provably
 * inside the callback — and provably not draining — while the burst behind it is
 * reported. The queue then accepts exactly `rx_queue_depth` frames and refuses the
 * rest.
 *
 * The assertion is on the number of frames that reach the callback, not on a
 * counter: `twai_link_t` has no receive-side drop counter and the ISR discards the
 * queue-send result (#1029 explicitly does not add one).
 */
void test_a_full_rx_queue_drops_rather_than_blocking() {
    std::printf("a full ISR->dispatch queue drops the excess without blocking:\n");
    fake_twai::reset();

    constexpr std::uint32_t kRxDepth = 4;
    constexpr int kExcess = 3;

    // All four of these outlive the link below on purpose: the callback captures
    // them, and the link's destructor joins the thread running it.
    recorder_t rec;
    std::mutex gate_m;
    std::condition_variable gate_cv;
    bool gate_open = false;
    int seen_in_callback = 0;  // dispatch thread only

    auto link = std::make_unique<twai_link_t>(config_with_rx_depth(kRxDepth));
    check(link->ok(), "the link came up on the fake controller");
    if (!link->ok()) return;
    link->on_receive([&](const can_frame_data_t& frame) {
        rec.record(frame);
        if (++seen_in_callback == 1) {
            std::unique_lock lock(gate_m);
            gate_cv.wait(lock, [&gate_open] { return gate_open; });
        }
    });

    // Frame 0 parks the dispatch thread inside the callback. Waiting for it to be
    // RECORDED is what makes the rest deterministic: the queue is provably empty
    // and provably undrained from here until the gate opens.
    fake_twai::scripted_rx_frame_t gate_frame = sentinel();
    gate_frame.id = 0x03000000u;
    fake_twai::script_rx(gate_frame);
    check(fake_twai::deliver_one_rx(), "the driver reported the parking frame");
    check(wait_for_count(rec, 1, 2000), "the dispatch thread is parked inside the callback");

    // Now more frames than the queue can hold, with nothing draining it.
    const int burst = static_cast<int>(kRxDepth) + kExcess;
    for (int i = 0; i < burst; ++i) {
        fake_twai::scripted_rx_frame_t f = sentinel();
        f.id = 0x03000001u + static_cast<std::uint32_t>(i);
        fake_twai::script_rx(f);
    }
    int reported = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < burst; ++i) {
        if (fake_twai::deliver_one_rx()) ++reported;
    }
    const long long elapsed = ms_since(start);
    check_eq(static_cast<std::size_t>(reported), static_cast<std::size_t>(burst),
             "the driver reported every frame of the burst");
    // Blocking in ISR context is the failure this rules out; the FreeRTOS queue's
    // refusal is immediate, so the whole burst costs no measurable wait.
    check_under_ms(elapsed, 100, "and the ISR path never blocked on the full queue");
    check_eq(rec.count(), 1, "none of the burst was delivered while the thread was parked");

    {
        const std::lock_guard lock(gate_m);
        gate_open = true;
    }
    gate_cv.notify_all();

    const std::size_t want = 1 + kRxDepth;
    check(wait_for_count(rec, want, 2000), "the queued frames drained once the callback returned");
    // A stragglers' window: if anything beyond the queue's depth had survived, it
    // would land here rather than being missed by an early read.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    check_eq(rec.count(), want, "and exactly queue-depth frames survived, the excess dropped");

    const auto got = rec.snapshot();
    bool head_of_burst = got.size() == want;
    for (std::size_t i = 1; i < got.size() && head_of_burst; ++i) {
        head_of_burst = got[i].id == 0x03000000u + static_cast<std::uint32_t>(i);
    }
    // Which end is discarded is FreeRTOS's (a full xQueueSendFromISR refuses the
    // NEW item), not the link's; recorded because it is what "dropped" means here.
    check(head_of_burst, "the survivors are the frames that arrived first");
}

}  // namespace

int main() {
    std::printf("twai_link RX ingress host suite (#1029):\n");
    test_a_standard_identifier_frame_is_refused();
    test_a_remote_request_frame_is_refused();
    test_an_extended_data_frame_is_delivered();
    test_a_dlc_above_the_classic_width_is_truncated();
    test_a_full_rx_queue_drops_rather_than_blocking();
    std::printf(g_failures == 0 ? "\nALL PASS\n" : "\n%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
