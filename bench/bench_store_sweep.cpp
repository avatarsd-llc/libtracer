/**
 * @file
 * @brief The ADR-0079 PER-CONFIGURATION store sweep: H-baseline / WIDE / MID / NARROW over one
 *        workload, reporting alloc hot-path latency, fan-out throughput across thread counts,
 *        and store high-water — the matrix ADR-0079 §Verification commissions (#941, #873).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The node under test — its four arms, the four allocation channels each arm wires, the equal
 * slab budget, and the ONE thing this sweep cannot vary (`vertex_t` placement, ADR-0079
 * Stage 2 / #843, still on the global heap) — is `bench/store_sweep_node.hpp`. Read that file
 * first; nothing here can be interpreted without it.
 *
 * @section modes Four modes, because the matrix has three columns and one guard
 *
 *  * `latency`    — single-threaded per-op cost, in PICOseconds, split into the workload's three
 *                   legs plus the composite. Arms rotate per round (`--round0`).
 *  * `throughput` — the fan-out arm across `kThreads = {1,2,4,8,16,24}`: T lanes, T receive
 *                   threads, the whole workload each. This is the arm ADR-0079 predicts WIDE's
 *                   shared lock to collapse on.
 *  * `hwm`        — per-store occupancy and per-channel draw counts. Run SINGLE-THREADED over
 *                   all T lanes on purpose: concurrent draws overlap, which would make the
 *                   high-water a function of the scheduler. Taken this way it is a function of
 *                   the workload and the topology only, so it is the trustworthy memory column.
 *  * `calibrate`  — the non-vacuity gate. Exits non-zero. See @ref run_calibrate.
 *
 * @section why_no_override Why the global `operator new` override is NOT in this TU
 *
 * It would cost a relaxed atomic load on every allocation inside the timed arms, on arms that
 * allocate at different rates — so the instrument would bias the comparison it exists to make.
 * The process-heap ESCAPE census lives in its own binary, `bench_store_escape.cpp`, which is
 * untimed and can afford it.
 *
 * @section estimator Estimator and window separation
 *
 * Best-of-rounds, never median (`docs/methodology.md`) — enforced in
 * `bench/collate_store_sweep.py`, together with the A/A null band that
 * `bench/run_store_sweep.sh` collects in the same window. The `latency` and `hwm` modes run
 * under `taskset -c <cpu>`; `throughput` must NOT be pinned, because a 24-thread arm on one
 * logical CPU measures nothing. The two windows therefore carry DIFFERENT RESULT tags and the
 * collator keeps them in separate tables.
 *
 * @section tags Output
 *
 *     RESULT_STORE_LAT   round tag arm leg p50ps p99ps meanps n batch
 *     RESULT_STORE_TPUT  round tag arm leg threads per_thread_ops_s agg_ops_s ns_per_op
 *     RESULT_STORE_HWM   arm threads store idx used capacity classes overflow
 *     RESULT_STORE_CHAN  arm threads channel blocks bytes peak_live refusals
 *
 * Own tags, never `RESULT`: every parser in the tree gates on the `RESULT` line's exact
 * 12-field arity (`bench/bench_common.hpp`), so a differently-shaped row under that tag would
 * make them match ZERO rows rather than fail loudly.
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "store_sweep_node.hpp"

namespace {

using bench_store::arm_t;
using bench_store::counting_source_t;
using bench_store::kArms;
using bench_store::kThreads;
using bench_store::name_of;
using bench_store::node_t;

/** @brief Picoseconds per nanosecond — the latency arm accumulates in ps. */
constexpr std::uint64_t kPsPerNs = 1000;

/** @brief Timed samples per latency cell; each sample is one calibrated batch. */
constexpr std::size_t kSamplesPerCell = 256;

/** @brief Wall-clock per throughput point. */
constexpr auto kWindow = std::chrono::milliseconds(200);

/** @brief Workload iterations per lane in the `hwm` mode — enough to reach steady state. */
constexpr std::size_t kHwmOpsPerLane = 64;

/** @brief The legs of the workload, timed separately so one channel cannot hide another. */
enum class leg_t {
    NET,   /**< @brief The rope FWD forward hop: the router `rx` draw plus the egress gather. */
    WRITE, /**< @brief One graph write: the `std::pmr` `mr` draw. */
    READ,  /**< @brief One composed subtree read: the graph `ctl` draw. */
    FULL,  /**< @brief All three, which is what the throughput arm runs. */
};

/** @brief Every leg, in the order the node runs them. */
constexpr leg_t kLegs[] = {leg_t::NET, leg_t::WRITE, leg_t::READ, leg_t::FULL};

/** @brief The leg's stable label — the key the collated latency table joins on. */
[[nodiscard]] const char* name_of(leg_t l) noexcept {
    switch (l) {
        case leg_t::NET:
            return "net-fwd";
        case leg_t::WRITE:
            return "graph-write";
        case leg_t::READ:
            return "graph-read";
        case leg_t::FULL:
            return "full";
    }
    return "?";
}

/** @brief Run one leg once on lane @p l of @p n. */
void step(node_t& n, bench_store::lane_t& l, leg_t leg) {
    switch (leg) {
        case leg_t::NET:
            n.step_net(l);
            return;
        case leg_t::WRITE:
            n.step_write(l);
            return;
        case leg_t::READ:
            n.step_read(l);
            return;
        case leg_t::FULL:
            n.step_full(l);
            return;
    }
}

/** @brief `RESULT_STORE_LAT` — one arm's one leg in one round. */
void emit_lat(int round, const char* tag, const char* arm, const char* leg,
              const bench::Latency::Summary& s, std::size_t batch) {
    std::printf("RESULT_STORE_LAT\t%d\t%s\t%s\t%s\t%llu\t%llu\t%llu\t%zu\t%zu\n", round, tag, arm,
                leg, static_cast<unsigned long long>(s.p50), static_cast<unsigned long long>(s.p99),
                static_cast<unsigned long long>(s.mean), s.n, batch);
    std::fflush(stdout);
}

/** @brief `RESULT_STORE_TPUT` — one arm's one leg at one thread count in one round. */
void emit_tput(int round, const char* tag, const char* arm, const char* leg, std::size_t threads,
               double per_thread, double aggregate, std::uint64_t ns_per_op) {
    std::printf("RESULT_STORE_TPUT\t%d\t%s\t%s\t%s\t%zu\t%.0f\t%.0f\t%llu\n", round, tag, arm, leg,
                threads, per_thread, aggregate, static_cast<unsigned long long>(ns_per_op));
    std::fflush(stdout);
}

/** @brief `RESULT_STORE_HWM` — one store's occupancy after the deterministic workload. */
void emit_hwm(const char* arm, std::size_t threads, const char* store, std::size_t idx,
              const bench_store::stores_t::pool_view_t& p) {
    std::printf("RESULT_STORE_HWM\t%s\t%zu\t%s\t%zu\t%zu\t%zu\t%zu\t%zu\n", arm, threads, store,
                idx, p.used, p.capacity, p.classes, p.overflow);
    std::fflush(stdout);
}

/** @brief `RESULT_STORE_CHAN` — what one allocation channel drew, per configuration. */
void emit_chan(const char* arm, std::size_t threads, const char* channel,
               const counting_source_t& c) {
    std::printf("RESULT_STORE_CHAN\t%s\t%zu\t%s\t%zu\t%zu\t%lld\t%zu\n", arm, threads, channel,
                c.blocks.load(), c.bytes_served.load(), c.peak.load(), c.refusals.load());
    std::fflush(stdout);
}

/** @brief Time every leg of one arm, single-threaded, and emit its four rows. */
[[nodiscard]] bool run_latency_arm(int round, const char* tag, arm_t arm) {
    node_t n(arm, /*lanes=*/1, /*count_channels=*/false);
    bench_store::lane_t& l = n.lane(0);

    for (const leg_t leg : kLegs) {
        // The batch is sized by WINDOW rather than by plateau: the plateau rule compares two
        // TIMED quantities, so the machine gets a vote in which batch is latched, and
        // bench_common.hpp records same-binary A/A differences of up to ~8 % from that lottery
        // alone — larger than the effect this sweep is looking for.
        const std::size_t batch = bench::calibrate_batch_for_window([&] { step(n, l, leg); });
        bench::Latency lat;
        lat.reserve(kSamplesPerCell);
        for (std::size_t s = 0; s < kSamplesPerCell; ++s) {
            const std::uint64_t a = bench::now_ns();
            for (std::size_t i = 0; i < batch; ++i) step(n, l, leg);
            const std::uint64_t window = bench::now_ns() - a;
            lat.add(window * kPsPerNs / batch);
        }
        emit_lat(round, tag, name_of(arm), name_of(leg), lat.summarize(), batch);
    }
    return n.faults("latency", 1);
}

/** @brief Sink for the sink counters, so no lane's work is optimised away. */
std::atomic<std::uint64_t> g_sink{0};

/**
 * @brief Run one (arm, leg, T) throughput point and emit its row.
 *
 * TWO legs are swept, not one, and the reason is attribution. WIDE / MID / NARROW differ from
 * each other ONLY in the net plane's topology; their graph plane is one locked store in all
 * three, because ADR-0079's fan is a net-plane fan and vertex placement is Stage 2 (#843). A
 * `full`-only table therefore shows every injected arm collapsing together with no way to say
 * which plane did it. Sweeping `net-fwd` beside `full` separates the two: the net leg is where
 * the arms genuinely differ, and the gap between the two legs is the graph plane's own term.
 */
[[nodiscard]] bool run_tput_point(int round, const char* tag, arm_t arm, leg_t leg, std::size_t T) {
    node_t n(arm, T, /*count_channels=*/false);

    std::atomic<std::uint64_t> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> total_ops{0};

    auto worker = [&](std::size_t i) {
        bench_store::lane_t& l = n.lane(i);
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
        }
        std::uint64_t local = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            step(n, l, leg);
            ++local;
        }
        total_ops.fetch_add(local, std::memory_order_relaxed);
    };

    std::vector<std::thread> workers;
    workers.reserve(T);
    for (std::size_t i = 0; i < T; ++i) workers.emplace_back(worker, i);
    while (ready.load(std::memory_order_acquire) < T) {
    }

    const std::uint64_t t0 = bench::now_ns();
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(kWindow);
    stop.store(true, std::memory_order_relaxed);
    for (auto& w : workers) w.join();
    const std::uint64_t t1 = bench::now_ns();

    const double secs = static_cast<double>(t1 - t0) / 1e9;
    const std::uint64_t ops = total_ops.load();
    const double agg = secs > 0 ? static_cast<double>(ops) / secs : 0.0;
    const double per_thread = agg / static_cast<double>(T);
    const std::uint64_t ns_per_op =
        per_thread > 0 ? static_cast<std::uint64_t>(1e9 / per_thread) : 0;
    emit_tput(round, tag, name_of(arm), name_of(leg), T, per_thread, agg, ns_per_op);

    for (std::size_t i = 0; i < T; ++i)
        g_sink.fetch_add(n.lane(i).out.bytes, std::memory_order_relaxed);
    return n.faults("throughput", T);
}

/**
 * @brief The deterministic memory column: per-store occupancy and per-channel draw counts.
 *
 * SINGLE-THREADED over all @p T lanes, deliberately. A concurrent run overlaps draws from the
 * shared stores, so its high-water depends on how the scheduler interleaved them — a number
 * that would move run to run and could not be gated or compared. Taken this way the figure is a
 * function of the workload and the topology alone, which is what makes it the trustworthy half
 * of the memory report (`bench/ram_census_pins.json` records the other half — a process-heap
 * high-water moving 66 % across runs — and that is why the escape column is never gated).
 */
[[nodiscard]] bool run_hwm_point(arm_t arm, std::size_t T) {
    node_t n(arm, T, /*count_channels=*/true);
    for (std::size_t k = 0; k < kHwmOpsPerLane; ++k) {
        for (std::size_t i = 0; i < T; ++i) n.step_full(n.lane(i));
    }
    std::size_t idx = 0;
    const char* last = "";
    for (const bench_store::stores_t::pool_view_t& p : n.stores().occupancy()) {
        idx = std::strcmp(p.label, last) == 0 ? idx + 1 : 0;
        last = p.label;
        emit_hwm(name_of(arm), T, p.label, idx, p);
    }
    const bench_store::stores_t& st = n.stores();
    const auto chan = [&](const char* label, const auto& p) {
        if (p) emit_chan(name_of(arm), T, label, *p);
    };
    chan("graph-ctl", st.count_ctl);
    chan("net-rx", st.count_rx);
    chan("net-egress", st.count_egr);
    for (const auto& p : st.count_lane_rx) chan("net-rx-per-lane", p);
    for (const auto& p : st.count_lane_tx) chan("net-egress-per-lane", p);
    return n.faults("hwm", T);
}

/**
 * @brief Break the instrument's line before believing any cell — the non-vacuity gate.
 *
 * Mirrors `bench_failable_census.cpp`'s `census_canary_seam_only` / `census_canary_heap_escape`
 * pattern (#1414/#1421): every check reports its own row and the accumulated failure count is
 * the exit code, so this is runnable in CI rather than a printout somebody reads.
 *
 *  1. **Per-channel seam reachability, WIDE.** Each of ADR-0079's four channels must show
 *     served blocks > 0 after ONE workload iteration. A channel wired to a store nothing draws
 *     from is an arm that silently measures nothing, and it is the single most likely way this
 *     harness ships vacuous — the egress leg in particular exists only because
 *     @ref bench_store::sink_out_t declines to override the gather form, which a well-meaning
 *     edit would undo without any other symptom.
 *  2. **Per-lane seam reachability, NARROW.** Same test on the per-child rx and per-link egress
 *     stores, which are a different wiring path (`add_child`'s `rx` argument and
 *     `transport_t::set_egress_source`) and can break independently.
 *  3. **The null arm.** Two occupancy snapshots with nothing between them must differ by
 *     exactly 0 — `bench_conn_ram.cpp`'s rule. A non-zero reading means the instrument is
 *     itself drawing inside the window and every memory figure is off by that.
 *  4. **No refusals, no overflow, no silent lane.** The same FAULT sweep the timed modes run.
 *
 * The ESCAPE canary — H-baseline must show a NON-ZERO process-heap escape, which catches an
 * `operator new` override gone blind — cannot live here: this TU deliberately has no override.
 * It is `bench_store_escape.cpp calibrate`, and `run_store_sweep.sh` runs both.
 *
 * @return 0 when every check passed.
 */
[[nodiscard]] int run_calibrate() {
    int failures = 0;
    const auto check = [&failures](const char* what, bool ok, const char* detail) {
        std::printf("CALIBRATE\t%s\t%s\t%s\n", what, ok ? "OK" : "FAIL", detail);
        if (!ok) ++failures;
    };

    {
        node_t n(arm_t::WIDE, /*lanes=*/1, /*count_channels=*/true);
        const bench_store::stores_t& st = n.stores();
        // The node's constructor already ran one full pass; run a second so a channel drawn
        // only on a warm path still shows.
        n.step_full(n.lane(0));
        const auto reach = [&](const char* label, const auto& p) {
            const bool ok = p && p->blocks.load() > 0;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "blocks=%zu", p ? p->blocks.load() : 0U);
            check(label, ok, buf);
        };
        reach("wide_seam_graph_ctl", st.count_ctl);
        reach("wide_seam_net_rx", st.count_rx);
        reach("wide_seam_net_egress", st.count_egr);
        check("wide_no_fault", n.faults("calibrate", 1), "overflow/refusal/silent-lane sweep");
    }

    {
        node_t n(arm_t::NARROW, /*lanes=*/2, /*count_channels=*/true);
        n.step_full(n.lane(0));
        n.step_full(n.lane(1));
        const bench_store::stores_t& st = n.stores();
        std::size_t rx_blocks = 0;
        std::size_t tx_blocks = 0;
        for (const auto& p : st.count_lane_rx) rx_blocks += p->blocks.load();
        for (const auto& p : st.count_lane_tx) tx_blocks += p->blocks.load();
        char buf[96];
        std::snprintf(buf, sizeof(buf), "blocks=%zu", rx_blocks);
        check("narrow_seam_per_child_rx", rx_blocks > 0, buf);
        std::snprintf(buf, sizeof(buf), "blocks=%zu", tx_blocks);
        check("narrow_seam_per_link_egress", tx_blocks > 0, buf);
        check("narrow_no_fault", n.faults("calibrate", 2), "overflow/refusal/silent-lane sweep");

        // (3) THE NULL ARM — two snapshots, nothing between them.
        const auto a = n.stores().occupancy();
        const auto b = n.stores().occupancy();
        std::size_t drift = 0;
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
            drift += b[i].used - a[i].used;
        }
        std::snprintf(buf, sizeof(buf), "delta_used=%zu", drift);
        check("null_arm_reads_zero", drift == 0 && a.size() == b.size(), buf);
    }

    std::printf("CALIBRATE\ttotal_failures\t%d\t-\n", failures);
    std::fflush(stdout);
    return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string_view mode =
        argc > 1 ? std::string_view(argv[1]) : std::string_view("latency");
    int round0 = 0;
    const char* tag = "A";
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--round0=", 0) == 0) {
            round0 = std::atoi(a.c_str() + 9);
        } else if (a.rfind("--tag=", 0) == 0) {
            tag = argv[i] + 6;
        }
    }

    if (mode == "calibrate") return run_calibrate();

    int faults = 0;
    constexpr std::size_t kNArms = sizeof(kArms) / sizeof(kArms[0]);

    if (mode == "latency") {
        std::printf("# RESULT_STORE_LAT round tag arm leg p50ps p99ps meanps n batch\n");
        for (std::size_t j = 0; j < kNArms; ++j) {
            // Rotate the arm order per round: exhausting one arm's runs before starting the
            // next is the shape that produced the recorded 55.2 / 53.0 / 149.8 M deliv/s swing
            // on identical code (bench/run_pin_ratio.sh), and nothing here does that.
            const arm_t arm = kArms[(static_cast<std::size_t>(round0) + j) % kNArms];
            if (!run_latency_arm(round0, tag, arm)) ++faults;
        }
    } else if (mode == "throughput") {
        std::printf(
            "# RESULT_STORE_TPUT round tag arm leg threads per_thread_ops_s agg_ops_s "
            "ns_per_op\n");
        for (const leg_t leg : {leg_t::NET, leg_t::FULL}) {
            for (std::size_t j = 0; j < kNArms; ++j) {
                const arm_t arm = kArms[(static_cast<std::size_t>(round0) + j) % kNArms];
                for (const std::size_t T : kThreads) {
                    if (!run_tput_point(round0, tag, arm, leg, T)) ++faults;
                }
            }
        }
    } else if (mode == "hwm") {
        std::printf("# RESULT_STORE_HWM arm threads store idx used capacity classes overflow\n");
        std::printf("# RESULT_STORE_CHAN arm threads channel blocks bytes peak_live refusals\n");
        for (const arm_t arm : kArms) {
            for (const std::size_t T : kThreads) {
                if (!run_hwm_point(arm, T)) ++faults;
            }
        }
    } else {
        std::fprintf(stderr, "usage: %s latency|throughput|hwm|calibrate [--round0=N] [--tag=X]\n",
                     argv[0]);
        return 2;
    }

    if (faults != 0) {
        std::fprintf(stderr, "\n%d faulted point(s) — the rows above are not trustworthy\n",
                     faults);
        return 1;
    }
    return static_cast<int>(g_sink.load() & 0);
}
