/**
 * @file
 * @brief The TCP server's END-TO-END baseline: aggregate throughput AND the full one-way
 *        latency distribution (p50 / p99 / p999 / max) swept over concurrent peers.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two instruments already touch this seam and neither answers the question. `bench_tcp_fanin`
 * sweeps peer count but records NO latency at all — it counts frames and nothing else, so the
 * plateau it reports is a throughput ceiling with no idea what the tail costs to reach it.
 * `bench_transports` records latency properly (two processes, real one-way clock) but drives
 * exactly ONE connection, so it says nothing about concurrency. This bench is the join: the
 * fan-in topology of the first with the timestamped payload of the second.
 *
 * @section phases Three phases per sweep point, each on a FRESH server
 *
 * Every phase constructs its own `transport_tcp_server` and its own dialers and destroys them
 * before the collector is read. That is deliberate: the receiver runs ON the poll thread, so
 * the only race-free moment to read its `Latency` vector is after the server's destructor has
 * joined that thread. A shared server across phases would need a quiescence protocol whose
 * correctness is harder to argue than the 0.35 s a fresh one costs.
 *
 *  - **LAT** — every sender PACES its frames (`sleep_for`, not a spin: at 24 senders a spin
 *    pace would put 25 runnable threads on 24 cores and the latency would then be measuring
 *    the scheduler). Pace jitter lands entirely BEFORE the send timestamp is taken, so it
 *    cannot contaminate the measured one-way latency — it only makes the arrival process
 *    slightly sparser than nominal, which is the safe direction for an UNLOADED number.
 *  - **THRU** — senders blast; the sink does one relaxed increment and nothing else. This is
 *    the arm comparable to `bench_tcp_fanin`.
 *  - **LOAD** — senders blast AND tag 1 frame in `kLoadSampleEvery` as a latency probe, so the
 *    tail is measured at saturation. `LOAD`'s throughput against `THRU`'s is the CONTROL for
 *    this phase: the sink does strictly more work here, and if that shows up as a throughput
 *    drop then the loaded latency figure is describing an instrument, not the transport.
 *
 * @section vacuous Guards against measuring nothing
 *
 * A fan-in bench that quietly ran with 3 of 24 peers connected would report a number that
 * looks entirely plausible. Every phase therefore WAITS for `enumerate_peers` to report
 * exactly N open peers before the window opens and aborts the point if it does not, and every
 * phase asserts it delivered a non-zero frame count. `peers_seen` is printed on every line so
 * a reader can check the topology actually under test rather than the one requested.
 *
 * @section honest What this is NOT
 *
 * Loopback, ONE process: senders and the single poll thread contend for the same 24 cores, so
 * at the top of the sweep the senders are a term in their own right. It prices the server's
 * demultiplex, not a NIC. The latency is one-way `CLOCK_MONOTONIC` within one process, so it
 * carries no cross-process clock question — and no cross-process cost either.
 *
 * `max` is an order statistic of the WHOLE sample, so it grows with `n` by construction: it is
 * comparable across repeats at one sweep point and NOT comparable between points with
 * different `n`. p50/p99/p999 are the figures to read across the sweep.
 *
 *     bench_tcp_baseline                       # forward sweep
 *     LIBTRACER_BENCH_REVERSE=1 ...            # reverse sweep (order-effect control)
 *     LIBTRACER_BENCH_SECONDS=2 ...            # blast window per throughput phase
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/transport_tcp.hpp"

namespace {

using namespace std::chrono_literals;
using bench::Latency;
using bench::now_ns;

/** @brief Concurrent peers swept. 24 is the host's core count — the interesting top end. */
constexpr std::size_t kPeerCounts[] = {1, 2, 4, 8, 16, 24};

/** @brief Payload bytes per frame. 64 B matches `bench_tcp_fanin`, so the throughput arm of
 *         this bench and the claim on record are the same measurement. */
constexpr std::size_t kFrameBytes = 64;

/** @brief Nominal per-sender gap between PACED latency probes. The realized gap is larger
 *         (`sleep_for` overshoots by tens of µs), which only lowers the offered load. */
constexpr std::uint64_t kPaceNs = 100000;

/** @brief Latency probes each sender emits in the paced phase, before the floor below. */
constexpr std::size_t kPacedTotalTarget = 12000;

/** @brief Per-sender minimum, so a wide point still gets a window worth measuring rather
 *         than a 20 ms transient. */
constexpr std::size_t kPacedPerSenderFloor = 2000;

/** @brief In the LOAD phase, 1 frame in this many is timestamp-sampled into the collector. */
constexpr std::size_t kLoadSampleEvery = 64;

/** @brief Payload offsets — `[0..7] send ts LE`, `[8] phase`, filler after. */
constexpr std::size_t kPhaseOffset = 8;

/** @brief What a frame is for, read by the sink off byte 8. */
enum class frame_kind_t : std::uint8_t { BULK = 0, PROBE = 1 };

/** @brief Blast window seconds per throughput phase. `LIBTRACER_BENCH_SECONDS` overrides. */
[[nodiscard]] double window_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return 1.0;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : 1.0;
}

/** @brief True when the sweep should run widest-first (the order-effect control arm). */
[[nodiscard]] bool reverse_sweep() {
    const char* const env = std::getenv("LIBTRACER_BENCH_REVERSE");
    return env != nullptr && env[0] == '1';
}

/** @brief Which phase a run is: what the senders do and what the sink records. */
enum class phase_t {
    LAT,  /**< @brief Paced probes, every frame timestamped and recorded. */
    THRU, /**< @brief Blast, count only — no timestamps read by the sink. */
    LOAD  /**< @brief Blast, 1-in-`kLoadSampleEvery` recorded — the loaded tail. */
};

/**
 * @brief The peer-named sink. Runs ON the poll thread and is touched by nothing else while
 *        the server lives, so the collector needs no lock — the server's destructor joins the
 *        poll thread and only then does the caller read `lat`.
 *
 * The counter is a relaxed atomic purely so the sender side could observe progress; the sink
 * itself is single-threaded. `lat.add` is a `push_back` into a reserved vector, which is the
 * whole extra cost the LOAD phase pays over THRU.
 */
struct sink_t {
    std::atomic<std::uint64_t> frames{0};
    Latency lat;
    bool record = false;

    /** @brief The peer-named receiver callable, bound by address. */
    void operator()(std::string_view, std::span<const std::byte> f) {
        frames.fetch_add(1, std::memory_order_relaxed);
        if (!record || f.size() <= kPhaseOffset) return;
        if (static_cast<frame_kind_t>(std::to_integer<std::uint8_t>(f[kPhaseOffset])) !=
            frame_kind_t::PROBE)
            return;
        std::uint64_t ts = 0;
        for (std::size_t i = 0; i < 8; ++i)
            ts |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(f[i])) << (8 * i);
        const std::uint64_t now = now_ns();
        // A frame that somehow carries a future timestamp would wrap to a colossal unsigned
        // latency and poison the max; drop it rather than let it define the tail.
        if (now >= ts) lat.add(now - ts);
    }
};

/** @brief Stamp `out` with a fresh send timestamp and its kind, immediately before the send. */
void stamp(std::vector<std::byte>& out, frame_kind_t kind) {
    const std::uint64_t ts = now_ns();
    for (std::size_t i = 0; i < 8; ++i) out[i] = static_cast<std::byte>((ts >> (8 * i)) & 0xFF);
    out[kPhaseOffset] = static_cast<std::byte>(kind);
}

/** @brief One phase's outcome. `ok` false means the point measured nothing and must not be read. */
struct result_t {
    std::size_t peers = 0;
    std::size_t peers_seen = 0;
    std::uint64_t frames = 0;    /**< @brief Delivered by the clock stop — the honest count. */
    std::uint64_t frames_fi = 0; /**< @brief Delivered by sender-join — the `bench_tcp_fanin`
                                  *          convention, kept only so the claim on record can be
                                  *          checked against its own instrument's arithmetic. */
    std::uint64_t sent = 0;
    std::uint64_t dropped = 0;   /**< @brief CONTROL: RX-backend exhaustion. Must stay 0. */
    std::uint64_t malformed = 0; /**< @brief CONTROL: framing desync. Must stay 0. */
    double seconds = 0.0;
    Latency::Summary lat;
    bool ok = false;
};

/**
 * @brief Dial one peer FEWER than asked, to prove the anti-vacuity guard is not decorative.
 *
 * `LIBTRACER_BENCH_UNDERDIAL=1` makes every point start `peers - 1` dialers. If the guard
 * works, every line comes back `ok=0` with `peers_seen = peers - 1` and no number at all. A
 * guard that cannot be made to fail has never been shown to fire.
 */
[[nodiscard]] std::size_t underdial() {
    const char* const env = std::getenv("LIBTRACER_BENCH_UNDERDIAL");
    return env == nullptr ? 0 : static_cast<std::size_t>(std::strtoul(env, nullptr, 10));
}

/** @brief Count the server's currently-open peers (the anti-vacuity check). */
[[nodiscard]] std::size_t open_peers(tr::net::transport_tcp_server& server) {
    std::size_t n = 0;
    server.bus()->enumerate_peers([&n](std::string_view) { ++n; });
    return n;
}

/**
 * @brief Run one (peers, phase) point end to end on a fresh server.
 *
 * The collector is read only after `server` is destroyed, which joins the poll thread — the
 * one point at which the sink's vector is provably quiescent.
 */
[[nodiscard]] result_t run_point(std::size_t peers, phase_t phase, double seconds) {
    result_t r;
    r.peers = peers;
    sink_t sink;
    sink.record = phase != phase_t::THRU;
    // Reserve so no `push_back` in the LOAD phase can reallocate on the poll thread and be
    // charged to the transport. Generous: the paced phase never approaches it.
    sink.lat.reserve(4u * 1000u * 1000u);

    std::uint64_t frames = 0;
    std::uint64_t frames_fi = 0;
    std::uint64_t sent = 0;
    std::uint64_t dropped = 0;
    std::uint64_t malformed = 0;
    double span = 0.0;
    std::size_t seen = 0;
    {
        tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/0,
                                             /*peer_named=*/true);
        if (!server.ok() || server.bus() == nullptr) return r;
        server.bus()->set_peer_receiver(sink);
        const std::uint16_t port = server.local_port();

        const std::size_t dial = peers > underdial() ? peers - underdial() : 0;
        std::vector<std::unique_ptr<tr::net::tcp_transport_t>> dialers;
        dialers.reserve(dial);
        for (std::size_t i = 0; i < dial; ++i)
            dialers.push_back(std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", port));

        // Anti-vacuity: a fan-in point run with fewer peers than requested reports a
        // perfectly plausible-looking number for the wrong topology. Wait for the server to
        // actually see all N, and refuse the point if it never does.
        for (int i = 0; i < 200 && seen < peers; ++i) {
            std::this_thread::sleep_for(10ms);
            seen = open_peers(server);
        }
        if (seen != peers) {
            r.peers_seen = seen;
            return r;
        }
        std::this_thread::sleep_for(200ms);

        std::atomic<bool> go{false};
        std::atomic<bool> stop{false};
        std::atomic<std::uint64_t> sent_total{0};
        const std::size_t paced_each =
            std::max(kPacedPerSenderFloor, kPacedTotalTarget / std::max<std::size_t>(peers, 1));

        std::vector<std::thread> senders;
        senders.reserve(peers);
        for (std::size_t i = 0; i < peers; ++i) {
            senders.emplace_back([&, i] {
                std::vector<std::byte> payload(kFrameBytes, std::byte{0x5A});
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                std::uint64_t n = 0;
                if (phase == phase_t::LAT) {
                    for (std::size_t k = 0; k < paced_each; ++k) {
                        stamp(payload, frame_kind_t::PROBE);
                        dialers[i]->send(std::span<const std::byte>(payload));
                        ++n;
                        std::this_thread::sleep_for(std::chrono::nanoseconds(kPaceNs));
                    }
                } else {
                    while (!stop.load(std::memory_order_relaxed)) {
                        stamp(payload, (phase == phase_t::LOAD && n % kLoadSampleEvery == 0)
                                           ? frame_kind_t::PROBE
                                           : frame_kind_t::BULK);
                        dialers[i]->send(std::span<const std::byte>(payload));
                        ++n;
                    }
                }
                sent_total.fetch_add(n, std::memory_order_relaxed);
            });
        }

        // Zero at the last possible moment: the settle above may have carried stray frames.
        sink.frames.store(0, std::memory_order_relaxed);
        const auto t0 = std::chrono::steady_clock::now();
        go.store(true, std::memory_order_release);
        if (phase == phase_t::LAT) {
            for (std::thread& t : senders) t.join();
            // The paced phase ends when the last probe has been DELIVERED, not when the last
            // one was handed to the kernel; give the poll thread a bounded moment to drain.
            std::this_thread::sleep_for(200ms);
            const auto t1 = std::chrono::steady_clock::now();
            frames = frames_fi = sink.frames.load(std::memory_order_relaxed);
            span = std::chrono::duration<double>(t1 - t0).count() - 0.2;  // less the drain pause
        } else {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            stop.store(true, std::memory_order_relaxed);
            // Read the counter and the clock TOGETHER, before joining. Frames still in the
            // kernel are simply not counted, which biases the rate DOWN — the safe direction
            // for a ceiling. `bench_tcp_fanin` instead stops its clock here and reads its
            // counter AFTER the join, so every frame the poll thread drains while the senders
            // wind down (with no sender contention, so at its FASTEST) is credited to a window
            // that excludes the wind-down: an UPWARD bias on the very number its VERDICT line
            // is built from. Both counts are recorded here so the size of that bias is a
            // measurement rather than an argument.
            const auto t1 = std::chrono::steady_clock::now();
            frames = sink.frames.load(std::memory_order_relaxed);
            span = std::chrono::duration<double>(t1 - t0).count();
            for (std::thread& t : senders) t.join();
            frames_fi = sink.frames.load(std::memory_order_relaxed);
        }
        sent = sent_total.load(std::memory_order_relaxed);
        dropped = server.dropped_rx();
        malformed = server.malformed_rx();
        // Dialers die before the server so the poll thread sees clean hangups rather than
        // racing its own teardown.
        dialers.clear();
    }
    // The poll thread is joined; the collector is ours.
    r.peers_seen = seen;
    r.frames = frames;
    r.frames_fi = frames_fi;
    r.sent = sent;
    r.dropped = dropped;
    r.malformed = malformed;
    r.seconds = span;
    r.lat = sink.lat.summarize();
    // A point is readable only if the topology was the requested one, frames actually arrived,
    // and neither control counter moved. Any of those failing makes the row a non-measurement.
    r.ok = seen == peers && frames > 0 && dropped == 0 && malformed == 0;
    return r;
}

}  // namespace

int main() {
    const double seconds = window_seconds();
    const bool rev = reverse_sweep();
    std::vector<std::size_t> order(std::begin(kPeerCounts), std::end(kPeerCounts));
    if (rev) std::reverse(order.begin(), order.end());

    std::printf(
        "# bench_tcp_baseline: transport_tcp_server (peer-named, one poll thread), %zu B frames\n"
        "# hardware threads %u | blast window %.2f s | sweep %s | pace %llu ns | "
        "load sample 1/%zu\n",
        kFrameBytes, std::thread::hardware_concurrency(), seconds, rev ? "REVERSE" : "FORWARD",
        static_cast<unsigned long long>(kPaceNs), kLoadSampleEvery);
    std::printf(
        "# cols: TCPB phase peers peers_seen ok frames frames_fanin_conv sent secs f/s "
        "f/s_fanin_conv n p50ns p99ns p999ns maxns tail_ok dropped malformed\n");

    for (const std::size_t n : order) {
        const result_t lat = run_point(n, phase_t::LAT, seconds);
        const result_t thru = run_point(n, phase_t::THRU, seconds);
        const result_t load = run_point(n, phase_t::LOAD, seconds);

        const auto rate = [](std::uint64_t f, double s) {
            return s > 0.0 ? static_cast<double>(f) / s : 0.0;
        };
        // TSV so a shell can pick columns without a parser. One line per phase; every line
        // carries peers_seen, the control counters and `ok`, so a reader can reject a vacuous
        // point on sight.
        const auto line = [&](const char* tag, const result_t& r) {
            std::printf(
                "TCPB\t%s\t%zu\t%zu\t%d\t%llu\t%llu\t%llu\t%.6f\t%.0f\t%.0f\t%zu\t%llu\t%llu\t%llu"
                "\t%llu\t%d\t%llu\t%llu\n",
                tag, r.peers, r.peers_seen, r.ok ? 1 : 0, static_cast<unsigned long long>(r.frames),
                static_cast<unsigned long long>(r.frames_fi),
                static_cast<unsigned long long>(r.sent), r.seconds, rate(r.frames, r.seconds),
                rate(r.frames_fi, r.seconds), r.lat.n, static_cast<unsigned long long>(r.lat.p50),
                static_cast<unsigned long long>(r.lat.p99),
                static_cast<unsigned long long>(r.lat.p999),
                static_cast<unsigned long long>(r.lat.max), r.lat.tail_ok ? 1 : 0,
                static_cast<unsigned long long>(r.dropped),
                static_cast<unsigned long long>(r.malformed));
            std::fflush(stdout);
        };
        line("LAT", lat);
        line("THRU", thru);
        line("LOAD", load);
    }
    return 0;
}
