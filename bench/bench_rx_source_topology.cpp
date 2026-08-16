/**
 * @file
 * @brief The failable-source topology at the FWD router's RX seam: shared vs per-child
 *        (ADR-0067 §3, #625).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0067 §3 rules that a `pool_source_t` is owned by one thread wherever it sits on a
 * per-frame path, and #623 implemented that by giving each `fwd_router_t` child its own
 * source. The justification was ENTIRELY BORROWED measurement — ADR-0060 erratum 1's
 * shared-pool collapse, taken at a different seam — so the rule this bench exists to test
 * is one the author already shipped. It measures the router seam itself.
 *
 * Three configurations of the same workload, swept over T concurrent receive threads:
 *
 *   rx=heap-shared      one `heap_source()` for every child — today's default. glibc's
 *                       per-thread tcache should scale; this is the reference curve.
 *   rx=pool-shared      ONE `pool_source_t<sync_mutex_t>` behind every child — the shape
 *                       §3 forbids. If the erratum's signature reproduces here, the ADR
 *                       owns its evidence instead of borrowing it.
 *   rx=pool-per-child   what #623 ships: one `pool_source_t<sync_none_t>` per child, so
 *                       no two receive threads touch the same allocator cacheline.
 *
 * Total slab bytes are held EQUAL across the two pool configurations (the shared pool gets
 * T x the per-child slab), so the comparison is topology, not budget.
 *
 * The workload is the ROPE forward hop, which is the router's live `rx` draw on a path
 * that mutates no graph state: a multi-link rope arrives on child `c<i>`, its `dst` names
 * child `u<i>`, and the hop scatter-gathers the untouched links onward through a
 * `block_array_t` iov drawn from the inbound child's source. Each thread owns its own
 * inbound link, its own egress sink, and its own pre-built rope, so the source is the ONLY
 * object two threads can contend on. A contiguous forward would measure nothing here — it
 * is zero-heap by construction (ADR-0038, gated by bench_forward_heap).
 *
 * DIAGNOSTIC, not a CI gate: thread-contention numbers are runner-dependent, so this is
 * deliberately not wired into perf.yml's regression gate — the same call
 * bench_route_handle_contention and bench_fanout_clone_storm make.
 *
 * Output: the shared bench RESULT contract (bench_common.hpp) —
 *   mode=rx_source_<config>, fanout=T, pub_per_s=per-thread forwards/s,
 *   deliv_per_s=aggregate forwards/s, latency fields = per-thread ns/forward.
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/mem_source_sync.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/**
 * @brief Receive-thread counts to sweep.
 *
 * Fixed rather than clamped to `hardware_concurrency`, exactly as
 * bench_route_handle_contention does, so a many-core host measures the tail and a small
 * runner still shows the onset plus the oversubscribed regime.
 */
constexpr std::size_t kThreads[] = {1, 2, 4, 8, 16, 24};

constexpr auto kWindow = std::chrono::milliseconds(300); /**< @brief Wall-clock per point. */

/**
 * @brief Rope link count per frame — the iov width the forward hop must draw for.
 *
 * A reassembling transport hands up the frame in the pieces its own framing produced; four
 * is a representative fragmented-WS / CAN-group split, and it is what makes the draw
 * happen at all (a one-link rope needs no gather array).
 */
constexpr std::size_t kRopeLinks = 4;

/**
 * @brief Per-child slab, and the free-list class slots over it.
 *
 * Both are INJECTED (RFC-0006) — this bench is a deployment like any other, so it sizes
 * its own budget rather than the library holding one. Generous by design: the run asserts
 * `overflowed() == 0` and reports peak `used()`, so an under-sized slab would be reported
 * as a fault rather than silently distorting the numbers into a degraded path.
 */
constexpr std::size_t kSlabPerChild = 8192;
constexpr std::size_t kClassSlots = 8;

/** @brief Which source topology a run wires under the router's children. */
enum class topo_t { HEAP_SHARED, POOL_SHARED, POOL_PER_CHILD };

[[nodiscard]] const char* mode_of(topo_t t) {
    switch (t) {
        case topo_t::HEAP_SHARED:
            return "rx_source_heap-shared";
        case topo_t::POOL_SHARED:
            return "rx_source_pool-shared";
        case topo_t::POOL_PER_CHILD:
            return "rx_source_pool-per-child";
    }
    return "rx_source_?";
}

// --- wire builders (canonical bytes via the production emit helpers) ---------------------
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}

std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        (void)tr::wire::emit_path_segment(body, s);
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src) {
    const std::byte ob{static_cast<std::uint8_t>(op)};
    std::vector<std::byte> ov;
    tr::wire::emit_tlv(ov, type_t::VALUE, opt_t{}, std::span<const std::byte>(&ob, 1));

    std::vector<std::byte> body;
    body.insert(body.end(), ov.begin(), ov.end());
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief One heap-backed rope link over @p bytes (a genuine scatter-gather piece). */
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief Split @p bytes into @p links roughly-equal rope links, each its own segment. */
tr::view::rope_t rope_of(std::span<const std::byte> bytes, std::size_t links) {
    tr::view::rope_t r;
    const std::size_t step = bytes.size() / links;
    std::size_t at = 0;
    for (std::size_t i = 0; i + 1 < links && step > 0; ++i) {
        r.append(make_value(bytes.subspan(at, step)));
        at += step;
    }
    r.append(make_value(bytes.subspan(at)));
    return r;
}

// --- links -------------------------------------------------------------------------------
/** @brief The inbound link: hands the frame up as the rope it already is (ADR-0053 §5). */
class rope_in_t : public transport_t {
   public:
    void send(std::span<const std::byte>) override {}  // inbound-only
    [[nodiscard]] bool delivers_ropes() const override { return true; }
    void inject(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
};

/**
 * @brief The egress sink: counts frames and drops them.
 *
 * Deliberately does NOT retain the bytes the way the tests' `fake_link_t` does — a
 * per-thread vector growing for 300 ms would put the general allocator back into the
 * measurement, which is the thing under test.
 */
class sink_out_t : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        bytes += frame.size();
        ++frames;
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        for (const auto& s : iov) bytes += s.size();
        ++frames;
    }
    std::size_t frames = 0; /**< @brief Frames this link was handed. */
    std::size_t bytes = 0;  /**< @brief Their total size, kept so the gather is not elided. */
};

/** @brief One receive thread's private wiring: its links, its frame, and its own slab. */
struct lane_t {
    rope_in_t in;
    sink_out_t out;
    tr::view::rope_t frame;
    std::vector<std::byte> slab;
    std::vector<tr::mem::size_class_t> classes;
    std::unique_ptr<tr::mem::pool_source_t<tr::mem::sync_none_t>> pool;
};

std::atomic<std::uint64_t> g_sink{0};

/**
 * @brief Run one (topology, T) point and emit its RESULT line.
 *
 * @return false if the run hit a fault that makes its numbers meaningless — a slab
 *         overflow (the pool fell back to counting rather than serving) or a lane that
 *         forwarded nothing (the frame never routed, so the loop timed an error path).
 */
bool run_point(topo_t topo, std::size_t T) {
    graph_t g;

    // The shared pool gets T x the per-child slab, so both pool configurations are given
    // the SAME total budget and only their topology differs.
    std::vector<std::byte> shared_slab(kSlabPerChild * T);
    std::vector<tr::mem::size_class_t> shared_classes(kClassSlots);
    tr::mem::pool_source_t<tr::mem::sync_mutex_t> shared_pool{shared_slab, shared_classes};

    tr::mem::block_source_t* router_default =
        topo == topo_t::POOL_SHARED ? static_cast<tr::mem::block_source_t*>(&shared_pool)
                                    : &tr::mem::heap_source();
    fwd_router_t router(g, std::pmr::get_default_resource(), router_default);

    std::vector<std::unique_ptr<lane_t>> lanes;  // stable addresses: the router holds pointers
    lanes.reserve(T);
    for (std::size_t i = 0; i < T; ++i) {
        auto lane = std::make_unique<lane_t>();
        const std::string in_name = "c" + std::to_string(i);
        const std::string out_name = "u" + std::to_string(i);

        const std::vector<std::byte> bytes =
            b_fwd(fwd_op_t::READ, b_path({out_name, "sensor"}), b_path({"reply-ep"}));
        lane->frame = rope_of(bytes, kRopeLinks);

        tr::mem::block_source_t* rx = nullptr;  // null => the router's default
        if (topo == topo_t::POOL_PER_CHILD) {
            lane->slab.resize(kSlabPerChild);
            lane->classes.resize(kClassSlots);
            lane->pool = std::make_unique<tr::mem::pool_source_t<tr::mem::sync_none_t>>(
                lane->slab, lane->classes);
            rx = lane->pool.get();
        }
        router.add_child(in_name, lane->in, rx);
        router.add_child(out_name, lane->out);
        lanes.push_back(std::move(lane));
    }

    // Route one frame per lane before the timed window: the first forward through a child
    // carves its slab and warms every lazily-built table, which would otherwise land
    // entirely in T=1's first microseconds and nowhere else.
    for (auto& lane : lanes) lane->in.inject(lane->frame);

    std::atomic<std::uint64_t> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> total_ops{0};

    auto worker = [&](std::size_t i) {
        lane_t& lane = *lanes[i];
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) {
        }
        std::uint64_t local = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            lane.in.inject(lane.frame);  // copy: a refcount bump on this lane's own links
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
    const double agg_ops_s = secs > 0 ? static_cast<double>(ops) / secs : 0.0;
    const double per_thread_ops_s = agg_ops_s / static_cast<double>(T);
    const std::uint64_t ns_per_op =
        per_thread_ops_s > 0 ? static_cast<std::uint64_t>(1e9 / per_thread_ops_s) : 0;

    const bench::Latency::Summary lat{ns_per_op, ns_per_op, ns_per_op};
    bench::emit("libtracer", mode_of(topo), /*size_bytes=*/kRopeLinks, /*fanout=*/T,
                /*endpoints=*/1, per_thread_ops_s, agg_ops_s, /*mb_per_s=*/0.0, lat);

    // Fault checks. A pool that overflowed stopped recycling and started counting, and a
    // lane that forwarded nothing timed a miss — either way the line above would be a
    // number describing something other than the workload, so say so loudly.
    bool ok = true;
    for (std::size_t i = 0; i < T; ++i) {
        g_sink.fetch_add(lanes[i]->out.bytes, std::memory_order_relaxed);
        if (lanes[i]->out.frames == 0) {
            std::fprintf(stderr, "FAULT %s T=%zu: lane %zu forwarded nothing\n", mode_of(topo), T,
                         i);
            ok = false;
        }
        if (lanes[i]->pool && lanes[i]->pool->overflowed() != 0) {
            std::fprintf(stderr, "FAULT %s T=%zu: lane %zu pool overflowed %zu\n", mode_of(topo), T,
                         i, lanes[i]->pool->overflowed());
            ok = false;
        }
    }
    if (topo == topo_t::POOL_SHARED && shared_pool.overflowed() != 0) {
        std::fprintf(stderr, "FAULT %s T=%zu: shared pool overflowed %zu\n", mode_of(topo), T,
                     shared_pool.overflowed());
        ok = false;
    }

    // Peak slab actually needed, on stderr so it never pollutes the RESULT stream. This is
    // the figure a deployment sizes its own span against, and it is the evidence that the
    // 8 KiB above is a ceiling rather than a constraint the run bumped into.
    if (topo == topo_t::POOL_SHARED) {
        std::fprintf(stderr, "  [slab] %s T=%zu shared used=%zu/%zu classes=%zu\n", mode_of(topo),
                     T, shared_pool.used(), shared_slab.size(), shared_pool.classes_used());
    } else if (topo == topo_t::POOL_PER_CHILD) {
        std::fprintf(stderr, "  [slab] %s T=%zu lane0 used=%zu/%zu classes=%zu\n", mode_of(topo), T,
                     lanes[0]->pool->used(), kSlabPerChild, lanes[0]->pool->classes_used());
    }
    return ok;
}

}  // namespace

int main() {
    int faults = 0;
    for (const topo_t topo : {topo_t::HEAP_SHARED, topo_t::POOL_SHARED, topo_t::POOL_PER_CHILD}) {
        for (const std::size_t T : kThreads) {
            if (!run_point(topo, T)) ++faults;
        }
    }
    if (faults != 0) {
        std::fprintf(stderr, "\n%d faulted point(s) — the RESULT lines above are not trustworthy\n",
                     faults);
        return 1;
    }
    return static_cast<int>(g_sink.load() & 0);
}
