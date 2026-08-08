/**
 * @file
 * @brief Shared framing and the RECEIVER-side counter for the two-process composition-throughput
 *        comparison (libtracer one-datagram-per-K-values vs an engine with no composite send).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section compose_claim The claim under test
 *
 * libtracer batches by **composition**, not by a timer: a composite endpoint's value is a
 * K-link **rope** that is already in memory, and the transport lowers that rope to its native
 * scatter-gather form — one `sendmsg(iovec)` carrying K values. An engine with no composite
 * send has to issue K separate messages for the same K values. So the quantity that separates
 * them is **values delivered per message**, and the prediction is that libtracer's per-value
 * cost stays flat as K grows while a per-value engine's does not.
 *
 * @section compose_measured What this harness measures, and what it does not
 *
 * MEASURES: the number of values a SEPARATE SUBSCRIBER PROCESS observed arriving over a real
 * loopback UDP socket, per second, at composition width K; the number of messages those values
 * arrived in; and the one-way latency of a whole K-value group (stamped in the group's first
 * record, taken when its last record lands). Every published figure is the receiver's own
 * count divided by the receiver's own clock. The publisher's send rate is printed too, but
 * only so the run can report LOSS — it is never the throughput number.
 *
 * DOES NOT MEASURE: any in-process graph cost. There is no `graph_t`, no vertex and no
 * subscription on either side; this is the transport seam and the kernel path, which is where
 * the one-datagram-per-K property lives. It also does not measure what a composite costs to
 * BUILD — the K records and the iovec over them are constructed once, before the timed loop,
 * because the value is supposed to be a rope that already exists. (The withdrawn `bench_scatter`
 * rebuilt its iovec inside the timed loop and understated libtracer by 33-58%.) And it does not
 * measure a receiver's cost to fan K values out to K consumers: the subscriber walks the
 * datagram's records and counts them.
 *
 * @section compose_alloc What each timed loop still allocates
 *
 * The harness does NOT claim an allocation-free timed loop on either arm. What it claims is
 * that no allocation in either timed loop is one the HARNESS added; each is the engine's own
 * cost of shipping the value. Stated per arm, because an earlier revision of this comment
 * asserted "neither timed loop allocates", which was false on both:
 *
 *  - **libtracer.** `udp_transport_t::send(iov)` gathers into a stack `::iovec` array while the
 *    entry count fits `tr::net::kMaxInlineIov` (16, `%iov_table.hpp`), and takes ONE nothrow
 *    heap block per datagram above it. COUNTED, with a replaced global `operator new` around
 *    1 000 sends per width: K=1, 8 and 16 cost **0 per send**, K=17, 64 and 256 cost **exactly
 *    1 per send** (a single warm-up allocation lands in whichever width is measured FIRST —
 *    reordering the sweep moves it, so it tracks order, not width). Of `run_compose.sh`'s four
 *    default widths that means `1` and `8` take the inline path and `64` and `256` take the
 *    overflow. The spill is inside the transport, it is the same code the shipping forward path
 *    runs, and `bench_transport_iov` is the in-tree instrument that located the boundary (0
 *    allocations up to 16 spans, 1 of ~272 B at 17) — so it is engine cost, and it is
 *    deliberately NOT hidden from the comparison.
 *  - **The per-value arm.** Handing a payload to the engine must not COPY it, because the
 *    composite arm's `std::span` iovec does not: a staging copy per put would be harness
 *    overhead charged to one side only. `bench_zenoh_compose.cpp` therefore aliases its staging
 *    buffer into the payload rather than copying it, using the vendored API's documented
 *    non-copying entry point — see the comment on `alias_bytes` there for the exact call, what
 *    is documented versus measured, and the lifetime argument. Whatever the engine then
 *    allocates internally is its own cost, and this harness neither counts nor claims anything
 *    about it.
 *
 * @section compose_record The record framing — identical on both engines
 *
 * One **value** is one length-prefixed record. A composite datagram is K records back to back,
 * gathered into one `sendmsg`; a per-value engine puts each record as its own message. The
 * receiver runs the SAME walk over whatever bytes it is handed, so "a value" means the same
 * thing on both sides and a lost datagram costs its whole group.
 *
 *     offset  size  field
 *          0     4  len       total record bytes, little-endian, including this prefix
 *          4     2  magic     @ref kMagic — this harness's framing, not a run identity
 *          6     2  width     K, the composition width this record belongs to
 *          8     2  index     0..K-1, position within the group
 *         10     1  phase     @ref phase_t
 *         11     1  reserved  0
 *         12     8  send_ts   CLOCK_MONOTONIC ns, or 0 when unstamped
 *         20     4  run_id    the DRIVER's per-point nonce — see below
 *         24        filler    to `len`
 *
 * The receiver checks `magic`, `width` and `run_id` against the point it was told to measure,
 * and counts any record failing one of them as MALFORMED — which makes the point unreportable
 * (@ref sub_counter_t::finish) rather than inflating its rate.
 *
 * `run_id` is why the check is worth anything on a shared host. `magic` is a compile-time
 * constant and `width` is the swept parameter, so with those two alone a SECOND concurrent run
 * of this same harness at the same K, on the deterministic port sequence `run_compose.sh`
 * walks, would have passed both checks and been folded into the rate. `run_id` is drawn once
 * per point by the driver and handed to BOTH processes on argv, so two runs are separated by a
 * 32-bit nonce collision instead of by luck. Neither process has a default for it: there is no
 * value of `run_id` that means "unchecked".
 *
 * CLOCK_MONOTONIC is system-wide on Linux, so `recv_ts - send_ts` across the two processes on
 * one host is a valid one-way latency.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "bench_common.hpp"

namespace bench::compose {

/** @brief What a record is doing in the run. */
enum class phase_t : std::uint8_t {
    LATENCY = 0,      /**< Paced group; the receiver times it. */
    THROUGHPUT = 1,   /**< Unpaced group; the receiver counts it. */
    END_OF_POINT = 2, /**< The publisher is done with this K; finalize and report. */
};

/** @brief Marks a datagram as belonging to THIS harness — framing only, not a run identity. */
inline constexpr std::uint16_t kMagic = 0xC0DE;

/** @brief Field offsets and the fixed header width of one value record. */
inline constexpr std::size_t kOffLen = 0, kOffMagic = 4, kOffWidth = 6, kOffIndex = 8,
                             kOffPhase = 10, kOffTs = 12, kOffRun = 20, kRecordHeader = 24;

// The swept K set, the bytes per value and the per-point counts are NOT defined here. Both
// binaries take every one of them on argv, and run_compose.sh is their single source: a default
// living in two places is a default that drifts, and the one place that has to be right is the
// script that drives both engines with the SAME numbers.

/** @brief Nanoseconds between paced groups, so a latency probe never queues behind itself. */
inline constexpr std::uint64_t kPaceNs = 150000;

/** @brief Pause after the paced phase so its backlog drains before the blast starts. */
inline constexpr std::uint64_t kDrainMs = 400;

/** @brief Store @p v little-endian at @p off. */
inline void put_le(std::span<std::byte> b, std::size_t off, std::uint64_t v, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) b[off + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}

/** @brief Read @p n little-endian bytes at @p off. */
[[nodiscard]] inline std::uint64_t get_le(std::span<const std::byte> b, std::size_t off,
                                          std::size_t n) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i)
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(b[off + i])) << (8 * i);
    return v;
}

/**
 * @brief Build the K records of one group, once.
 *
 * Called BEFORE the timed loop on purpose — see @ref compose_measured. The only per-send
 * mutation the timed loops perform is @ref stamp on record 0, and only in the paced phase.
 *
 * @param run_id The driver's per-point nonce, stamped into every record (@ref compose_record).
 */
[[nodiscard]] inline std::vector<std::vector<std::byte>> make_group(std::size_t k,
                                                                    std::size_t value_bytes,
                                                                    phase_t phase,
                                                                    std::uint32_t run_id) {
    std::vector<std::vector<std::byte>> recs(k,
                                             std::vector<std::byte>(value_bytes, std::byte{0xAB}));
    for (std::size_t i = 0; i < k; ++i) {
        const std::span<std::byte> r(recs[i]);
        put_le(r, kOffLen, value_bytes, 4);
        put_le(r, kOffMagic, kMagic, 2);
        put_le(r, kOffWidth, k, 2);
        put_le(r, kOffIndex, i, 2);
        r[kOffPhase] = static_cast<std::byte>(phase);
        r[kOffPhase + 1] = std::byte{0};
        put_le(r, kOffTs, 0, 8);
        put_le(r, kOffRun, run_id, 4);
    }
    return recs;
}

/**
 * @brief Reject a value size that cannot hold the record header.
 *
 * The framing is fixed-width and the value size is an operator knob (`COMPOSE_VALUE_BYTES`), so
 * the two can be set against each other. A record shorter than @ref kRecordHeader would make
 * every datagram malformed at the receiver and every point unreportable, for a reason no output
 * line would name; both roles of both arms call this on entry instead.
 *
 * @return false after printing the reason, so the caller can exit non-zero.
 */
[[nodiscard]] inline bool value_bytes_ok(std::size_t value_bytes) {
    if (value_bytes >= kRecordHeader) return true;
    std::fprintf(stderr, "compose: value_bytes=%zu is below the %zu-byte record header\n",
                 value_bytes, kRecordHeader);
    return false;
}

/** @brief Write the group's send timestamp into record 0 — one 8-byte store per group. */
inline void stamp(std::vector<std::byte>& rec0, std::uint64_t ts) {
    put_le(std::span<std::byte>(rec0), kOffTs, ts, 8);
}

/**
 * @brief The receiver's counter — the ONLY source of a published rate in this harness.
 *
 * Every field here is observed on the subscriber side. Nothing is derived from the publisher,
 * and there is deliberately no way to hand this class a send count.
 */
class sub_counter_t {
   public:
    /**
     * @param system Engine name for the RESULT row.
     * @param mode Row mode (e.g. `compose-udp`).
     * @param width The K this point is measuring; a record claiming any other width is
     *              malformed.
     * @param value_bytes Bytes per value, for the MB/s column.
     * @param run_id The driver's per-point nonce; a record carrying any other one is malformed.
     *               Together with @p width this is what rejects a datagram from a CONCURRENT run
     *               of this harness on the same port (@ref compose_record).
     * @param min_thru_messages Fewest throughput messages a publishable point may rest on; below
     *               it @ref finish refuses the point. 0 disables the floor and is used only by
     *               the record-level self-test.
     */
    sub_counter_t(std::string system, std::string mode, std::size_t width, std::size_t value_bytes,
                  std::uint32_t run_id, std::uint64_t min_thru_messages)
        : system_(std::move(system)),
          mode_(std::move(mode)),
          width_(width),
          value_bytes_(value_bytes),
          run_id_(run_id),
          min_thru_messages_(min_thru_messages) {}

    /**
     * @brief Feed one received message — a composite datagram, or a single-value message.
     * @return false once the publisher's END_OF_POINT record has been seen.
     */
    bool on_message(std::span<const std::byte> d) {
        if (done_.load(std::memory_order_relaxed)) return false;
        const std::uint64_t now = now_ns();
        std::size_t off = 0;
        bool counted_message = false;
        bool counted_thru_message = false;
        while (off + kRecordHeader <= d.size()) {
            const std::uint64_t len = get_le(d, off + kOffLen, 4);
            if (len < kRecordHeader || off + len > d.size()) {
                ++bad_;
                break;
            }
            const std::span<const std::byte> r = d.subspan(off, static_cast<std::size_t>(len));
            off += static_cast<std::size_t>(len);
            // Framing, swept parameter, and the driver's per-point nonce. The nonce is the one
            // that survives a concurrent run of this same harness at this same K.
            if (get_le(r, kOffMagic, 2) != kMagic || get_le(r, kOffWidth, 2) != width_ ||
                static_cast<std::uint32_t>(get_le(r, kOffRun, 4)) != run_id_) {
                ++bad_;
                continue;
            }
            const auto phase = static_cast<phase_t>(std::to_integer<std::uint8_t>(r[kOffPhase]));
            if (phase == phase_t::END_OF_POINT) {
                done_.store(true, std::memory_order_release);
                return false;
            }
            if (!counted_message) {
                ++messages_;
                counted_message = true;
            }
            ++values_;
            if (phase == phase_t::THROUGHPUT) {
                if (thru_values_ == 0) thru_first_ = now;
                thru_last_ = now;
                ++thru_values_;
                // One message per DATAGRAM, however many records it carried — that ratio is
                // the whole measurement, so it must never be counted per record.
                if (!counted_thru_message) {
                    ++thru_messages_;
                    counted_thru_message = true;
                }
            } else {
                observe_latency(r, now);
            }
        }
        return true;
    }

    /**
     * @brief Has the publisher's END_OF_POINT record arrived?
     *
     * THE ONLY member either subscriber may touch while the receive path is live — both wait
     * loops poll it — and therefore the only one that is atomic. ThreadSanitizer found the
     * plain `bool` version racing here (write in `on_message` on the transport's recv thread
     * against the poll on the main thread) even after the counter's READ was moved behind
     * teardown, which is why it is `std::atomic` and not a comment saying it is fine.
     */
    [[nodiscard]] bool done() const { return done_.load(std::memory_order_acquire); }

    // The three below are plain counters, read only AFTER the receive path has been torn down
    // (bench_compose_net.cpp / bench_zenoh_compose.cpp scope the transport / the session for
    // exactly that) or, in test_compose_record.cpp, on a single thread. They are deliberately
    // not atomic: they are incremented per RECORD in the receive path, which is the thing being
    // measured, and making them atomic would put a read-modify-write inside it for no reader.
    /** @brief Values observed. */
    [[nodiscard]] std::uint64_t values() const { return values_; }
    /** @brief Records rejected by the magic / width / run-id check. */
    [[nodiscard]] std::uint64_t bad() const { return bad_; }
    /** @brief Throughput-phase DATAGRAMS observed — the sample count behind the rate. */
    [[nodiscard]] std::uint64_t thru_messages() const { return thru_messages_; }

    /**
     * @brief Emit the point, or refuse to.
     *
     * A point emits NO `RESULT` row at all — an engine that never reached the wire, a run
     * polluted by a foreign datagram, and a window too small to divide by must all be
     * unreportable rather than reported as a number — when any of these holds:
     *
     *  - it observed no values, or no throughput-phase values at all;
     *  - its throughput window has no positive duration;
     *  - any record was malformed (@ref compose_record);
     *  - it observed fewer than `min_thru_messages` throughput datagrams. This is the SAMPLE
     *    floor. `run_compose.sh` spends a fixed VALUE budget per point, so the datagram count
     *    falls by K and it is exactly at the largest K — where the "per-value cost stays flat"
     *    reading is taken — that the window would otherwise be thinnest. The driver both floors
     *    the group count it asks for and passes the floor the receiver enforces here, so a
     *    window that evaporated cannot come back as a rate.
     *
     * The floor is a datagram COUNT, not a duration: how long those datagrams take is a
     * property of the host, and a duration floor would bake a host-speed assumption into the
     * refusal.
     *
     * @return Process exit status: 0 when a row was emitted, 1 otherwise.
     */
    [[nodiscard]] int finish() {
        const double secs =
            thru_last_ > thru_first_ ? static_cast<double>(thru_last_ - thru_first_) / 1e9 : 0.0;
        if (values_ == 0 || thru_values_ == 0 || secs <= 0.0 || bad_ != 0 ||
            thru_messages_ < min_thru_messages_) {
            std::printf(
                "COMPOSE_FAIL\t%s\t%s\t%zu\tvalues=%llu\tthroughput_values=%llu\tbad=%llu\t"
                "span_s=%.6f\tmessages=%llu\tmin_messages=%llu\n",
                system_.c_str(), mode_.c_str(), width_, static_cast<unsigned long long>(values_),
                static_cast<unsigned long long>(thru_values_),
                static_cast<unsigned long long>(bad_), secs,
                static_cast<unsigned long long>(thru_messages_),
                static_cast<unsigned long long>(min_thru_messages_));
            std::fflush(stdout);
            return 1;
        }
        const double vps = static_cast<double>(thru_values_) / secs;
        const double mps = static_cast<double>(thru_messages_) / secs;
        const Latency::Summary s = lat_.summarize();
        emit(system_.c_str(), mode_.c_str(), value_bytes_, width_, 1, mps, vps,
             vps * static_cast<double>(value_bytes_) / 1e6, s);
        emit_tail(system_.c_str(), mode_.c_str(), value_bytes_, width_, 1, s);
        // The counts behind the rates, so a reader never has to trust a division: messages and
        // values are what the subscriber saw, and values/message is the composition property.
        std::printf("RESULT_COMPOSE\t%s\t%s\t%zu\t%zu\t%llu\t%llu\t%.3f\t%.6f\t%zu\n",
                    system_.c_str(), mode_.c_str(), width_, value_bytes_,
                    static_cast<unsigned long long>(thru_messages_),
                    static_cast<unsigned long long>(thru_values_),
                    static_cast<double>(thru_values_) / static_cast<double>(thru_messages_), secs,
                    lat_.size());
        std::fflush(stdout);
        return 0;
    }

    /**
     * @brief The WIRE-USE AUDIT pass's verdict — deliveries only, no rate.
     *
     * The audit runs the publisher under `strace` (run_compose.sh), which ptrace-stops every
     * syscall, so nothing timed under it would mean anything. This pass therefore asserts only
     * the two facts the audit exists to establish: values arrived, and none were malformed.
     *
     * @return Process exit status: 0 when the pass observed clean deliveries, 1 otherwise.
     */
    [[nodiscard]] int finish_audit() {
        const bool ok = values_ > 0 && bad_ == 0;
        std::printf("COMPOSE_AUDIT\t%s\t%s\t%zu\tmessages=%llu\tvalues=%llu\tbad=%llu\t%s\n",
                    system_.c_str(), mode_.c_str(), width_,
                    static_cast<unsigned long long>(messages_),
                    static_cast<unsigned long long>(values_), static_cast<unsigned long long>(bad_),
                    ok ? "PASS" : "FAIL");
        std::fflush(stdout);
        return ok ? 0 : 1;
    }

   private:
    /**
     * @brief Time a whole GROUP, not its first value.
     *
     * The group's send timestamp rides record 0 and the sample is taken when record `K-1`
     * lands, so the figure is "when did all K values become available at the receiver". On the
     * composite arm those are the same instant; on a per-value arm they are not, and timing
     * record 0 alone would quietly measure the easier thing for that engine.
     */
    void observe_latency(std::span<const std::byte> r, std::uint64_t now) {
        const auto idx = static_cast<std::size_t>(get_le(r, kOffIndex, 2));
        const std::uint64_t ts = get_le(r, kOffTs, 8);
        if (idx == 0) group_ts_ = ts;
        if (idx + 1 == width_ && group_ts_ != 0) {
            lat_.add(now - group_ts_);
            group_ts_ = 0;
        }
    }

    std::string system_, mode_;
    std::size_t width_, value_bytes_;
    std::uint32_t run_id_ = 0;
    std::uint64_t min_thru_messages_ = 0;
    Latency lat_;
    std::uint64_t group_ts_ = 0;
    std::uint64_t messages_ = 0, values_ = 0, bad_ = 0;
    std::uint64_t thru_messages_ = 0, thru_values_ = 0, thru_first_ = 0, thru_last_ = 0;
    std::atomic<bool> done_{false}; /**< @brief See @ref done — the one cross-thread member. */
};

}  // namespace bench::compose
