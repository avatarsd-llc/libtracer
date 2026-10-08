/**
 * @file
 * @brief Zenoh side of the comparison: in-process (peer) pub/sub via zenoh-cpp over zenoh-c.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Sweeps the same matrix as bench_libtracer — fan-out (F subscribers on
 * one key expression), payload size, and endpoint count (E key expressions) —
 * and emits the same RESULT line. Intra-session local delivery is the closest
 * Zenoh analogue to libtracer's in-process path. See bench/README.md.
 *
 * @section matched Matched semantics (#1809)
 *
 * Each libtracer row of the compared families has a Zenoh row under the same key, measured the
 * same way:
 *   - **One process per family.** The default sweep runs each family below in a fresh child
 *     process under the pinned allocator tunables (bench_process.hpp), exactly as
 *     bench_libtracer's families run, and each family opens its own `Session`. No row inherits
 *     a heap or a session another family aged.
 *   - **The same pin, runtime threads accounted for.** Every compared family, on both sides,
 *     narrows itself to one logical CPU, the lowest of the set `bench_conditions.py` gives it
 *     (#1906), and Zenoh's runtime threads share it with the publishing thread.
 *     A `NOTE zenoh-runtime` line after each row gives those threads' CPU time over the
 *     throughput window: 0 ns on the in-process rows, so delivery there runs on the putting
 *     thread, as libtracer's does. Any share of the pin the runtime takes is on the record
 *     rather than hidden in the row.
 *   - **Equal payload bytes.** Each put carries @ref bench::value_wire_bytes bytes: the bytes
 *     libtracer moves for the row's value, header included. Rows stay keyed by the value size.
 *   - **The producer's build on the same side of the clock.** Below
 *     @ref bench::kProducerUntimedFrom each timed put builds its `Bytes` (an allocation and a
 *     payload copy), as each libtracer `inproc` write builds its owned value; from 1 KiB both
 *     engines build their values before the clock starts, free them after it stops, and time
 *     the put alone (#1905).
 *   - **Resolution against resolution.** `inproc-path` puts through `Session::put` on a
 *     pre-built `KeyExpr`, resolved on every put, as libtracer's `inproc-path` writes by a
 *     pre-parsed path. The bound spelling is the `topics-bound` row.
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <unistd.h>
#endif

#include "bench_common.hpp"
#include "bench_process.hpp"
#include "zenoh.hxx"

using namespace zenoh;
using namespace bench;

namespace {

/**
 * @brief How a point spells its destination — the #1485 addendum-C axis.
 *
 * The two spellings are the SAME semantic operation reaching the SAME subscribers; what differs
 * is whether the destination was resolved once at setup or is resolved inside every iteration.
 * Keeping them apart is the whole point: the pre-existing comparison put libtracer's
 * resolve-per-write row against Zenoh's declared-publisher row, so a resolution term sat inside
 * one arm and nowhere in the other.
 */
enum class addr_t {
    BOUND, /**< @brief A declared `Publisher` — Zenoh's pre-bound handle. */
    ADDR   /**< @brief `Session::put` against a pre-built `KeyExpr`, resolved on every put. */
};

/** @brief CPU time the process's threads other than the caller have used, and how many there are.
 */
struct runtime_cpu_t {
    std::uint64_t ns = 0;    /**< @brief Summed on-CPU time of every other thread, in ns. */
    std::size_t threads = 0; /**< @brief How many other threads were counted. */
};

/**
 * @brief Read the on-CPU time of every thread but the caller's: Zenoh's runtime threads.
 *
 * From `/proc/self/task/<tid>/schedstat` (first field, nanoseconds), which unlike `stat`'s
 * utime/stime is not quantized to the scheduler tick. Zero threads where it cannot be read.
 */
runtime_cpu_t runtime_cpu() {
    runtime_cpu_t r;
#if defined(__linux__)
    DIR* const dir = opendir("/proc/self/task");
    if (dir == nullptr) return r;
    const std::string self = std::to_string(gettid());
    while (const dirent* const e = readdir(dir)) {
        if (e->d_name[0] == '.' || self == e->d_name) continue;
        const std::string path = std::string("/proc/self/task/") + e->d_name + "/schedstat";
        std::FILE* const f = std::fopen(path.c_str(), "r");
        if (f == nullptr) continue;  // the thread exited between readdir and open
        unsigned long long ns = 0;
        if (std::fscanf(f, "%llu", &ns) == 1) {
            r.ns += ns;
            ++r.threads;
        }
        std::fclose(f);
    }
    closedir(dir);
#endif
    return r;
}

void run(Session& session, std::size_t S, std::size_t F, std::size_t E, const char* mode,
         std::uint64_t budget = kDeliveryBudget, std::uint64_t latbudget = kLatencyDeliveryBudget,
         addr_t addr = addr_t::BOUND) {
    std::atomic<std::uint64_t> recv{0};
    std::vector<Subscriber<void>> subs;
    std::vector<Publisher> pubs;
    std::vector<KeyExpr> kes;
    subs.reserve(F * E);
    pubs.reserve(E);
    kes.reserve(E);
    for (std::size_t e = 0; e < E; ++e) {
        const std::string ke = "bench/zenoh/" + std::to_string(e);
        for (std::size_t f = 0; f < F; ++f) {
            subs.push_back(session.declare_subscriber(
                KeyExpr(ke), [&](const Sample&) { recv.fetch_add(1, std::memory_order_relaxed); },
                closures::none));
        }
        if (addr == addr_t::BOUND)
            pubs.push_back(session.declare_publisher(KeyExpr(ke)));
        else
            kes.push_back(KeyExpr(ke));
    }
    // The bytes libtracer moves for an S-byte value, header included (#1809).
    const std::vector<std::uint8_t> payload(value_wire_bytes(S), 0xAB);
    // The ONE line the two spellings differ by. The `KeyExpr` objects are pre-built, exactly as
    // libtracer's `topics-addr` arm pre-parses its `path_t`s: what is being compared is
    // per-operation RESOLUTION, not per-operation string parsing, and charging one engine for a
    // parse the other hoisted is how the previous comparison went wrong in the first place.
    const auto publish = [&](std::size_t i) {
        if (addr == addr_t::BOUND)
            pubs[i % E].put(Bytes(payload));
        else
            session.put(kes[i % E], Bytes(payload));
    };
    // From 1 KiB the producer's `Bytes` (an allocation and a copy of the payload) is built
    // before the clock starts and freed after it stops, as bench_libtracer's owned values are
    // (#1905).
    const bool staged = S >= kProducerUntimedFrom;
    const auto put_built = [&](std::size_t i, Bytes&& b) {
        if (addr == addr_t::BOUND)
            pubs[i % E].put(std::move(b));
        else
            session.put(kes[i % E], std::move(b));
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let pub<->sub match

    const std::size_t MSGS = publishes_for(F, budget);
    const std::uint64_t want = static_cast<std::uint64_t>(MSGS) * F;

    // Equal warmup with bench_libtracer (1000 puts), drained before the counter reset
    // so a leftover warmup delivery never counts toward the timed phase's `want`.
    for (std::size_t i = 0; i < 1000; ++i) publish(i);
    const auto wd = Clock::now() + std::chrono::seconds(5);
    while (recv.load(std::memory_order_relaxed) < 1000ull * F && Clock::now() < wd)
        std::this_thread::yield();

    recv.store(0);
    const runtime_cpu_t rt0 = runtime_cpu();
    std::uint64_t wall_ns = 0;
    if (staged) {
        // The window is the sum of the put runs plus the final drain. The producer's `Bytes` are
        // built once, `kStagedBuffers` of them, and each put takes a shallow clone, staged between
        // runs: no allocation, copy or free of a payload is inside a run, as in bench_libtracer's
        // staged rows (#1905).
        std::vector<Bytes> bufs;
        for (std::size_t b = 0; b < kStagedBuffers; ++b) bufs.emplace_back(payload);
        std::vector<Bytes> built;
        built.reserve(kStagedChunk);
        for (std::size_t i = 0; i < MSGS;) {
            const std::size_t n = std::min(kStagedChunk, MSGS - i);
            built.clear();
            for (std::size_t k = 0; k < n; ++k)
                built.push_back(bufs[(i + k) % kStagedBuffers].clone());
            const auto a = now_ns();
            for (std::size_t k = 0; k < n; ++k, ++i) put_built(i, std::move(built[k]));
            wall_ns += now_ns() - a;
        }
        const auto a = now_ns();
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        while (recv.load(std::memory_order_relaxed) < want && Clock::now() < deadline)
            std::this_thread::yield();
        wall_ns += now_ns() - a;
    } else {
        const auto t0 = now_ns();
        for (std::size_t i = 0; i < MSGS; ++i) publish(i);
        const auto deadline = Clock::now() + std::chrono::seconds(30);
        while (recv.load(std::memory_order_relaxed) < want && Clock::now() < deadline)
            std::this_thread::yield();
        wall_ns = now_ns() - t0;
    }
    const runtime_cpu_t rt1 = runtime_cpu();
    const double secs = wall_ns / 1e9;
    const std::uint64_t got = recv.load(std::memory_order_relaxed);
    if (got < want)
        std::fprintf(stderr, "[zenoh] S=%zu F=%zu E=%zu delivered %llu/%llu (best-effort drops)\n",
                     S, F, E, static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));

    Latency lat;
    const std::size_t LATN = publishes_for(F, latbudget);
    // Symmetric bracketing with bench_libtracer: the timed window is put + spin-on-recv
    // ONLY. The escape hatch for a dropped sample is an iteration cap fixed OUTSIDE the
    // bracket — the old per-sample `Clock::now() + 500ms` deadline put a time_point
    // construction and per-spin clock reads inside the start..stop window, inflating
    // every Zenoh sample by ~tens of ns.
    constexpr std::uint64_t kSpinCap = 5'000'000;  // yields (~1s) before giving up a sample
    for (std::size_t i = 0; i < LATN; ++i) {
        const std::uint64_t want_i = recv.load(std::memory_order_relaxed) + F;
        if (staged) {
            Bytes b(payload);              // the producer's build, before the clock starts
            const Bytes keep = b.clone();  // and its free, after the clock stops
            const auto start = now_ns();
            put_built(i, std::move(b));
            for (std::uint64_t spins = 0;
                 recv.load(std::memory_order_relaxed) < want_i && spins < kSpinCap; ++spins)
                std::this_thread::yield();
            lat.add(now_ns() - start);
            continue;
        }
        const auto start = now_ns();
        publish(i);
        for (std::uint64_t spins = 0;
             recv.load(std::memory_order_relaxed) < want_i && spins < kSpinCap; ++spins)
            std::this_thread::yield();
        lat.add(now_ns() - start);
    }
    const double pub_s = MSGS / secs;
    const double deliv_s = got / secs;
    emit("zenoh", mode, S, F, E, pub_s, deliv_s, deliv_s * static_cast<double>(S) / 1e6,
         lat.summarize());
    // The runtime threads' share of the throughput window, on the pin both engines share. A
    // thread that started or exited inside the window is counted from what was read, which is
    // why the line carries both thread counts.
    const std::uint64_t rt_ns = rt1.ns > rt0.ns ? rt1.ns - rt0.ns : 0;
    std::printf(
        "NOTE zenoh-runtime mode=%s size=%zu fan=%zu ep=%zu wire_bytes=%zu threads=%zu/%zu "
        "runtime_cpu_ns=%llu window_ns=%llu runtime_cpu_pct=%.1f\n",
        mode, S, F, E, value_wire_bytes(S), rt0.threads, rt1.threads,
        static_cast<unsigned long long>(rt_ns), static_cast<unsigned long long>(wall_ns),
        wall_ns > 0 ? 100.0 * static_cast<double>(rt_ns) / static_cast<double>(wall_ns) : 0.0);
    std::fflush(stdout);
}

/**
 * @brief Response-surface grid matching bench_libtracer's grid: size x fanout (mode `inproc`) and
 *        size x endpoints (mode `inproc-path`).
 *
 * Emits the same mode-tagged
 * RESULT line as the default run so one parser feeds the docs comparison charts.
 */
void run_grid(Session& session) {
    for (std::size_t S : kGridSizes)
        for (std::size_t F : kGridFanouts)
            run(session, S, F, 1, "inproc", kGridBudget, kGridLatBudget);
    // Resolved on every put, as bench_libtracer's grid writes by path (#1910): the grid's
    // `inproc-path` rows used to put through a declared `Publisher`, the bound spelling.
    for (std::size_t S : kGridSizes)
        for (std::size_t E : kGridEndpoints)
            run(session, S, 1, E, "inproc-path", kGridBudget, kGridLatBudget, addr_t::ADDR);
    // The dense payload sweep (#1890), at fan-out 1 only: the payload charts' slice.
    for (std::size_t S : sweep_extra(kGridSizes))
        run(session, S, 1, 1, "inproc", ladder_budget(S, kGridBudget),
            ladder_budget(S, kGridLatBudget));
}

/**
 * @brief #1485 addendum C — the TOPIC-COUNT arm, in both address spellings.
 *
 * @p bound_first flips which spelling runs first at each topic count. Arm order is not free on a
 * shared host: an always-same-first ordering manufactured an apparent 12.55-vs-8.07 M/s win here
 * that vanished on the flip. `run_topics.sh` executes both orders and reduces with
 * `best_of_rounds.py`, so a point whose two orders disagree on the sign of a trend is visible as
 * unresolved rather than published as a verdict.
 */
void run_topics(Session& session, bool bound_first) {
    for (const std::size_t E : kTopicLadder) {
        const auto bound = [&] {
            run(session, kRefSize, kRefFanout, E, "topics-bound", kDeliveryBudget,
                kLatencyDeliveryBudget, addr_t::BOUND);
        };
        const auto by_addr = [&] {
            run(session, kRefSize, kRefFanout, E, "topics-addr", kDeliveryBudget,
                kLatencyDeliveryBudget, addr_t::ADDR);
        };
        if (bound_first) {
            bound();
            by_addr();
        } else {
            by_addr();
            bound();
        }
    }
}

/*
 * `run_scatter` was DELETED, not fixed. It declared a publisher with no subscriber and no
 * peer, so `put()` never reached the wire — measured with strace, 5 `sendto` for 520 000
 * puts, and those five were multicast scouting beacons. It then emitted ONE K-independent
 * put rate for every K in {1,8,64,256}, so its "curve" was arithmetic. Charted against a
 * libtracer side that published `sendmsg_rate * K` (egress-only, no receiver), it produced
 * a published multi-x win on a page that described the scenario as "loopback UDP, two
 * processes" — one process, and no network on the Zenoh side at all.
 *
 * A valid composition comparison needs a real subscriber in a SECOND PROCESS on both sides
 * with deliveries counted at the receiver. That is a new benchmark; it is tracked as an
 * issue rather than left here as something to "fix".
 */

/*
 * The default sweep, as FAMILIES (#1809), named and ordered as bench_libtracer's families of
 * the same rows, so each RESULT key has one libtracer and one Zenoh row from the same process
 * shape. bench_libtracer runs other families between these; their rows have no Zenoh match.
 */

/** @brief `inproc` fan-out sweep at the reference payload. */
void family_inproc_fan(Session& session) {
    for (std::size_t F : kFanouts) run(session, kRefSize, F, kRefEndpoints, "inproc");
}

/** @brief `inproc` payload sweep at the reference fan-out, then the payload ladder (#1806) and
 *         the dense comparison sweep (#1890). */
void family_inproc_size(Session& session) {
    for (std::size_t S : kSizes) run(session, S, kRefFanout, kRefEndpoints, "inproc");
    for (std::size_t S : ladder_extra())
        run(session, S, kRefFanout, kRefEndpoints, "inproc", ladder_budget(S, kDeliveryBudget),
            ladder_budget(S, kLatencyDeliveryBudget));
    // The dense payload sweep (#1890), after every existing row.
    for (std::size_t S : sweep_extra(kSizes, kPayloadLadder))
        run(session, S, kRefFanout, kRefEndpoints, "inproc", ladder_budget(S, kDeliveryBudget),
            ladder_budget(S, kLatencyDeliveryBudget));
}

/** @brief `inproc-path` topic-count sweep, resolved on every put like libtracer's by-path write. */
void family_inproc_path(Session& session) {
    for (std::size_t E : kEndpoints)
        run(session, kRefSize, kRefFanout, E, "inproc-path", kDeliveryBudget,
            kLatencyDeliveryBudget, addr_t::ADDR);
}

/** @brief The mid fan-out arms (#844), on the same `inproc` series as the coarse ladder. */
void family_inproc_fan_mid(Session& session) {
    for (std::size_t F : kFanoutsMid) run(session, kRefSize, F, kRefEndpoints, "inproc");
}

/** @brief The `topics` pair, bound arm first, as bench_libtracer's `topics` family. */
void family_topics(Session& session) { run_topics(session, true); }

/** @brief One family of the default sweep: its `--family` spelling and its runner. */
struct zenoh_family_t {
    std::string_view name; /**< @brief What `--family` must equal to select this family. */
    void (*run)(Session&); /**< @brief The family's rows, on a session of its own. */
};

constexpr zenoh_family_t kFamilies[] = {
    {"inproc-fan", family_inproc_fan},   {"inproc-size", family_inproc_size},
    {"inproc-path", family_inproc_path}, {"inproc-fan-mid", family_inproc_fan_mid},
    {"topics", family_topics},
};

/**
 * @brief Open an in-process session.
 *
 * Multicast scouting OFF, as `bench_zenoh_net` already does. This is an IN-PROCESS comparison:
 * libtracer runs no discovery subsystem at all, so leaving Zenoh's on puts a background thread
 * and real multicast traffic inside the timed window on one side only. Measured on the default
 * config: 23 sendto + 35 receives across one `grid` run. Small, but it is a fairness asymmetry
 * in a chart whose whole premise is like-for-like.
 */
Session open_session() {
    Config cfg = Config::create_default();
    cfg.insert_json5("scouting/multicast/enabled", "false");
    return Session::open(std::move(cfg));
}

}  // namespace

int main(int argc, char** argv) {
    // Fixed allocator state, as bench_libtracer (#1803): re-exec before any output, and every
    // family child inherits it.
    pin_allocator_state(argv);
    init_log_from_env_or("error");
    const std::string_view arg = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
    if (argc > 2 && arg == "--family") {
        for (const zenoh_family_t& f : kFamilies) {
            if (f.name != argv[2]) continue;
            // One logical CPU, as bench_libtracer's single-threaded families (#1906): the
            // compared rows keep the same pin, and Zenoh's runtime threads share it.
            pin_to_one_cpu();
            std::fprintf(stderr, "FAMILY %.*s\n", static_cast<int>(f.name.size()), f.name.data());
            emit_alloc_state();  // the settings this family's rows run under (#1903)
            const std::size_t start_kb = rss_kb();
            {
                auto session = open_session();
                f.run(session);
            }
            // Prefixed: perf_emit_benchmark.py keys RSS by family name across every raw it
            // is given, and bench_libtracer's family of the same name must not merge with it.
            emit_family_rss("zenoh-" + std::string(f.name), start_kb);
            return 0;
        }
        std::fprintf(stderr, "error: unknown family '%s'\n", argv[2]);
        return 2;
    }
    if (argc == 2 && arg == "--families") {
        for (const zenoh_family_t& f : kFamilies)
            std::printf("%.*s\tsingle\n", static_cast<int>(f.name.size()), f.name.data());
        return 0;
    }
    if (arg == "grid" || arg == "topics" || arg == "topics-rev") {
        // One logical CPU before the session starts its runtime threads, so they inherit it:
        // bench_libtracer pins its `grid` and `topics` modes the same way (#1910).
        pin_to_one_cpu();
        emit_clock_floor();
        emit_alloc_state();
        auto session = open_session();
        if (arg == "grid")
            run_grid(session);
        else
            run_topics(session, arg == "topics");
        return 0;
    }
    if (arg == "scatter") {
        std::fprintf(stderr,
                     "bench_zenoh: the `scatter` mode was removed — it measured no network "
                     "I/O (see the note above run_grid). Emitting nothing is deliberate.\n");
        return 0;
    }
    if (argc > 1) {
        std::fprintf(stderr,
                     "error: unknown mode '%s' (grid | topics | topics-rev | --family NAME | "
                     "--families)\n",
                     argv[1]);
        return 2;
    }
    // The default sweep: one fresh process per family, the clock floor once, ahead of them.
    emit_clock_floor();
    emit_alloc_state();
    for (const zenoh_family_t& f : kFamilies) {
        if constexpr (!kFamilyProcesses) {
            auto session = open_session();
            f.run(session);
            continue;
        }
        const int rc = run_family_process(argv[0], f.name);
        if (rc != 0) {
            std::fprintf(stderr, "error: family '%.*s' failed (status %d)\n",
                         static_cast<int>(f.name.size()), f.name.data(), rc);
            return 1;
        }
    }
    return 0;
}
