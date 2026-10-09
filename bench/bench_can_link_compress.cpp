/**
 * @file
 * @brief #1953 — CAN link-local compression, before and after: frames, wire bytes, time.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * REPORT ONLY; no gate reads it. Two `can_transport_t` ends over an in-memory, PUMPED bus
 * (write_raw queues, the bench thread drains), sending directed FWD{WRITE} deliveries of N
 * streams round-robin (a stream = one destination; every frame is the same length). Two arms,
 * alternated round by round inside one process so drift is common-mode:
 *
 *   before  both ends run with `compress_ids = 0`: the wire `main` puts on the bus before
 *           #1953 — an advertise manifest (header + identity path) on the control
 *           identifier, then the frame's slices on a fresh group block.
 *   after   both ends announce compression: a stream's second send binds it to a block of
 *           its sender's stream identifiers, and every later send is the frame minus its
 *           FWD prefix, one slice per identifier, with no manifest.
 *
 * STEADY rows (one per mode x payload x N x arm), N = 1, 64 and `max`, the most streams a
 * 512-identifier window holds at this frame's slice count (512 / k):
 *   frames      CAN frames one send puts on the bus, averaged over the N streams
 *   wire_B      data-field bytes one send puts on the bus
 *   native      share of sends that went on stream identifiers
 *   ns_send     best-of-rounds mean host time per send, sender encode through receiver
 *               delivery, one thread, no I/O. It prices the per-send TX lookup (one hash
 *               of the prefix and one keyed search) as N grows. NOT a bus figure: at
 *               500 kbit/s one classic frame is ~250 us on the wire.
 *
 * CHURN rows (after arm against before, same mode and payload):
 *   oneshot     4096 distinct streams, each sent once: what a stream that never repeats
 *               costs (it should be exactly the before wire).
 *   beyond      1.5 x max streams round-robin: the window holds max, the rest go whole and
 *               nothing is evicted.
 *   rehello     max streams bound, then the receiver restarts (its join hello heard): the
 *               frames of the first pass afterwards (every stream binds again) and of the
 *               second (native again), and the receiver's acknowledgement frames.
 *   lost-hello  the same restart with the hello lost: frames lost before the link
 *               recovers, and the recovery pass's cost.
 *
 * Usage: bench_can_link_compress [rounds [payload]]   (default 7 rounds, every payload)
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "../core/tests/fwd_frame_builder.hpp"  // host-only frame builders
#include "bench_common.hpp"
#include "bench_process.hpp"
#include "libtracer/can.hpp"
#include "libtracer/transport_can.hpp"

namespace {

namespace can = tr::net::can;
using bytes_t = std::vector<std::byte>;
using frame_t = tr::net::can_frame_data_t;

class pump_link_t;

/** @brief The pumped bus: queues on write, delivers on @ref pump, counts by emitting node. */
class pump_bus_t {
   public:
    struct cost_t {
        std::size_t frames = 0;
        std::size_t bytes = 0;
        std::size_t control = 0;  ///< frames on the control identifier (endpoint 0)
    };
    void attach(pump_link_t* l) { links_.push_back(l); }
    void detach(pump_link_t* l) {
        std::erase(links_, l);
        for (auto& q : q_)
            if (q.first == l) q.first = nullptr;
    }
    void post(pump_link_t* from, const frame_t& f) {
        if (drop_ && drop_(f)) return;
        if (const auto fields = can::decode_can_id(f.id)) {
            cost_t& c = cost_[fields->node];
            ++c.frames;
            c.bytes += f.len;
            c.control += fields->endpoint == 0 ? 1 : 0;
        }
        q_.emplace_back(from, f);
    }
    void pump();
    void set_drop(std::function<bool(const frame_t&)> pred) { drop_ = std::move(pred); }
    [[nodiscard]] cost_t cost(std::uint16_t node) const {
        const auto it = cost_.find(node);
        return it == cost_.end() ? cost_t{} : it->second;
    }

   private:
    std::vector<pump_link_t*> links_;
    std::deque<std::pair<pump_link_t*, frame_t>> q_;
    std::function<bool(const frame_t&)> drop_;
    std::map<std::uint16_t, cost_t> cost_;
};

class pump_link_t : public tr::net::can_link_t {
   public:
    explicit pump_link_t(pump_bus_t& bus) : bus_(bus) { bus_.attach(this); }
    ~pump_link_t() override { bus_.detach(this); }
    pump_link_t(const pump_link_t&) = delete;
    pump_link_t& operator=(const pump_link_t&) = delete;
    void write_raw(const frame_t& f) override { bus_.post(this, f); }
    void on_receive(rx_fn_t rx) override { rx_ = std::move(rx); }
    void start() override {}
    void deliver(const frame_t& f) {
        if (rx_) rx_(f);
    }

   private:
    pump_bus_t& bus_;
    rx_fn_t rx_;
};

void pump_bus_t::pump() {
    while (!q_.empty()) {
        const auto [from, f] = q_.front();
        q_.pop_front();
        const std::vector<pump_link_t*> snapshot = links_;
        for (pump_link_t* l : snapshot)
            if (l != from) l->deliver(f);
    }
}

/** @brief Counts deliveries and keeps the last frame's size, so nothing is optimized out. */
struct sink_t {
    std::size_t count = 0;
    std::size_t last = 0;
    void operator()(std::span<const std::byte> f) {
        ++count;
        last = f.size();
    }
};

std::unique_ptr<tr::net::can_transport_t> make_node(pump_bus_t& bus, std::uint16_t node,
                                                    const char* path, bool compress,
                                                    can::can_frame_mode_t mode) {
    tr::net::transport_can_config_t c;
    c.node = node;
    c.mode = mode;
    c.path = path;
    c.compress_ids = compress ? tr::net::kCanStreamIds : 0;
    return std::make_unique<tr::net::can_transport_t>(
        tr::mem::make_poly<pump_link_t>(tr::mem::net_source(), bus), c);
}

/** @brief One sender-receiver pair on its own bus. */
struct pair_t {
    pump_bus_t bus;
    bool compress;
    can::can_frame_mode_t mode;
    std::unique_ptr<tr::net::can_transport_t> a, b;
    sink_t at_b;
    tr::net::transport_t* to_b = nullptr;

    pair_t(bool on, can::can_frame_mode_t m) : compress(on), mode(m) {
        a = make_node(bus, 1, "sensor", on, m);
        restart_receiver(false);
        to_b = a->peer_link("n2");
    }
    /** @brief A fresh receiver; with @p lose_hello its join hello never reaches the bus. */
    void restart_receiver(bool lose_hello) {
        b.reset();
        if (lose_hello)
            bus.set_drop([](const frame_t& f) {
                const auto fields = can::decode_can_id(f.id);
                return fields && fields->node == 2;
            });
        b = make_node(bus, 2, "logger", compress, mode);
        b->set_receiver(at_b);
        bus.pump();
        bus.set_drop(nullptr);
    }
    struct pass_t {
        pump_bus_t::cost_t tx;      ///< what the sender put on the bus
        std::size_t rx_frames = 0;  ///< what the receiver put on the bus (acks)
        std::size_t delivered = 0;
        std::size_t sends = 0;
    };
    /** @brief Send every frame of @p streams once, in order. */
    pass_t pass(const std::vector<bytes_t>& streams) {
        const pump_bus_t::cost_t a0 = bus.cost(1);
        const pump_bus_t::cost_t b0 = bus.cost(2);
        const std::size_t d0 = at_b.count;
        std::size_t native = 0;
        for (const bytes_t& f : streams) {
            const std::size_t c0 = bus.cost(1).control;
            to_b->send(f);
            bus.pump();
            native += bus.cost(1).control == c0 ? 1 : 0;
        }
        const pump_bus_t::cost_t a1 = bus.cost(1);
        return {{a1.frames - a0.frames, a1.bytes - a0.bytes, native},
                bus.cost(2).frames - b0.frames,
                at_b.count - d0,
                streams.size()};
    }
};

/** @brief A delivery-shaped FWD{WRITE} to `telemetry/imu/sNNNN`, src `n1/cli`, a VALUE. */
bytes_t make_frame(std::size_t payload, std::size_t stream) {
    bytes_t body(payload);
    for (std::size_t i = 0; i < payload; ++i) body[i] = static_cast<std::byte>(i * 31 + 7);
    bytes_t value;
    tr::wire::emit_tlv(value, tr::wire::type_t::VALUE, tr::wire::opt_t{.ll = payload > 0xFFFF},
                       body);
    char leaf[8];
    std::snprintf(leaf, sizeof leaf, "s%04zu", stream % 10000);
    const bytes_t dst = tr::testing::b_path({"telemetry", "imu", leaf});
    const bytes_t src = tr::testing::b_path({"n1", "cli"});
    return tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE, dst, src, {}, value);
}

std::vector<bytes_t> make_streams(std::size_t payload, std::size_t first, std::size_t n) {
    std::vector<bytes_t> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) out.push_back(make_frame(payload, first + i));
    return out;
}

/** @brief Native slices one send of this payload needs: its VALUE over the frame's step. */
std::size_t slices_of(std::size_t payload, can::can_frame_mode_t mode) {
    const std::size_t value = payload + (payload > 0xFFFF ? 10 : 4);  // VALUE TLV header
    const std::size_t step = can::can_max_data(mode);
    return (value + step - 1) / step;
}

/** @brief Sends per timed round, sized so a round is milliseconds at every payload. */
std::size_t sends_for(std::size_t payload, std::size_t n) {
    const std::size_t want = std::max<std::size_t>(256, 400000 / (payload + 64));
    return std::max<std::size_t>(1, want / n) * n;
}

double per(std::size_t total, std::size_t n) {
    return static_cast<double>(total) / static_cast<double>(n);
}

void steady(can::can_frame_mode_t mode, const char* mode_name, std::size_t payload, std::size_t n,
            const char* n_name, std::size_t rounds) {
    const std::vector<bytes_t> streams = make_streams(payload, 0, n);
    std::array<std::unique_ptr<pair_t>, 2> pairs;
    std::array<pair_t::pass_t, 2> cost{};
    std::array<double, 2> best{std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::infinity()};
    std::array<bool, 2> exact{true, true};
    for (int arm = 0; arm < 2; ++arm) {
        pairs[arm] = std::make_unique<pair_t>(arm == 1, mode);
        (void)pairs[arm]->pass(streams);  // first sighting
        (void)pairs[arm]->pass(streams);  // bind
        cost[arm] = pairs[arm]->pass(streams);
        exact[arm] = cost[arm].delivered == n && pairs[arm]->at_b.last == streams.back().size();
    }
    const std::size_t sends = sends_for(payload, n);
    for (std::size_t r = 0; r < rounds; ++r) {
        for (int arm = 0; arm < 2; ++arm) {
            pair_t& p = *pairs[arm];
            const std::size_t before = p.at_b.count;
            const std::uint64_t t0 = bench::now_ns();
            for (std::size_t i = 0; i < sends; ++i) {
                p.to_b->send(streams[i % n]);
                p.bus.pump();
            }
            const std::uint64_t t1 = bench::now_ns();
            exact[arm] = exact[arm] && p.at_b.count == before + sends;
            best[arm] = std::min(best[arm], per(t1 - t0, sends));
        }
    }
    const char* const names[2] = {"before", "after"};
    for (int arm = 0; arm < 2; ++arm) {
        const pair_t::pass_t& c = cost[arm];
        std::printf(
            "STEADY mode=%s payload=%zu frame_B=%zu streams=%zu(%s) arm=%s frames=%.2f "
            "wire_B=%.1f native=%.0f%% ns_send=%.0f delivered_exact=%s\n",
            mode_name, payload, streams.front().size(), n, n_name, names[arm],
            per(c.tx.frames, c.sends), per(c.tx.bytes, c.sends), 100.0 * per(c.tx.control, c.sends),
            best[arm], exact[arm] ? "yes" : "NO");
    }
}

void churn(can::can_frame_mode_t mode, const char* mode_name, std::size_t payload,
           std::size_t max) {
    // oneshot: every stream sent once.
    {
        const std::vector<bytes_t> streams = make_streams(payload, 0, 4096);
        pair_t before(false, mode);
        pair_t after(true, mode);
        const auto b = before.pass(streams);
        const auto a = after.pass(streams);
        std::printf(
            "CHURN mode=%s payload=%zu row=oneshot streams=4096 before_frames=%.2f "
            "after_frames=%.2f rx_frames=%zu delivered=%zu/%zu\n",
            mode_name, payload, per(b.tx.frames, b.sends), per(a.tx.frames, a.sends), a.rx_frames,
            a.delivered, a.sends);
    }
    if (max == 0) return;
    // beyond: half as many again as the window holds.
    {
        const std::size_t over = max + max / 2;
        const std::vector<bytes_t> streams = make_streams(payload, 0, over);
        pair_t before(false, mode);
        pair_t after(true, mode);
        for (int i = 0; i < 2; ++i) {
            (void)before.pass(streams);
            (void)after.pass(streams);
        }
        const auto b = before.pass(streams);
        const auto a = after.pass(streams);
        std::printf(
            "CHURN mode=%s payload=%zu row=beyond streams=%zu before_frames=%.2f "
            "after_frames=%.2f native=%.0f%% delivered=%zu/%zu\n",
            mode_name, payload, over, per(b.tx.frames, b.sends), per(a.tx.frames, a.sends),
            100.0 * per(a.tx.control, a.sends), a.delivered, a.sends);
    }
    // rehello and lost-hello: max streams bound, then the receiver restarts.
    for (const bool lose : {false, true}) {
        const std::vector<bytes_t> streams = make_streams(payload, 0, max);
        pair_t after(true, mode);
        for (int i = 0; i < 3; ++i) (void)after.pass(streams);
        const auto steady_pass = after.pass(streams);
        after.restart_receiver(lose);
        const auto p1 = after.pass(streams);
        const auto p2 = after.pass(streams);
        const auto p3 = after.pass(streams);
        std::printf(
            "CHURN mode=%s payload=%zu row=%s streams=%zu steady_frames=%.2f "
            "pass1_frames=%.2f pass2_frames=%.2f pass3_frames=%.2f acks=%zu lost=%zu\n",
            mode_name, payload, lose ? "lost-hello" : "rehello", max,
            per(steady_pass.tx.frames, max), per(p1.tx.frames, max), per(p2.tx.frames, max),
            per(p3.tx.frames, max), p1.rx_frames + p2.rx_frames + p3.rx_frames,
            3 * max - (p1.delivered + p2.delivered + p3.delivered));
    }
}

}  // namespace

int main(int argc, char** argv) {
    bench::pin_allocator_state(argv);
    bench::emit_clock_floor();
    bench::emit_alloc_state();
    const std::size_t start_kb = bench::peak_rss_kb();
    const std::size_t rounds = argc > 1 ? static_cast<std::size_t>(std::atoi(argv[1])) : 7;
    const std::size_t only = argc > 2 ? static_cast<std::size_t>(std::atoi(argv[2])) : 0;
    std::printf("# CAN link-local compression, N directed streams, report only (#1953)\n");
    constexpr std::size_t kPayloads[] = {4, 64, 1024};
    for (const can::can_frame_mode_t mode :
         {can::can_frame_mode_t::CLASSIC, can::can_frame_mode_t::FD}) {
        const char* const name = mode == can::can_frame_mode_t::FD ? "fd" : "classic";
        for (const std::size_t p : kPayloads) {
            if (only != 0 && p != only) continue;
            const std::size_t k = slices_of(p, mode);
            const std::size_t max =
                k <= tr::net::kCanMaxStreamSlices ? tr::net::kCanStreamIds / k : 0;
            steady(mode, name, p, 1, "1", rounds);
            steady(mode, name, p, 64, "64", rounds);
            if (max != 0) steady(mode, name, p, max, "max", rounds);
            churn(mode, name, p, max);
        }
    }
    bench::emit_family_rss("can-link-compress", start_kb);
    return 0;
}
