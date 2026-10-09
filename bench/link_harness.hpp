/**
 * @file
 * @brief The engine-neutral driver of the Noise link harness (#2065): one request/reply row, one
 *        streaming row, one session-setup row and one RAM reading per link arm, written once
 *        and run unchanged over libtracer's links (`bench_noise_link`) and Zenoh's
 *        (`bench_zenoh_link`).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * An ARM is a factory for a `link_pair_t`: a client end and a server end in this process,
 * joined over loopback by whatever link the arm names (a libtracer UDP link, a libtracer Noise
 * link, a Zenoh session pair over UDP, TLS or QUIC). The pair reports every frame its server
 * end receives to `rx_state_t::on_server` and echoes it back when told to, and reports every
 * frame its client end receives to `rx_state_t::on_client`. Everything else — what a row
 * sends, when the clock starts and stops, what counts as delivered — is here, once, so the
 * engines cannot be measured by different code.
 *
 * @section setup One session, warm
 *
 * Every arm builds exactly ONE pair for its request/reply and streaming rows, completes one
 * exchange on it before any clock starts, and keeps it for every size. Session setup is its
 * own row (`link-<arm>-setup`): a fresh pair, timed from the factory call to the first reply,
 * repeated, so a handshake is priced separately from the per-frame cost and never amortized
 * into it. The RAM reading is taken around the arm's FIRST pair in the process, so it includes
 * whatever the engine starts once per process (Zenoh's runtime threads, for one).
 *
 * @section rows Rows
 *
 *  - `link-<arm>-rr` — one request and its echoed reply, one outstanding: the RTT
 *    distribution; `pub/s` = completed exchanges per second, `MB/s` = request bytes per second.
 *  - `link-<arm>-stream` — the client blasts for the window; the server counts. `pub/s` = sent
 *    per second, `deliv/s` = delivered per second, `MB/s` = delivered payload; the latency
 *    columns are the one-way latency of 1 frame in 64, stamped at send (the loaded latency). A
 *    `NOTE` line carries sent, delivered and the loss.
 *  - `link-<arm>-stream-batch` — the same window with K values per send, packed into one
 *    batch frame (`compose_batch`, RFC-0008's batch carriage) and unpacked by the server end;
 *    the `fanout` column carries K = min(16, what fits one frame), and every rate counts
 *    VALUES. Value sizes 64 B, 1 KiB and 4 KiB. It shows what one send's fixed cost (a
 *    syscall, and on the Noise arm one seal) comes to spread over K values. Arms whose engine
 *    batches by itself stage nothing and skip the row.
 *  - `link-<arm>-setup` — fresh pair to first reply, in ns. Each setup also waits out the
 *    harness's own re-send grain (100 µs), so a setup row below about 100 µs is quantized.
 *  - `LINK_RAM system arm metric value` — `heap_live_bytes` (both ends, after the first
 *    exchange, counted at `malloc`), `rss_delta_bytes` (resident set, which also sees thread
 *    stacks) and `threads_added`.
 *
 * Every timing row carries the payload sizes 64 B, 1 KiB, 4 KiB, 16 KiB and the arm's largest
 * payload (the IPv4 datagram bound, 65507 B, for a UDP link).
 *
 * @section shapes Link against session: what the streaming rows compare
 *
 * A libtracer arm drives a LINK: every `send` is one frame, one datagram, one syscall, with no
 * batching of its own (`shape=link-unbatched` on the `NOTE` lines). A Zenoh arm drives a
 * SESSION, whose streaming publisher batches small puts into one transport frame by default
 * (`shape=session-batched`). The two streaming rows therefore measure different shapes, and
 * the gap at 64 B is batching, not framing cost. The `stream-batch` row is the libtracer link
 * with the batching done the way an application does it (`compose_batch`); the composed-graph
 * comparison, which is what the methodology publishes, is `run_compose.sh`'s. Request/reply
 * rows use Zenoh's express publishers, which skip that batching, so the RTT rows are one
 * frame per send on both sides.
 *
 * @section heap Heap readings
 *
 * `heap_live_bytes` counts only blocks allocated inside the window and still live at its end;
 * a free of an older block costs the window nothing (`malloc_probe.cpp`, the #1420 rule), and
 * every binary runs the probe's own canary before any row.
 */
#pragma once

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "malloc_probe.hpp"

namespace bench::link {

/** @brief Payload offsets: `[0..7]` send timestamp, `[8..15]` sequence, `[16]` kind, `[17]`
 *         row epoch. Every size this harness sends is at least 64 B. */
inline constexpr std::size_t kSeqOffset = 8;
/** @brief Offset of the frame-kind byte. */
inline constexpr std::size_t kKindOffset = 16;
/** @brief Offset of the row-epoch byte, which fences one row's stragglers out of the next. */
inline constexpr std::size_t kEpochOffset = 17;
/** @brief Header bytes, the smallest frame the server end will read. */
inline constexpr std::size_t kHeader = 18;

/** @brief The IPv4 UDP payload bound: 65535 less the 20-byte IP and 8-byte UDP headers. */
inline constexpr std::size_t kDatagramBound = 65507;

/** @brief In the streaming row, 1 frame in this many is a stamped latency probe. */
inline constexpr std::size_t kProbeEvery = 64;

/** @brief What a frame is for, read by the server end off @ref kKindOffset. */
enum class kind_t : std::uint8_t {
    REQUEST = 1, /**< @brief Echo me (request/reply and setup rows). */
    BULK = 2,    /**< @brief Count me (streaming row). */
    PROBE = 3    /**< @brief Count me and record my one-way latency (streaming row). */
};

/** @brief Read a little-endian u64 at @p off. */
[[nodiscard]] inline std::uint64_t get_u64(std::span<const std::byte> f, std::size_t off) {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < 8; ++i)
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(f[off + i])) << (8 * i);
    return v;
}

/** @brief Write a little-endian u64 at @p off. */
inline void put_u64(std::span<std::byte> f, std::size_t off, std::uint64_t v) {
    for (std::size_t i = 0; i < 8; ++i) f[off + i] = static_cast<std::byte>((v >> (8 * i)) & 0xFF);
}

/**
 * @brief What both ends of a pair report into. The server-end members are written on the
 *        engine's receive thread; the harness reads them only after a row has drained.
 */
struct rx_state_t {
    std::atomic<std::uint64_t> reply_seq{~std::uint64_t{0}}; /**< @brief Last echoed sequence. */
    std::atomic<std::uint64_t> server_frames{0}; /**< @brief Frames of this epoch received. */
    std::atomic<bool> echo{true};                /**< @brief Server end echoes requests. */
    std::atomic<std::uint8_t> epoch{0};          /**< @brief The current row's epoch. */
    std::atomic<bool> record{false};             /**< @brief Server end records probes. */
    std::atomic<bool> batch{false}; /**< @brief Datagrams carry a K-value batch, not a frame. */
    std::atomic_flag lat_lock = ATOMIC_FLAG_INIT; /**< @brief Guards @ref lat (probes only). */
    Latency lat;                                  /**< @brief Probe one-way latencies. */

    /**
     * @brief The server end received @p f.
     * @return True when the pair must echo @p f back to the client end.
     */
    [[nodiscard]] bool on_server(std::span<const std::byte> f) {
        if (f.size() < kHeader) return false;
        const auto kind = static_cast<kind_t>(std::to_integer<std::uint8_t>(f[kKindOffset]));
        if (kind == kind_t::REQUEST) return echo.load(std::memory_order_relaxed);
        if (std::to_integer<std::uint8_t>(f[kEpochOffset]) != epoch.load(std::memory_order_relaxed))
            return false;
        server_frames.fetch_add(1, std::memory_order_relaxed);
        if (kind == kind_t::PROBE && record.load(std::memory_order_relaxed)) {
            const std::uint64_t ts = get_u64(f, 0);
            const std::uint64_t now = now_ns();
            while (lat_lock.test_and_set(std::memory_order_acquire)) {
            }
            if (now >= ts) lat.add(now - ts);
            lat_lock.clear(std::memory_order_release);
        }
        return false;
    }

    /** @brief The client end received the echo @p f. */
    void on_client(std::span<const std::byte> f) {
        if (f.size() >= kHeader) reply_seq.store(get_u64(f, kSeqOffset), std::memory_order_release);
    }
};

/**
 * @brief A client end and a server end joined by one link. The constructor wires both ends to
 *        @ref rx_state_t; the destructor tears both down.
 */
class link_pair_t {
   public:
    virtual ~link_pair_t() = default;
    /** @brief Send @p f from the client end to the server end. */
    virtual void send(std::span<const std::byte> f) = 0;
    /**
     * @brief Send @p f as a streaming-row frame. A pair whose engine has a distinct streaming
     *        path (Zenoh's batched publisher, against its express one for request/reply)
     *        overrides it; by default it is @ref send.
     */
    virtual void send_stream(std::span<const std::byte> f) { send(f); }
    /**
     * @brief The application clock seam: a link with no timers of its own (the Noise binding,
     *        #2064) is driven with `now` while the harness waits. Default: nothing to drive.
     */
    virtual void tick(std::uint64_t /*now*/) {}
    /**
     * @brief Prepare @p k values of @p value_bytes each for `stream-batch` rows and return their
     *        writable payloads, which the harness stamps before every @ref send_batch. Empty
     *        when this pair does not pack batches (the row is then skipped).
     */
    virtual std::vector<std::span<std::byte>> stage_batch(std::size_t /*k*/,
                                                          std::size_t /*value_bytes*/) {
        return {};
    }
    /** @brief Pack the staged values into one batch and send it as one frame. */
    virtual void send_batch() {}
};

/** @brief One arm: its row labels, its pair factory and its largest payload. */
struct arm_t {
    const char* system;      /**< @brief The `system` column (`libtracer`, `zenoh`). */
    const char* name;        /**< @brief The arm in the mode: `link-<name>-rr`. */
    std::size_t max_payload; /**< @brief The largest payload one frame may carry. */
    /** @brief What the streaming rows measure, printed on their `NOTE` lines: a libtracer
     *         link (unbatched) or a Zenoh session (batched by default). */
    const char* shape;
    /** @brief Build a fresh, wired pair; null if the link could not come up. */
    std::function<std::unique_ptr<link_pair_t>(rx_state_t&)> make;
};

/** @brief Run-wide knobs, from argv and the environment. */
struct options_t {
    double row_s = 0.5;      /**< @brief Time budget per timed row. */
    double stream_s = 0.5;   /**< @brief Blast window of a streaming row. */
    double setup_s = 2.0;    /**< @brief Time budget of the setup row. */
    std::string_view only{}; /**< @brief Run just this arm (`--arm=<name>`), empty = all. */
    bool list = false;       /**< @brief Print the compiled arms' names and exit (`--list`). */
};

/**
 * @brief Parse `--quick` and `--arm=<name>`; anything else refuses (#1040: an unknown argument
 *        must never fall through into a different run).
 * @return False on an unknown argument.
 */
[[nodiscard]] inline bool parse_options(int argc, char** argv, options_t& o) {
    if (const char* env = std::getenv("LIBTRACER_BENCH_SECONDS"); env != nullptr) {
        if (const double v = std::strtod(env, nullptr); v > 0.0) o.row_s = o.stream_s = v;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--quick") {
            o.row_s /= 5;
            o.stream_s /= 5;
            o.setup_s /= 5;
        } else if (a.starts_with("--arm=")) {
            o.only = a.substr(6);
        } else if (a == "--list") {
            o.list = true;
        } else {
            std::fprintf(stderr, "%s: unknown argument '%s' (--quick, --arm=<name>, --list)\n",
                         argv[0], argv[i]);
            return false;
        }
    }
    return true;
}

/** @brief Resident set size of this process in bytes, from `/proc/self/statm`. */
[[nodiscard]] inline long long rss_bytes() {
    std::FILE* f = std::fopen("/proc/self/statm", "r");
    if (f == nullptr) return 0;
    long long size = 0;
    long long resident = 0;
    const int got = std::fscanf(f, "%lld %lld", &size, &resident);
    std::fclose(f);
    return got == 2 ? resident * sysconf(_SC_PAGESIZE) : 0;
}

/** @brief Threads in this process, from `/proc/self/task`. */
[[nodiscard]] inline long long thread_count() {
    DIR* d = opendir("/proc/self/task");
    if (d == nullptr) return 0;
    long long n = 0;
    while (const dirent* e = readdir(d)) n += e->d_name[0] != '.' ? 1 : 0;
    closedir(d);
    return n;
}

/** @brief Stamp the harness header (send time, sequence, kind, epoch) into @p f. */
inline void stamp(std::span<std::byte> f, kind_t kind, std::uint64_t seq, std::uint8_t epoch) {
    put_u64(f, kSeqOffset, seq);
    f[kKindOffset] = static_cast<std::byte>(kind);
    f[kEpochOffset] = static_cast<std::byte>(epoch);
    put_u64(f, 0, now_ns());
}

/**
 * @brief Send request @p seq every 100 µs until its echo arrives or @p timeout_ns passes.
 * @return True when the echo arrived.
 */
[[nodiscard]] inline bool exchange(link_pair_t& p, rx_state_t& rx, std::vector<std::byte>& f,
                                   std::uint64_t seq, std::uint64_t timeout_ns) {
    const std::uint64_t t0 = now_ns();
    std::uint64_t next = t0;
    for (;;) {
        const std::uint64_t now = now_ns();
        if (rx.reply_seq.load(std::memory_order_acquire) == seq) return true;
        if (now - t0 > timeout_ns) return false;
        if (now >= next) {
            stamp(f, kind_t::REQUEST, seq, rx.epoch.load(std::memory_order_relaxed));
            p.send(f);
            next = now + 100'000;
        }
        p.tick(now);
    }
}

/** @brief The sizes every timing row runs at, for an arm whose largest payload is @p max. */
[[nodiscard]] inline std::vector<std::size_t> sizes_for(std::size_t max) {
    std::vector<std::size_t> s;
    for (const std::size_t v :
         {std::size_t{64}, std::size_t{1024}, std::size_t{4096}, std::size_t{16384}})
        if (v <= max) s.push_back(v);
    if (max > s.back()) s.push_back(max);
    return s;
}

/** @brief Build a mode label `link-<arm>-<row>` into @p buf. */
[[nodiscard]] inline const char* mode(char (&buf)[64], const arm_t& a, const char* row) {
    std::snprintf(buf, sizeof(buf), "link-%s-%s", a.name, row);
    return buf;
}

/** @brief The request/reply row at @p size on a warm pair. */
inline void rr_row(const arm_t& a, link_pair_t& p, rx_state_t& rx, std::size_t size,
                   const options_t& o, std::uint64_t& seq) {
    rx.echo.store(true, std::memory_order_relaxed);
    std::vector<std::byte> f(size, std::byte{0x5A});
    for (int i = 0; i < 32; ++i) (void)exchange(p, rx, f, ++seq, 50'000'000);  // warm-up
    Latency lat;
    lat.reserve(400000);
    std::size_t lost = 0;
    const std::uint64_t budget = static_cast<std::uint64_t>(o.row_s * 1e9);
    const std::uint64_t t0 = now_ns();
    std::size_t n = 0;
    while (n < 1000 || (n < 400000 && now_ns() - t0 < budget)) {
        ++seq;
        stamp(f, kind_t::REQUEST, seq, rx.epoch.load(std::memory_order_relaxed));
        const std::uint64_t s = get_u64(f, 0);
        p.send(f);
        bool got = false;
        for (;;) {
            const std::uint64_t now = now_ns();
            if (rx.reply_seq.load(std::memory_order_acquire) == seq) {
                lat.add(now - s);
                got = true;
                break;
            }
            if (now - s > 50'000'000) break;  // 50 ms: a lost request or reply
            p.tick(now);
        }
        lost += got ? 0 : 1;
        ++n;
        if (lost > 100) break;
    }
    const double secs = static_cast<double>(now_ns() - t0) / 1e9;
    const auto s = lat.summarize();
    const double rps = secs > 0 ? static_cast<double>(s.n) / secs : 0.0;
    char buf[64];
    emit(a.system, mode(buf, a, "rr"), size, 1, 1, rps, rps, rps * static_cast<double>(size) / 1e6,
         s);
    emit_tail(a.system, mode(buf, a, "rr"), size, 1, 1, s);
    std::printf("NOTE mode=%s system=%s size=%zu exchanges=%zu lost=%zu\n", buf, a.system, size, n,
                lost);
}

/** @brief Wait until the server end's count stops moving (100 ms quiet, 3 s at most). */
inline void drain(rx_state_t& rx, link_pair_t& p) {
    std::uint64_t last = rx.server_frames.load(std::memory_order_relaxed);
    std::uint64_t quiet_since = now_ns();
    const std::uint64_t t0 = quiet_since;
    while (now_ns() - quiet_since < 100'000'000 && now_ns() - t0 < 3'000'000'000ULL) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        p.tick(now_ns());
        const std::uint64_t c = rx.server_frames.load(std::memory_order_relaxed);
        if (c != last) {
            last = c;
            quiet_since = now_ns();
        }
    }
}

/**
 * @brief The streaming row at @p size on a warm pair: one frame per send when @p k is 0
 *        (`stream`), else @p k values of @p size packed into one batch per send
 *        (`stream-batch`, counted per value). A pair that stages no batch skips the row.
 */
inline void stream_row(const arm_t& a, link_pair_t& p, rx_state_t& rx, std::size_t size,
                       const options_t& o, std::size_t k = 0) {
    const std::vector<std::span<std::byte>> staged =
        k > 0 ? p.stage_batch(k, size) : std::vector<std::span<std::byte>>{};
    if (k > 0 && staged.size() != k) return;
    rx.batch.store(k > 0);
    rx.echo.store(false, std::memory_order_relaxed);
    const auto epoch = static_cast<std::uint8_t>(rx.epoch.load() + 1);
    rx.epoch.store(epoch);
    rx.server_frames.store(0);
    {
        while (rx.lat_lock.test_and_set(std::memory_order_acquire)) {
        }
        rx.lat = Latency{};
        rx.lat.reserve(1u << 20);
        rx.lat_lock.clear(std::memory_order_release);
    }
    rx.record.store(true);
    std::vector<std::byte> f(size, std::byte{0x5A});
    const std::uint64_t window = static_cast<std::uint64_t>(o.stream_s * 1e9);
    std::uint64_t sent = 0;
    const std::uint64_t t0 = now_ns();
    std::uint64_t now = t0;
    std::uint64_t sends = 0;
    while (now - t0 < window) {
        if (k == 0) {
            stamp(f, sent % kProbeEvery == 0 ? kind_t::PROBE : kind_t::BULK, sent, epoch);
            p.send_stream(f);
            ++sent;
        } else {
            for (const std::span<std::byte> v : staged) {
                stamp(v, sent % kProbeEvery == 0 ? kind_t::PROBE : kind_t::BULK, sent, epoch);
                ++sent;
            }
            p.send_batch();
        }
        if ((++sends & 63) == 0) p.tick(now);
        now = now_ns();
    }
    // Read the count WITH the clock, so a frame drained after the window is not credited to it
    // (the bias bench_tcp_baseline documents); the drained total is the loss figure's.
    const std::uint64_t in_window = rx.server_frames.load(std::memory_order_relaxed);
    const double secs = static_cast<double>(now - t0) / 1e9;
    drain(rx, p);
    rx.record.store(false);
    rx.batch.store(false);
    const std::uint64_t total = rx.server_frames.load(std::memory_order_relaxed);
    while (rx.lat_lock.test_and_set(std::memory_order_acquire)) {
    }
    const auto s = rx.lat.summarize();
    rx.lat_lock.clear(std::memory_order_release);
    const double sps = static_cast<double>(sent) / secs;
    const double dps = static_cast<double>(in_window) / secs;
    char buf[64];
    const char* row = k > 0 ? "stream-batch" : "stream";
    const int fan = k > 0 ? static_cast<int>(k) : 1;
    emit(a.system, mode(buf, a, row), size, fan, 1, sps, dps, dps * static_cast<double>(size) / 1e6,
         s);
    emit_tail(a.system, mode(buf, a, row), size, fan, 1, s);
    std::printf(
        "NOTE mode=%s system=%s size=%zu values_per_send=%zu shape=%s sent=%llu delivered=%llu "
        "loss=%.4f\n",
        buf, a.system, size, k > 0 ? k : std::size_t{1}, k > 0 ? "link-batched" : a.shape,
        static_cast<unsigned long long>(sent), static_cast<unsigned long long>(total),
        sent > 0 ? 1.0 - static_cast<double>(total) / static_cast<double>(sent) : 0.0);
    std::fflush(stdout);
    rx.epoch.store(static_cast<std::uint8_t>(epoch + 1));
}

/** @brief The setup row: fresh pair to first reply, repeated within the setup budget. */
inline void setup_row(const arm_t& a, const options_t& o) {
    Latency lat;
    lat.reserve(4096);
    std::size_t failed = 0;
    const std::uint64_t budget = static_cast<std::uint64_t>(o.setup_s * 1e9);
    const std::uint64_t t0 = now_ns();
    std::vector<std::byte> f(64, std::byte{0x5A});
    for (std::size_t n = 0; n < 2000 && (n < 10 || now_ns() - t0 < budget); ++n) {
        rx_state_t rx;
        const std::uint64_t s = now_ns();
        std::unique_ptr<link_pair_t> p = a.make(rx);
        const bool ok = p != nullptr && exchange(*p, rx, f, 1, 5'000'000'000ULL);
        const std::uint64_t e = now_ns();
        p.reset();  // teardown is not setup: off the clock
        if (ok)
            lat.add(e - s);
        else
            ++failed;
    }
    const auto s = lat.summarize();
    char buf[64];
    const double ps = s.mean > 0 ? 1e9 / static_cast<double>(s.mean) : 0.0;
    emit(a.system, mode(buf, a, "setup"), 64, 1, 1, ps, ps, 0.0, s);
    emit_tail(a.system, mode(buf, a, "setup"), 64, 1, 1, s);
    std::printf("NOTE mode=%s system=%s setups=%zu failed=%zu\n", buf, a.system, s.n, failed);
}

/** @brief Emit one `LINK_RAM` row. */
inline void emit_ram(const arm_t& a, const char* metric, long long v) {
    std::printf("LINK_RAM\t%s\t%s\t%s\t%lld\n", a.system, a.name, metric, v);
    std::fflush(stdout);
}

/**
 * @brief Run one arm: its RAM reading around the first pair, the request/reply and streaming
 *        rows on that pair, then the setup row.
 * @return False if the arm's link never came up (no rows were emitted for it).
 */
inline bool run_arm(const arm_t& a, const options_t& o) {
    if (o.list) {
        std::printf("%s\n", a.name);
        return true;
    }
    if (!o.only.empty() && o.only != a.name) return true;
    if (malloc_probe::kAvailable && !malloc_probe::canary()) {
        std::fprintf(stderr, "FATAL arm %s/%s: malloc probe canary failed (#1420)\n", a.system,
                     a.name);
        std::exit(1);
    }
    rx_state_t rx;
    std::vector<std::byte> f(64, std::byte{0x5A});
    const long long rss0 = rss_bytes();
    const long long thr0 = thread_count();
    malloc_probe::arm();
    std::unique_ptr<link_pair_t> p = a.make(rx);
    const bool up = p != nullptr && exchange(*p, rx, f, 1, 5'000'000'000ULL);
    const malloc_probe::reading_t heap = malloc_probe::disarm();
    if (!up) {
        std::printf("# arm %s/%s: link did not come up; no rows\n", a.system, a.name);
        return false;
    }
    emit_ram(a, "heap_live_bytes", heap.live);
    if (heap.untracked != 0) emit_ram(a, "heap_untracked_blocks", heap.untracked);
    emit_ram(a, "rss_delta_bytes", rss_bytes() - rss0);
    emit_ram(a, "threads_added", thread_count() - thr0);
    std::uint64_t seq = 1;
    for (const std::size_t size : sizes_for(a.max_payload)) rr_row(a, *p, rx, size, o, seq);
    for (const std::size_t size : sizes_for(a.max_payload)) stream_row(a, *p, rx, size, o);
    for (const std::size_t v : {std::size_t{64}, std::size_t{1024}, std::size_t{4096}})
        if (const std::size_t k = std::min<std::size_t>(16, (a.max_payload - 32) / (v + 4)); k > 1)
            stream_row(a, *p, rx, v, o, k);
    p.reset();
    setup_row(a, o);
    return true;
}

}  // namespace bench::link
