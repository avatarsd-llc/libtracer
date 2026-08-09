/**
 * @file
 * @brief Separates the THREE limits of the single-poll-thread TCP server, which a fan-in
 *        sweep cannot tell apart.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `slot_server_t::run` (`core/src/posix_endpoint.cpp`, the poll loop the tcp
 * and ws servers have shared since #871) is one thread that, per wakeup,
 * rebuilds a `pollfd` vector over every live session **under `peers_m_`** and calls `::poll`
 * on all of them. Three costs hide in that sentence and they are independent:
 *
 *  1. **One thread** serves accept and every peer, so a 24-core host runs the transport on one
 * core.
 *  2. **`poll()` rescans the whole descriptor set** in the kernel on every call.
 *  3. **The `pollfd` array is rebuilt** in userspace, under a mutex, on every pass.
 *
 * @section why Why `bench_tcp_fanin` cannot resolve them
 *
 * That bench sweeps 1..32 peers with **every peer active**. Aggregate throughput then moves for
 * two reasons at once — real demultiplex work (limit 1) and descriptor scanning (limits 2 and 3)
 * — and the sweep cannot attribute a plateau to either. Worse, at 32 active peers on a 24-core
 * host the **dialer threads compete with the server for cores**, so a plateau may be client-side.
 * Its recorded 16→32 ratio is 1.08x with overlapping ranges: the instrument cannot establish its
 * own conclusion, which is why [ADR-0071] names this file as prerequisite to acting on any of it.
 *
 * @section arms The two arms, and what each one alone can prove
 *
 * **`idle-fanout` — the decisive arm.** ONE active sender at full rate, while the number of
 * CONNECTED-BUT-SILENT peers sweeps. Idle peers are raw `::connect`ed sockets: no dialer thread,
 * no client CPU, no frames. The server treats them as live sessions regardless — it has no
 * handshake, so a peer is in the poll set the moment it is accepted
 * (`slot_server_t::accept_peer`, `core/src/posix_endpoint.cpp`).
 *
 * Therefore **every nanosecond this arm loses is scanning overhead**, limits 2 and 3, with the
 * work held exactly constant. If the active peer's rate is flat across the sweep, those two
 * limits do not bind at that width and only limit 1 is left to argue about. If it falls, the
 * slope IS the per-idle-peer cost of the design, and it is a cost paid by a peer that is doing
 * nothing at all.
 *
 * **`active-fanout`** sweeps ACTIVE peers, capped well below the core count so dialers never
 * starve the server. This prices limit 1 on its own: per-peer rate falling as 1/N while
 * aggregate is flat means the one thread is saturated.
 *
 * @section reading Reading it honestly
 *
 * Each point is repeated and reported as median with min/max, because a single ratio off a
 * contended arm is how this project has previously convinced itself of effects that were not
 * there. A trend claim needs the min/max columns to be disjoint between the ends of the sweep;
 * where they overlap the honest reading is **no claim**, and the summary says so rather than
 * printing a ratio.
 *
 * Loopback, one host, one process: this prices the server's demultiplex, not a NIC and not a
 * real network. It is a DIAGNOSTIC, not a CI gate — thread-contention numbers are runner-
 * dependent, the same call `bench_tcp_fanin` and `bench_lkv_slot` make.
 *
 * @section traps Two ways this file was wrong before it was right
 *
 * Both are recorded because each produced a confident, reproducible, WRONG number, and the
 * second is not obvious even in hindsight.
 *
 * **The settle time co-varied with the swept variable.** The first version slept
 * `200 + 2*idle` ms before timing, so a low-idle point spent part of its window not yet
 * connected while a high-idle point was fully settled. It reported that 512 idle peers made
 * the server 2.4x FASTER. The fix is not a longer sleep — the server accepts at most ONE peer
 * per poll pass, so the drain time for a deep backlog is not knowable up front. WAIT on
 * `enumerate_peers` until the fixture actually exists, and skip the point if it never does.
 *
 * **At max offered load the batch size is free to move, and it moves the wrong way.** With the
 * fixture fixed, the arm STILL reported 2.7x faster. More descriptors make each poll pass
 * longer, which lets more bytes accumulate between passes, so the server drains a bigger batch
 * per wakeup. The scan does cost more; it is simply amortised over more work, and the net is
 * positive. An arm that lets throughput float therefore measures net-of-batching while
 * claiming to measure scanning. Pacing the sender below saturation pins the batch, which is
 * what makes the latency column mean what it says.
 *
 * The general form, worth carrying to the next instrument: when a sweep produces an
 * IMPOSSIBLE result, the instrument is telling you which variable you failed to hold.
 *
 * **Third: a negative result was a width, not an answer.** Swept to 512 idle peers the arm
 * reports that limits 2 and 3 do not bind, and that reading is correct AT 512. Swept to 8192
 * the server delivers ~77% of a pinned 100k frames/s while the sender's own p50 stays flat —
 * the shortfall is entirely the server, and 8,193 `pollfd` entries rebuilt under a mutex per
 * pass is what it is. The knee sits between 4096 and 8192. `LIBTRACER_BENCH_IDLE_MAX` exists
 * so the width is a parameter rather than a constant somebody has to notice.
 *
 * That failure mode is also why the paced arm checks whether the PACER STILL HOLDS. Pinning
 * the offered rate makes latency the signal, but only while the server can drain what is
 * offered; past that the shortfall lands in the throughput column the paced verdict is
 * deliberately not reading, and a latency-only verdict announces "does not bind" at exactly
 * the width where it finally does.
 */

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/transport_tcp.hpp"

namespace {

using namespace std::chrono_literals;
using namespace bench;  // Latency, now_ns — the shared harness helpers

/**
 * @brief Idle (connected, silent) peer counts swept while the active load is held at one.
 *
 * Doubling from 8 up to `LIBTRACER_BENCH_IDLE_MAX` (default 512). Configurable because the
 * question this arm answers is width-dependent: a negative result at 512 says the O(peers)
 * terms do not bind AT 512, and the only way to find where they do is to widen. The server
 * accepts at most one peer per poll pass, so a deep sweep spends real time forming its
 * fixture — which the fixture wait handles rather than assumes.
 */
[[nodiscard]] std::vector<std::size_t> idle_counts() {
    const char* const env = std::getenv("LIBTRACER_BENCH_IDLE_MAX");
    const std::size_t cap = env != nullptr ? std::strtoull(env, nullptr, 10) : 512;
    std::vector<std::size_t> v{0};
    for (std::size_t n = 8; n <= cap; n *= 2) v.push_back(n);
    return v;
}

/** @brief Active peer counts. Capped at 8 on a 24-core host so dialers never starve the server —
 *         the confound that makes `bench_tcp_fanin`'s widest point unreadable. */
constexpr std::size_t kActiveCounts[] = {1, 2, 4, 8};

/** @brief Payload bytes per frame — small, so what is measured is per-FRAME demultiplex. */
constexpr std::size_t kFrameBytes = 64;

/**
 * @brief Inter-frame gap for the PACED idle arm — 10 us, i.e. ~100k frames/s offered.
 *
 * Chosen well below the ~600k frames/s this server reaches unpaced, so the send buffer
 * never backs up and the batch the server drains per wakeup stays constant across the
 * sweep. That is the whole point: at max load the batch grows with the poll-pass duration
 * and hides the scan behind an amortisation win.
 */
[[nodiscard]] std::uint64_t paced_gap_ns() {
    const char* const env = std::getenv("LIBTRACER_BENCH_PACED_HZ");
    const std::uint64_t hz = env != nullptr ? std::strtoull(env, nullptr, 10) : 100000;
    return hz > 0 ? 1000000000ULL / hz : 0;
}

/** @brief Repeats per sweep point. Medians of fewer than five have misled this project before. */
constexpr int kReps = 5;

/** @brief Seconds each repeat runs. Override with LIBTRACER_BENCH_SECONDS. */
[[nodiscard]] double window_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return 0.5;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : 0.5;
}

/**
 * @brief Counts inbound frames on the server without serialising its poll thread.
 *
 * A relaxed increment and nothing else. The receiver runs ON the poll thread, so any work it
 * does is charged directly to the term under test — a mutex here would measure the sink and a
 * payload copy would measure the allocator.
 */
struct counting_sink_t {
    std::atomic<std::uint64_t> frames{0};

    /** @brief The peer-named receiver callable (bound by address). */
    void operator()(std::string_view, std::span<const std::byte>) {
        frames.fetch_add(1, std::memory_order_relaxed);
    }
};

/**
 * @brief A connected socket that never sends — an idle peer with NO client-side thread.
 *
 * This is what makes the idle arm attributable. A `tcp_transport_t` would bring its own receive
 * thread, so 512 idle peers would be 512 threads perturbing the very scheduler under test. A raw
 * descriptor costs the client nothing and still occupies a full session slot on the server,
 * because the server has no handshake phase.
 */
class idle_peer_t {
   public:
    explicit idle_peer_t(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    idle_peer_t(const idle_peer_t&) = delete;
    idle_peer_t& operator=(const idle_peer_t&) = delete;
    idle_peer_t(idle_peer_t&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    idle_peer_t& operator=(idle_peer_t&&) = delete;
    ~idle_peer_t() {
        if (fd_ >= 0) ::close(fd_);
    }
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

   private:
    int fd_ = -1;
};

/** @brief One repeat's outcome. */
struct rep_t {
    double frames_per_s = 0.0;
    std::uint64_t p50_ns = 0;
    std::uint64_t p99_ns = 0;
};

/**
 * @brief Run one point: @p active senders and @p idle silent peers against one server.
 *
 * The peers are all connected and settled BEFORE the timed window, so what is priced is
 * steady-state demultiplex rather than N connects.
 */
[[nodiscard]] rep_t run_point(std::size_t active, std::size_t idle, double seconds,
                              std::uint64_t gap_ns) {
    counting_sink_t sink;
    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/0,
                                         /*peer_named=*/true);
    if (!server.ok() || server.bus() == nullptr) return rep_t{};
    server.bus()->set_peer_receiver(sink);
    const std::uint16_t port = server.local_port();

    std::vector<idle_peer_t> idlers;
    idlers.reserve(idle);
    for (std::size_t i = 0; i < idle; ++i) {
        idlers.emplace_back(port);
        if (!idlers.back().ok()) return rep_t{};
    }

    std::vector<std::unique_ptr<tr::net::tcp_transport_t>> dialers;
    dialers.reserve(active);
    for (std::size_t i = 0; i < active; ++i)
        dialers.push_back(std::make_unique<tr::net::tcp_transport_t>("127.0.0.1", port));

    // WAIT FOR THE FIXTURE, do not sleep for it. The server accepts at most ONE peer per poll
    // pass, so the time to drain a 512-deep backlog is not a constant and is not knowable up
    // front. An earlier version of this bench slept `200 + 2*idle` ms, which made the settle
    // time CO-VARY WITH THE SWEPT VARIABLE: the low-idle points spent part of their window not
    // yet connected, and the arm reported that adding 512 idle descriptors made the server
    // 2.4x FASTER. Poll `enumerate_peers` until every peer this point needs is actually in the
    // server's session list, and give up rather than measure a fixture that never formed.
    const std::size_t want = idle + active;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    std::size_t seen = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        seen = 0;
        server.bus()->enumerate_peers([&seen](std::string_view) { ++seen; });
        if (seen >= want) break;
        std::this_thread::sleep_for(1ms);
    }
    if (seen < want) {
        std::fprintf(stderr, "SKIP active=%zu idle=%zu: only %zu of %zu peers accepted\n", active,
                     idle, seen, want);
        return rep_t{};
    }

    const std::vector<std::byte> payload(kFrameBytes, std::byte{0x5A});
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::vector<Latency> lat(active);

    std::vector<std::thread> senders;
    senders.reserve(active);
    for (std::size_t i = 0; i < active; ++i) {
        senders.emplace_back([&, i] {
            Latency mine;
            mine.reserve(1 << 16);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            // PACED when `gap_ns` is non-zero. At max offered load the server's batch size is
            // free to vary with the poll-pass duration, so adding idle peers makes each pass
            // slower, lets MORE bytes accumulate between passes, and the bigger drain per
            // wakeup shows up as higher throughput and lower per-frame cost — the opposite of
            // the scanning cost, and larger than it. Holding the offered rate fixed pins the
            // batch, so what is left to move is the scan.
            std::uint64_t next = now_ns();
            while (!stop.load(std::memory_order_relaxed)) {
                if (gap_ns != 0) {
                    next += gap_ns;
                    while (now_ns() < next) {
                        if (stop.load(std::memory_order_relaxed)) break;
                    }
                }
                const std::uint64_t t0 = now_ns();
                dialers[i]->send(std::span<const std::byte>(payload));
                mine.add(now_ns() - t0);
            }
            lat[i] = std::move(mine);
        });
    }

    // Warm up to steady state BEFORE the clock starts, so the window prices demultiplex and
    // not the first frames through a cold path. Gated on frames actually arriving, so it costs
    // nothing at high rates and cannot silently pass when nothing is flowing.
    go.store(true, std::memory_order_release);
    const auto warm_until = std::chrono::steady_clock::now() + 100ms;
    while (std::chrono::steady_clock::now() < warm_until &&
           sink.frames.load(std::memory_order_relaxed) < 1000)
        std::this_thread::sleep_for(1ms);

    const std::uint64_t before = sink.frames.load(std::memory_order_relaxed);
    const auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& t : senders) t.join();
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    // Drain: frames already on the wire when the window closed are still arriving.
    std::this_thread::sleep_for(50ms);
    const std::uint64_t got = sink.frames.load(std::memory_order_relaxed) - before;

    Latency pooled;
    for (Latency& l : lat) pooled.merge(l);
    const Latency::Summary s = pooled.summarize();
    return rep_t{elapsed > 0.0 ? static_cast<double>(got) / elapsed : 0.0, s.p50, s.p99};
}

/** @brief Median of a copy — the reported statistic for every point. */
[[nodiscard]] double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::ranges::sort(v);
    return v[v.size() / 2];
}

/** @brief Run and print one sweep. @p vary_idle selects which axis moves. */
void sweep(bool vary_idle, double seconds) {
    const char* const label = vary_idle ? "idle-fanout" : "active-fanout";
    std::printf("\n%s — %s\n", label,
                vary_idle ? "1 sender PACED at ~100k f/s so the drain batch cannot grow; "
                            "idle peers are connected and silent, so latency movement here "
                            "is descriptor scanning (limits 2+3)"
                          : "all peers active, capped below the core count; prices the single "
                            "thread itself (limit 1)");
    std::printf("%-10s %-14s %-12s %-12s %-10s %-10s\n", vary_idle ? "idle" : "active",
                "median f/s", "min f/s", "max f/s", "p50 ns", "p99 ns");

    const double offered_hz =
        vary_idle && paced_gap_ns() > 0 ? 1e9 / static_cast<double>(paced_gap_ns()) : 0.0;
    double last_rate_med = 0.0;
    std::vector<double> first_med;
    std::vector<double> last_med;
    std::vector<double> first_all;
    std::vector<double> last_all;
    const std::vector<std::size_t> idles = idle_counts();
    const std::size_t n_points = vary_idle ? idles.size() : std::size(kActiveCounts);

    for (std::size_t pi = 0; pi < n_points; ++pi) {
        const std::size_t idle = vary_idle ? idles[pi] : 0;
        const std::size_t active = vary_idle ? 1 : kActiveCounts[pi];
        std::vector<double> rates;
        std::vector<std::uint64_t> p50s;
        std::vector<std::uint64_t> p99s;
        for (int r = 0; r < kReps; ++r) {
            const rep_t rep = run_point(active, idle, seconds, vary_idle ? paced_gap_ns() : 0);
            rates.push_back(rep.frames_per_s);
            p50s.push_back(rep.p50_ns);
            p99s.push_back(rep.p99_ns);
        }
        std::ranges::sort(p50s);
        std::ranges::sort(p99s);
        const double med = median_of(rates);
        std::printf("%-10zu %-14.0f %-12.0f %-12.0f %-10llu %-10llu\n", vary_idle ? idle : active,
                    med, *std::ranges::min_element(rates), *std::ranges::max_element(rates),
                    static_cast<unsigned long long>(p50s[p50s.size() / 2]),
                    static_cast<unsigned long long>(p99s[p99s.size() / 2]));
        // The paced arm PINS throughput on purpose, so judging it on rates would test the
        // pacer. Its signal is latency; the unpaced arm's signal is throughput. Each arm is
        // judged on the column that is free to move.
        std::vector<double> signal;
        if (vary_idle)
            for (std::uint64_t v : p50s) signal.push_back(static_cast<double>(v));
        else
            signal = rates;
        const double sig_med = median_of(signal);
        if (pi + 1 == n_points) last_rate_med = med;
        if (pi == 0) {
            first_med.push_back(sig_med);
            first_all = signal;
        }
        if (pi + 1 == n_points) {
            last_med.push_back(sig_med);
            last_all = signal;
        }
    }

    // The verdict is allowed to be "no claim". Overlapping ends mean the sweep did not resolve
    // anything, and saying so is the result — printing a ratio there is how a contended arm
    // manufactures a finding.
    if (first_all.empty() || last_all.empty()) return;
    const double lo_first = *std::ranges::min_element(first_all);
    const double hi_first = *std::ranges::max_element(first_all);
    const double lo_last = *std::ranges::min_element(last_all);
    const double hi_last = *std::ranges::max_element(last_all);
    const bool overlap = lo_first < hi_last && lo_last < hi_first;
    if (overlap) {
        std::printf(
            "  VERDICT no claim — the sweep's ends OVERLAP on %s (%.0f..%.0f vs %.0f..%.0f). "
            "This arm did not resolve its question.\n",
            vary_idle ? "p50 ns" : "frames/s", lo_first, hi_first, lo_last, hi_last);
        return;
    }
    // THE PACER LOSING CONTROL IS ITSELF THE RESULT. This arm pins the offered rate so that
    // latency is the signal — but that only holds while the server can still drain what is
    // offered. Once it cannot, the shortfall moves into the throughput column that the paced
    // verdict is deliberately NOT reading, and a latency-only verdict would report "does not
    // bind" at exactly the width where it finally does. Check for it explicitly.
    if (vary_idle && offered_hz > 0.0) {
        const double achieved = last_rate_med;
        if (achieved < 0.95 * offered_hz) {
            std::printf(
                "  VERDICT BINDS at %zu idle peers — the server delivered %.0f f/s against a "
                "pinned %.0f f/s offered (%.0f%%).\n           The sender kept writing (p50 is "
                "flat), so the shortfall is the server: that IS the O(peers) poll + locked "
                "rebuild, and this width is the knee.\n",
                idles.back(), achieved, offered_hz, 100.0 * achieved / offered_hz);
            return;
        }
    }
    const double ratio = last_med[0] / first_med[0];
    if (vary_idle)
        std::printf(
            "  VERDICT p50 x%.2f across %zu idle peers doing NOTHING, at a PINNED offered "
            "rate.\n           %s\n",
            ratio, idles.back(),
            ratio > 1.0 ? "ABOVE 1.0 — that rise IS the O(peers) poll + locked rebuild."
                        : "AT OR BELOW 1.0 — limits 2+3 do NOT bind at this width. A ratio "
                          "under 1.0 is NOT a win and is not claimed as one: nothing here "
                          "explains why latency would fall, only that it does not rise.");
    else
        std::printf("  VERDICT CLEAN x%.2f aggregate across the active sweep.\n", ratio);
}

}  // namespace

int main() {
    const double seconds = window_seconds();
    std::printf(
        "TCP peer scaling — which of the single-poll-thread's three limits actually binds?\n"
        "Repeats: %d per point, %.2fs each. Medians reported with min/max so a trend claim can\n"
        "be checked for overlap rather than taken on the ratio alone.\n",
        kReps, seconds);
    sweep(/*vary_idle=*/true, seconds);
    sweep(/*vary_idle=*/false, seconds);
    return 0;
}
