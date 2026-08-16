/**
 * @file
 * @brief The producer fan-out's REPLY LEG — what one remote delivery pays to find its link.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A write to a vertex with N remote subscribers runs `fwd_router_t::deliver_remote` N times, and
 * every one of those calls opens with `registry_.by_name(sub.link)` — a linear scan of the child
 * registry with a string compare per slot (`child_registry_t::by_name`). The candidate optimisation
 * is a per-fan-out memo: resolve the link once and reuse the `transport_t*` across the deliveries
 * of the same write. Nothing measured what that scan costs per delivery, so the memo could only
 * ever have been argued from code reading — which is what this bench exists to stop.
 *
 * `bench_forward_demux` prices the registry scan on the FORWARD hop (one scan per inbound frame);
 * `bench_compact_delivery` prices a warm compacted delivery arriving from the wire. Neither drives
 * the PRODUCER leg, where the scan count is multiplied by the fan-out width rather than by the
 * frame rate.
 *
 * @section axes What is swept
 *
 * - **fan-out N** — remote subscribers on the producer vertex; the number of `deliver_remote`
 *   calls, and therefore of resolutions, per write.
 * - **registry width W** — children registered on the router. `by_name`'s scan is O(W).
 * - **scan position** — the subscribers' link registered FIRST among the W (the scan stops at slot
 *   0) or LAST (it walks all W). The difference between the two arms at a fixed (N, W) IS the
 *   marginal cost of the scan, isolated from everything else the leg does.
 *
 * @section instrument The reachability instrument
 *
 * Two independent counts, because a timing on a leg that was never reached is worse than no
 * timing. Both are printed per point and both are asserted, not assumed:
 *
 * 1. **Deliveries** — the destination link counts the frames handed to it. A point whose
 *    `sends != fanout` per write did not fan out the way it claims.
 * 2. **Resolutions** — the `peer` mode addresses its subscribers by a BUS PEER name rather than a
 *    child name (ADR-0044's second resolution tier), so `by_name` falls through its exact-name
 *    scan into `bus_link_t::peer_link`, which this bench implements and counts. That count is an
 *    EXACT number of `by_name` calls per write, not an inference from a slope.
 *
 * The line is broken once, deliberately: a `fanout=0` control point runs the same write against a
 * producer with no remote subscribers and must report zero on BOTH counters. A counter that still
 * moves there is measuring something other than the leg.
 *
 * Interleaved arms, batch-amortized and self-calibrating, for the same reasons the other framed
 * benches are: one delivery sits close enough to `clock_gettime` that per-op timing would measure
 * the clock. Each point reports a median across batches with the min/max range beside it, so a
 * difference smaller than the arms' spread can be read as the non-result it is.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Payload the producer writes — fixed, because the leg's cost is route and iov, not bytes.
 */
constexpr std::size_t kPayload = 64;

/**
 * @brief Distinct destination links the SPREAD mode rotates the fan-out over.
 *
 * The shared-link modes are the best case for any resolve-once scheme; a fan-out whose
 * subscribers sit on DIFFERENT links is the case that decides whether one memoized slot is
 * enough, so the sweep has to contain both.
 */
constexpr std::size_t kSpreadLinks = 4;

constexpr double kDefaultBudgetSeconds = 1.0;

/** @brief Per-point time budget; `LIBTRACER_BENCH_SECONDS` shortens a local run. */
[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

using bench::calibrate_batch;

/** @brief A link that swallows what it is handed and counts it — no I/O, no allocation. */
struct sink_link_t : tr::net::transport_t {
    std::size_t sends = 0;
    void send(std::span<const std::byte>) override { ++sends; }
    void send(std::span<const std::span<const std::byte>>) override { ++sends; }
};

/**
 * @brief A bus child exposing one peer — the EXACT resolution counter.
 *
 * `child_registry_t::by_name` resolves an exact child name first and only then asks each bus child
 * to resolve the name as an audible peer. A subscriber whose stored link is this bus's PEER name
 * therefore reaches @ref peer_link exactly once per `by_name` call, which makes @ref lookups a
 * count of resolutions rather than an estimate of them.
 */
struct peer_bus_t : tr::net::transport_t, tr::net::bus_link_t {
    sink_link_t peer;        /**< @brief The directed endpoint the peer name resolves to. */
    std::size_t lookups = 0; /**< @brief `by_name` resolutions that reached this bus. */
    void send(std::span<const std::byte>) override {}
    void send(std::span<const std::span<const std::byte>>) override {}
    [[nodiscard]] tr::net::bus_link_t* bus() override { return this; }
    void enumerate_peers(const peer_visitor_t& visit) const override { visit("p0"); }
    [[nodiscard]] tr::net::transport_t* peer_link(std::string_view name) override {
        ++lookups;
        return name == "p0" ? &peer : nullptr;
    }
    /** @brief The one peer this bus exposes, for any valid handle (#1294). */
    [[nodiscard]] std::string_view peer_name(tr::net::peer_handle_t peer_h,
                                             std::span<char>) const override {
        return peer_h.valid() ? "p0" : std::string_view{};
    }
};

/** @brief A PATH TLV over @p segs — the subscriber's stored return route. */
std::vector<std::byte> path_tlv(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A VALUE TLV of @p n payload bytes — what the producer writes. */
std::vector<std::byte> value_tlv(std::size_t n) {
    const std::vector<std::byte> payload(n, std::byte{0xAB});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload));
    return out;
}

/** @brief A borrowed view over stable bytes — the write itself allocates nothing. */
view_t borrowed(std::span<const std::byte> bytes) {
    return view_t::over(tr::view::borrow_const(bytes));
}

/** @brief How the subscribers' link sits in the registry the scan walks. */
enum class pos_t {
    FIRST,  /**< @brief Registered at slot 0 — the scan stops immediately. */
    LAST,   /**< @brief Registered at slot W-1 — the scan walks every slot. */
    PEER,   /**< @brief Not a child name at all: a bus peer, resolved after a full miss. */
    SPREAD, /**< @brief kSpreadLinks distinct destination links, round-robin over the fan-out. */
};

[[nodiscard]] const char* pos_name(pos_t p) {
    switch (p) {
        case pos_t::FIRST:
            return "reply-first";
        case pos_t::LAST:
            return "reply-last";
        case pos_t::PEER:
            return "reply-peer";
        case pos_t::SPREAD:
            return "reply-spread";
    }
    return "reply-?";
}

/**
 * @brief The median of @p v, and its min/max, in one pass over a copy.
 *
 * A median with its range beside it, because a point whose arms overlap has not shown a
 * difference — and a bare median invites reading one anyway.
 */
struct spread_t {
    std::uint64_t med = 0, lo = 0, hi = 0;
};

[[nodiscard]] spread_t spread_of(std::vector<std::uint64_t> v) {
    if (v.empty()) return {};
    std::sort(v.begin(), v.end());
    return {v[v.size() / 2], v.front(), v.back()};
}

/**
 * @brief One measured point: a write fanning out to @p fanout remote subscribers.
 *
 * @param fanout Remote subscribers on the producer (0 = the broken-line control).
 * @param width  Children registered on the router.
 * @param pos    Where the subscribers' link sits in that registry.
 */
void run_point(std::size_t fanout, std::size_t width, pos_t pos) {
    graph_t g;
    fwd_router_t router(g);

    // The registry: `width` children, with the subscribers' destination placed per `pos`.
    // Every child is a distinct heap object so no two share a slot's identity.
    std::vector<sink_link_t> decoys(width);
    peer_bus_t bus;
    std::vector<sink_link_t> targets(kSpreadLinks);
    std::vector<std::string> link_names;

    const auto add_decoys = [&](std::size_t from, std::size_t to) {
        for (std::size_t i = from; i < to; ++i)
            (void)router.add_child("net/decoy/d" + std::to_string(i), decoys[i]);
    };
    const auto add_targets = [&](std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            link_names.push_back("net/tgt/t" + std::to_string(i));
            (void)router.add_child(link_names.back(), targets[i]);
        }
    };
    switch (pos) {
        case pos_t::PEER:
            // The bus sits LAST, so an exact-name miss walks the whole table before the peer tier
            // — the shape the counter is placed in, and the most expensive real resolution.
            add_decoys(0, width);
            (void)router.add_child("net/bus/b", bus);
            link_names.emplace_back("p0");
            break;
        case pos_t::FIRST:
            add_targets(1);
            add_decoys(0, width - 1);
            break;
        case pos_t::LAST:
            add_decoys(0, width - 1);
            add_targets(1);
            break;
        case pos_t::SPREAD:
            add_decoys(0, width - kSpreadLinks);
            add_targets(kSpreadLinks);
            break;
    }
    // Deliveries are counted across every destination the mode uses, so the SPREAD mode's count
    // is the whole fan-out and not one link's share of it.
    const auto delivered = [&] {
        if (pos == pos_t::PEER) return bus.peer.sends;
        std::size_t n = 0;
        for (const sink_link_t& s : targets) n += s.sends;
        return n;
    };
    const auto reset_counts = [&] {
        bus.peer.sends = 0;
        bus.lookups = 0;
        for (sink_link_t& s : targets) s.sends = 0;
    };

    // The producer, and `fanout` remote subscriber edges on it — all addressing the SAME link,
    // which is the shape a memo would collapse. `fanout == 0` leaves the vertex bare.
    const vertex_handle_t prod = g.register_vertex(*path_t::parse("/prod"), role_t::STORED_VALUE);
    const std::vector<std::byte> route = path_tlv({"back", "to", "sub"});
    const std::vector<std::byte> sub_tlv = [] {
        std::vector<std::byte> out;
        tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, {});
        return out;
    }();
    std::size_t admitted = 0;
    for (std::size_t i = 0; i < fanout; ++i)
        if (g.subscribe_wire(prod, borrowed(sub_tlv), borrowed(route),
                             link_names[i % link_names.size()])
                .has_value())
            ++admitted;
    if (admitted != fanout) {
        std::printf("WARN mode=%s fan=%zu w=%zu admitted=%zu — the topology is not the one timed\n",
                    pos_name(pos), fanout, width, admitted);
        return;
    }

    const std::vector<std::byte> value = value_tlv(kPayload);
    const auto write_once = [&] { (void)g.write(prod, borrowed(value)); };

    for (int i = 0; i < 64; ++i) write_once();  // warm the stripe, the LKV slot and the caches

    // Reachability, counted around exactly one write and outside every timed window.
    reset_counts();
    write_once();
    const std::size_t sends = delivered();
    const std::size_t lookups = bus.lookups;

    const std::size_t batch = calibrate_batch(write_once);
    std::vector<std::uint64_t> per_batch;
    bench::Latency lat;
    const std::uint64_t t0 = bench::now_ns();
    const auto deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) write_once();
        const std::uint64_t per_op = (bench::now_ns() - a) / batch;
        lat.add(per_op);
        per_batch.push_back(per_op);
        ++batches;
        total = bench::now_ns() - t0;
    }

    const double ops = static_cast<double>(batches) * static_cast<double>(batch);
    const double writes_per_s = total == 0 ? 0.0 : ops * 1e9 / static_cast<double>(total);
    const bench::Latency::Summary s = lat.summarize();
    const spread_t sp = spread_of(per_batch);
    bench::emit("libtracer", pos_name(pos), kPayload, fanout, width, writes_per_s,
                writes_per_s * static_cast<double>(fanout), 0.0, s);
    bench::emit_tail("libtracer", pos_name(pos), kPayload, fanout, width, s);
    // The instrument line: the two counts, then the median with its range. `resolutions` is
    // meaningful only in the peer mode (the mode that routes every by_name through the counter);
    // the name-addressed modes print it as the zero it must be.
    std::printf(
        "NOTE mode=%s fan=%zu w=%zu batch=%zu batches=%zu deliveries=%zu resolutions=%zu "
        "med=%lluns lo=%lluns hi=%lluns\n",
        pos_name(pos), fanout, width, batch, batches, sends, lookups,
        static_cast<unsigned long long>(sp.med), static_cast<unsigned long long>(sp.lo),
        static_cast<unsigned long long>(sp.hi));

    // Both counters are asserted against the topology, per point.
    if (sends != fanout)
        std::printf("WARN mode=%s fan=%zu w=%zu delivered %zu times, not %zu\n", pos_name(pos),
                    fanout, width, sends, fanout);
    if (pos == pos_t::PEER && lookups != fanout)
        std::printf(
            "WARN mode=%s fan=%zu w=%zu resolved %zu times, not %zu — by_name is NOT once "
            "per delivery\n",
            pos_name(pos), fanout, width, lookups, fanout);
    if (pos != pos_t::PEER && lookups != 0)
        std::printf("WARN mode=%s fan=%zu w=%zu reached the peer tier %zu times\n", pos_name(pos),
                    fanout, width, lookups);
}

}  // namespace

int main() {
    std::printf("# bench_reply_leg — the producer fan-out's per-delivery link resolution\n");
    // BREAK THE LINE first: the same write with no remote subscribers. Both counters must read
    // zero here, so a non-zero reading below is the leg and not the harness.
    std::printf("# control: fanout=0 — both counters must read 0\n");
    run_point(0, 32, pos_t::LAST);
    run_point(0, 32, pos_t::PEER);

    for (const std::size_t width : {4U, 32U}) {
        for (const std::size_t fanout : {1U, 8U, 64U}) {
            // Interleaved: the two scan positions alternate at the same (fan, width) so a machine
            // that drifts during the sweep drifts through both arms, not into one of them.
            run_point(fanout, width, pos_t::FIRST);
            run_point(fanout, width, pos_t::LAST);
            run_point(fanout, width, pos_t::FIRST);
            run_point(fanout, width, pos_t::LAST);
        }
    }
    // A resolve-once scheme's WORST case: the same fan-out rotated over kSpreadLinks
    // destinations, so consecutive deliveries never repeat a link and nothing a single-slot memo
    // holds can be reused. Read against the shared-link LAST arm at the same (fan, width).
    for (const std::size_t fanout : {8U, 64U}) {
        run_point(fanout, 32, pos_t::LAST);
        run_point(fanout, 32, pos_t::SPREAD);
        run_point(fanout, 32, pos_t::LAST);
        run_point(fanout, 32, pos_t::SPREAD);
    }
    // The counted mode: one width, the full fan-out sweep, so the resolution count is asserted
    // against every fan-out this bench claims a per-delivery scan for.
    for (const std::size_t fanout : {1U, 8U, 64U}) run_point(fanout, 32, pos_t::PEER);
    return 0;
}
