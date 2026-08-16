/**
 * @file
 * @brief RFC-0022 §6, host half — end-to-end FWD{WRITE} delivery over real UDP, TWO PROCESSES,
 *        counted by the RECEIVER, with a bounded RX pool whose free-slot floor is the RAM
 *        instrument.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A sender-side rate is not a measurement: this bench's throughput number is the count the
 * SUBSCRIBER process observed landing in its graph, and nothing else. The publisher reports
 * only how many datagrams it handed to the kernel, for a loss figure.
 *
 * @section pin_net_pool Why the receiver runs on a bounded pool, not the heap
 *
 * §6's MCU half is a pool-dynamics question — "the receive pool is small" — and the host
 * half is where the latency win is supposed to live. A heap-backed receiver can measure only
 * the second: `heap_backend` never refuses, so no arm can ever show the cost of holding a
 * segment. Backing the receiver with `sync_pool_t` over a fixed slab makes the first
 * observable on the host too: a pinned value holds its whole RX slot for its lifetime, so
 * `available()` is a free-slot floor and `dropped_rx()` is backpressure onset, both sampled
 * while the load runs. It also puts `segment_bytes` under the bench's control, because the
 * transport sizes its RX segment at `min(kMaxDatagram, backend->max_segment_size())` — which
 * is the ONLY lever that makes §3.D's predicate reachable at a sane K (on the default heap
 * backend the segment is 65,536 B whatever the datagram's length, so a 1 KB payload needs
 * K >= 64 before it pins at all).
 *
 * One recv thread allocates from the pool, so `sync_pool_t`'s mutex is uncontended here and
 * ADR-0060 Erratum 1's multi-thread pool collapse is not in play — and it is identical in
 * every arm regardless.
 *
 * @section pin_net_reach Reachability
 *
 * The subscriber reports `pins` / `copies` by segment-pointer identity between the stored
 * value and the RX segments the backend handed out — an outcome, available with or without
 * `LIBTRACER_PIN_INSTRUMENT`. An arm that intends to pin and reports zero pins invalidates its
 * own row.
 *
 * @section pin_net_control What the control arm is
 *
 * **Arm B, the SENTINEL arm — this same binary run at `K = tr::graph::kPinNever`**, not a
 * separate pre-RFC build. This source used to double as a build against untouched
 * `origin/main`; RFC-0022 §3.B deleted `settings_t`, so there is no longer a main for it to
 * compile against and that arm is gone permanently. The sentinel arm controls for the thing
 * that is still controllable — the ADR-0041 §2 one-copy store branch, reached on the same
 * binary, the same pool and the same transport as every pinning arm — which is what the paired
 * per-round delta in `collate_pin.py` is taken against.
 *
 * K reaches the store site through `graph_t::set_pin_payload_ratio`, the owner-declared
 * per-vertex override RFC-0022 §3.D keeps for exactly this: rotating arms inside one process.
 *
 * Usage:
 *   bench_pin_net sub <port> <K> <payload_bytes> <slot_bytes> <slots> <ms> [vertices] [label]
 *   bench_pin_net pub <port> <payload_bytes> <count> [vertices]
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_udp.hpp"

using namespace std::chrono_literals;

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief A NAME TLV. */
std::vector<std::byte> b_name(std::string_view s) {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, s);
    return out;
}

/** @brief A PATH TLV over `segs`. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) {
        (void)tr::wire::emit_path_segment(body, s);
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A one-byte VALUE TLV. */
std::vector<std::byte> b_u8_value(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief A trailer-less VALUE TLV of `n` payload bytes — the pinnable shape. */
std::vector<std::byte> b_value(std::size_t n) {
    std::vector<std::byte> p(n);
    for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::byte>(i & 0xFF);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

/** @brief The wire frame the publisher sends: FWD{WRITE} to `/sensor/blob<idx>`. */
std::vector<std::byte> fwd_write_frame(std::size_t payload_bytes, std::size_t idx) {
    const std::string leaf = "blob" + std::to_string(idx);
    std::vector<std::byte> body;
    const auto app = [&body](const std::vector<std::byte>& s) {
        body.insert(body.end(), s.begin(), s.end());
    };
    app(b_u8_value(static_cast<std::uint8_t>(fwd_op_t::WRITE)));
    app(b_path({"sensor", leaf}));
    app(b_path({"client"}));
    app(b_value(payload_bytes));
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/**
 * @brief A pool backend that also REMEMBERS which segments it handed out.
 *
 * The membership set is what turns "the stored value's segment" into a pin/copy verdict
 * without needing the decision site's counters — an OUTCOME instrument, so a build with
 * `LIBTRACER_PIN_INSTRUMENT` off still reports a reachability figure per row.
 */
class recording_pool_t final : public tr::mem::mem_backend_t {
   public:
    recording_pool_t(std::span<std::byte> slab, std::size_t slot)
        : mem_backend_t("bench_recording_pool"), inner_(slab, slot) {}

    tr::view::segment_t* alloc(std::size_t n, tr::mem::alloc_hint_t hint) override {
        const std::lock_guard<std::mutex> g(m_);
        tr::view::segment_t* seg = inner_.alloc(n, hint);
        if (seg != nullptr) {
            // Re-point reclaim at THIS backend (sync_pool_t's trick): the devirtualized POOL
            // fast path in destroy_dispatch would return the slot without the mutex, and the
            // free-slot floor would then be sampled against a racing counter.
            seg->backend = this;
            seg->btag = tr::mem::backend_tag::UNKNOWN;
            issued_.insert(seg);
        }
        return seg;
    }
    void destroy(tr::view::segment_t* seg) noexcept override {
        const std::lock_guard<std::mutex> g(m_);
        inner_.destroy(seg);
    }
    [[nodiscard]] std::size_t alignment() const noexcept override { return inner_.alignment(); }
    [[nodiscard]] std::size_t max_segment_size() const noexcept override {
        return inner_.max_segment_size();
    }
    [[nodiscard]] tr::mem::backend_tag tag() const noexcept override {
        return tr::mem::backend_tag::UNKNOWN;
    }

    /**
     * @brief Was @p p one of this pool's RX slots?
     *
     * The pin/copy verdict, and the one that needs no compile-time instrument: a pinned store
     * holds an RX slot, a copied store holds a fresh heap segment. Bounded by the slot count,
     * because a pool reuses its `segment_t` control blocks.
     */
    [[nodiscard]] bool issued(const tr::view::segment_t* p) const {
        const std::lock_guard<std::mutex> g(m_);
        return issued_.count(p) != 0;
    }
    /** @brief Free slots right now — sampled during the load, so the MINIMUM is a real floor. */
    [[nodiscard]] std::size_t available() {
        const std::lock_guard<std::mutex> g(m_);
        return inner_.available();
    }
    [[nodiscard]] std::size_t capacity() const noexcept { return inner_.capacity(); }

   private:
    mutable std::mutex m_;
    tr::mem::pool_t inner_;
    std::unordered_set<const tr::view::segment_t*> issued_;
};

/** @brief The delivery-COUNTING receiver process. */
int run_sub(int argc, char** argv) {
    const std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    const std::uint32_t k = static_cast<std::uint32_t>(std::strtoul(argv[3], nullptr, 10));
    const std::size_t payload = static_cast<std::size_t>(std::atoi(argv[4]));
    const std::size_t slot = static_cast<std::size_t>(std::atoi(argv[5]));
    const std::size_t slots = static_cast<std::size_t>(std::atoi(argv[6]));
    const int expect_ms = std::atoi(argv[7]);
    const std::size_t vertices = argc > 8 ? static_cast<std::size_t>(std::atoi(argv[8])) : 1;
    const char* label = argc > 9 ? argv[9] : "?";

    std::vector<std::byte> slab((slot + 128) * slots + 4096);
    recording_pool_t pool(std::span<std::byte>(slab), slot);

    graph_t node;
    tr::net::fwd_router_t router(node);
    tr::net::udp_transport_t t(port, "127.0.0.1", static_cast<std::uint16_t>(port + 1), &pool);
    if (!t.ok()) {
        std::fprintf(stderr, "sub: bind failed on %u\n", port);
        return 1;
    }

    // WHY MANY VERTICES. A single STORED_VALUE vertex holds exactly ONE value, so pinning it
    // holds exactly ONE RX slot however large the segment is — the pool never gets tight and
    // §6's RAM question cannot be asked at all. The held quantity is `live vertices x
    // segment_bytes`, so the vertex count IS the RAM axis: at `vertices > slots` a pinned
    // steady state cannot fit in the pool and the transport must start refusing datagrams,
    // which is the backpressure onset the acceptance criterion asks for.
    for (std::size_t i = 0; i < vertices; ++i) {
        const tr::graph::vertex_handle_t v =
            node.register_vertex(path_t("/sensor/blob" + std::to_string(i)), role_t::STORED_VALUE);
        node.set_pin_payload_ratio(v, k);  // the arm's K, owner-declared (RFC-0022 §3.D)
    }
    router.add_child("a", t);

    std::atomic<std::uint64_t> delivered{0};
    std::atomic<std::uint64_t> pins{0};
    std::atomic<std::uint64_t> copies{0};
    // Counted in the subscribe callback: the store has already happened when this fires, so
    // the value it reads is the one that landed, and its segment is the pin/copy verdict.
    auto on_blob = [&](const tr::view::rope_t& r) {
        delivered.fetch_add(1, std::memory_order_relaxed);
        if (r.link_count() == 1 && r.links()[0].owner && pool.issued(r.links()[0].owner.get()))
            pins.fetch_add(1, std::memory_order_relaxed);
        else
            copies.fetch_add(1, std::memory_order_relaxed);
    };
    for (std::size_t i = 0; i < vertices; ++i)
        (void)node.subscribe(path_t("/sensor/blob" + std::to_string(i)), on_blob);

    std::printf("SUB_READY\n");
    std::fflush(stdout);

    // Sample the pool while the load runs — a floor read after the load is a floor of nothing.
    std::size_t free_floor = pool.capacity();
    const auto t0 = std::chrono::steady_clock::now();
    const auto deadline = t0 + std::chrono::milliseconds(expect_ms);
    std::uint64_t last = 0;
    auto last_change = t0;
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
        free_floor = std::min(free_floor, pool.available());
        const std::uint64_t d = delivered.load(std::memory_order_relaxed);
        if (d != last) {
            last = d;
            last_change = std::chrono::steady_clock::now();
        } else if (d > 0 && std::chrono::steady_clock::now() - last_change > 200ms) {
            break;  // the publisher is done and the queue has drained
        }
    }
    const double secs = std::chrono::duration<double>(last_change - t0).count();
    const std::uint64_t n = delivered.load();

    std::printf(
        "RESULT_PINNET\t%s\t%u\t%zu\t%zu\t%zu\t%llu\t%.0f\t%llu\t%llu\t%zu\t%llu\t%zu\t%zu\n",
        label, k, payload, slot, slots, static_cast<unsigned long long>(n),
        secs > 0 ? static_cast<double>(n) / secs : 0.0,
        static_cast<unsigned long long>(pins.load()),
        static_cast<unsigned long long>(copies.load()), free_floor,
        static_cast<unsigned long long>(t.dropped_rx()), vertices, pool.capacity());
    std::fflush(stdout);
    return 0;
}

/** @brief The publisher process. Its rate is NOT the measurement; only its send count is. */
int run_pub(int argc, char** argv) {
    (void)argc;
    const std::uint16_t port = static_cast<std::uint16_t>(std::atoi(argv[2]));
    const std::size_t payload = static_cast<std::size_t>(std::atoi(argv[3]));
    const std::uint64_t count = static_cast<std::uint64_t>(std::atoll(argv[4]));
    const std::size_t vertices = argc > 5 ? static_cast<std::size_t>(std::atoi(argv[5])) : 1;

    tr::net::udp_transport_t t(static_cast<std::uint16_t>(port + 1), "127.0.0.1", port);
    if (!t.ok()) {
        std::fprintf(stderr, "pub: bind failed on %u\n", port + 1);
        return 1;
    }
    // Round-robin over the vertex set so every vertex holds a live value at once — the
    // steady state whose pool occupancy is the RAM measurement.
    std::vector<std::vector<std::byte>> frames;
    for (std::size_t i = 0; i < vertices; ++i) frames.push_back(fwd_write_frame(payload, i));
    const std::vector<std::byte>& frame = frames[0];
    std::this_thread::sleep_for(150ms);  // let the subscriber's recv thread settle

    const std::uint64_t a = bench::now_ns();
    for (std::uint64_t i = 0; i < count; ++i) {
        t.send(frames[i % vertices]);
        // A pure blast overruns the loopback socket buffer and measures the kernel's drop
        // policy instead of the store leg. A tiny yield every 64 frames keeps the receiver's
        // recv thread scheduled without pacing the load — and is identical in every arm.
        if ((i & 63) == 0) std::this_thread::yield();
    }
    const std::uint64_t b = bench::now_ns();
    std::printf("PUB_SENT\t%llu\t%zu\t%.0f\n", static_cast<unsigned long long>(count), frame.size(),
                static_cast<double>(count) / (static_cast<double>(b - a) / 1e9));
    std::fflush(stdout);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 8 && std::string_view(argv[1]) == "sub") return run_sub(argc, argv);
    if (argc >= 5 && std::string_view(argv[1]) == "pub") return run_pub(argc, argv);
    std::fprintf(stderr,
                 "usage: bench_pin_net sub <port> <K> <payload> <slot> <slots> <ms> "
                 "[vertices] [label]\n"
                 "       bench_pin_net pub <port> <payload> <count> [vertices]\n");
    return 2;
}
