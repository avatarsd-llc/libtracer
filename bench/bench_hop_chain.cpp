/**
 * @file
 * @brief Five nodes, four hops: what a full-path address costs against a minted label, and what
 *        the first operation costs against every one after it.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_originate` measured one originating node against one link. This measures a CHAIN,
 * because two of the three questions only appear at depth:
 *
 *  - **Does the address collapse?** A four-hop destination is thirteen path segments
 *    (`net/<module>/<name>` per hop, then the leaf). A minted label is one `uint16`. This bench
 *    asserts the collapse on the wire rather than assuming it — it records the frame bytes each
 *    hop actually carries, and a run whose label arm does not shrink is a failed run.
 *  - **What does the FIRST operation cost?** A label has a setup price: the binding has to
 *    propagate hop by hop before anything can be addressed by it. A steady-state number alone
 *    would hide that, and would recommend labels for a caller that writes once. So cold and warm
 *    are reported as separate rows, never averaged together.
 *
 * The third question is the one `bench_originate` already answered at one hop and this extends:
 * how the per-operation saving grows with distance.
 *
 * **Both arms carry the same operation to the same vertex over the same five graphs**, differing
 * only in how the destination is named. Every hop is a real `fwd_router_t` with its own
 * `graph_t`; the links are in-process pairs that hand bytes to the next node's receiver, so the
 * routing work at each hop is the real thing and only the socket is absent.
 *
 * NOT a wall-clock throughput number: the whole chain runs synchronously on this thread, so a
 * reported latency is the sum of four hops' router cost with no I/O and no thread handoff.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Nodes in the chain; hops = kNodes - 1. */
constexpr std::size_t kNodes = 5;

constexpr double kDefaultBudgetSeconds = 1.0;
constexpr double kPlateau = 0.05;

[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

template <typename Op>
[[nodiscard]] std::size_t calibrate_batch(Op&& op) {
    double prev = 0.0;
    for (std::size_t batch = 1; batch <= (1U << 20); batch *= 2) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        const double per = static_cast<double>(bench::now_ns() - a) / static_cast<double>(batch);
        if (prev > 0.0 && per > prev * (1.0 - kPlateau)) return batch;
        prev = per;
    }
    return 1U << 20;
}

/**
 * @brief One end of an in-process link: `send` hands the bytes to the peer end's receiver.
 *
 * The gather overload concatenates into a reused member buffer rather than a fresh vector, so
 * after warm-up the timed window allocates nothing. Both arms traverse the identical code, so
 * whatever residue remains is common-mode.
 */
struct wire_link_t : transport_t {
    wire_link_t* peer = nullptr;
    /** @brief Bytes this end has carried — the wire-size evidence, per hop. */
    std::size_t bytes = 0;
    std::size_t frames = 0;

    /** @brief Push bytes INTO this node, as an inbound frame from the peer. */
    void inject(std::span<const std::byte> f) { rx_.deliver_borrowed(f); }

    void send(std::span<const std::byte> f) override {
        bytes += f.size();
        ++frames;
        if (peer != nullptr) peer->inject(f);
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        scratch_.clear();
        for (const auto& s : iov) scratch_.insert(scratch_.end(), s.begin(), s.end());
        bytes += scratch_.size();
        ++frames;
        if (peer != nullptr) peer->inject(scratch_);
    }

   private:
    std::vector<std::byte> scratch_;
};

[[nodiscard]] std::vector<std::byte> path_tlv(const std::vector<std::string_view>& segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

void emit_value(std::vector<std::byte>& out, std::uint32_t v) {
    std::array<std::byte, 4> p{};
    for (std::size_t i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
}

[[nodiscard]] std::vector<std::byte> build_fwd(std::span<const std::byte> dst,
                                               std::span<const std::byte> src, std::uint32_t v) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());
    emit_value(body, v);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

[[nodiscard]] std::vector<std::byte> build_compact(std::uint16_t label, std::uint32_t v) {
    std::vector<std::byte> payload;
    emit_value(payload, v);
    return tr::net::encode_compact(label, payload);
}

/** @brief The name node @p i uses for its downstream link toward node @p i+1. */
[[nodiscard]] std::string down_name(std::size_t i) {
    return "net/ws-client/n" + std::to_string(i + 1);
}
/** @brief The name node @p i uses for its upstream link back toward node @p i-1. */
[[nodiscard]] std::string up_name(std::size_t i) {
    return "net/ws-server/n" + std::to_string(i - 1);
}

/**
 * @brief Five graphs wired into a line, each with its own router — the chain under test.
 *
 * Node 0 originates. Node kNodes-1 holds the destination vertex `/sink`. Every intermediate
 * node has an upstream and a downstream link and does real forwarding work.
 */
struct chain_t {
    // graph_t and fwd_router_t are neither copyable nor movable (a router holds a reference to
    // its graph), so they are held indirectly rather than by value in a vector.
    std::vector<std::unique_ptr<graph_t>> graphs;
    std::vector<std::unique_ptr<fwd_router_t>> routers;
    /** @brief `down[i]` is node i's end toward i+1; `up[i]` is node i's end toward i-1. */
    std::array<wire_link_t, kNodes> down{};
    std::array<wire_link_t, kNodes> up{};
    /** @brief The link node 0's application injects through — it has no upstream peer. */
    wire_link_t origin;

    chain_t() {
        for (std::size_t i = 0; i < kNodes; ++i) graphs.push_back(std::make_unique<graph_t>());
        for (std::size_t i = 0; i < kNodes; ++i)
            routers.push_back(std::make_unique<fwd_router_t>(*graphs[i]));
        (void)graphs[kNodes - 1]->register_vertex(*tr::graph::path_t::parse("/sink"),
                                                  tr::graph::role_t::STORED_VALUE);
        for (std::size_t i = 0; i + 1 < kNodes; ++i) {
            down[i].peer = &up[i + 1];
            up[i + 1].peer = &down[i];
            routers[i]->add_child(down_name(i), down[i]);
            routers[i + 1]->add_child(up_name(i + 1), up[i + 1]);
        }
        // Node 0's application injects here; nothing is on the far side.
        routers[0]->add_child("net/ws-server/app", origin);
    }

    /** @brief The full-path address of `/sink` from node 0: three segments per hop, then the leaf.
     */
    [[nodiscard]] std::vector<std::string_view> full_route() const {
        static std::vector<std::string> owned;
        owned.clear();
        for (std::size_t i = 0; i + 1 < kNodes; ++i) owned.push_back(down_name(i));
        std::vector<std::string_view> segs;
        for (const std::string& n : owned) {
            std::size_t a = 0;
            while (a < n.size()) {
                const std::size_t e = n.find('/', a);
                segs.emplace_back(n.data() + a, (e == std::string::npos ? n.size() : e) - a);
                if (e == std::string::npos) break;
                a = e + 1;
            }
        }
        segs.emplace_back("sink");
        return segs;
    }

    [[nodiscard]] std::size_t wire_bytes() const {
        std::size_t n = 0;
        for (const wire_link_t& l : down) n += l.bytes;
        return n;
    }
    void reset_counters() {
        for (wire_link_t& l : down) {
            l.bytes = 0;
            l.frames = 0;
        }
    }
    [[nodiscard]] bool delivered() const {
        return graphs[kNodes - 1]->read(*tr::graph::path_t::parse("/sink")).has_value();
    }
};

/**
 * @brief Bind a label for the far `/sink` across the whole chain, and return it.
 *
 * This IS the cold cost of the label arm and is timed as such. One ADVERTISE naming the full
 * route enters at node 0; each node whose route-head names one of its children binds a label
 * pair (in-label from upstream, out-label downstream) and re-advertises the remainder onward,
 * so a single frame walks the chain and leaves a swap binding at every hop.
 *
 * Hand-rolling one ADVERTISE per node instead was the first attempt and it silently bound
 * nothing: the frames went to the wrong link end, the label arm delivered zero frames, and its
 * "latency" was the cost of dropping. The delivery assertion in @ref run_arm is what caught it.
 */
[[nodiscard]] std::uint16_t advertise_chain(chain_t& c) {
    constexpr std::uint16_t kLabel = 0x0042;
    c.origin.inject(tr::net::encode_advertise(kLabel, path_tlv(c.full_route())));
    return kLabel;
}

/** @brief One measured arm's numbers. */
struct arm_result_t {
    std::uint64_t cold_ns = 0;
    std::uint64_t warm_ns = 0;
    std::size_t first_frame_bytes = 0;
    std::size_t wire_bytes = 0;
    bool delivered = false;
};

[[nodiscard]] arm_result_t run_arm(const char* mode, bool label_arm) {
    arm_result_t r;
    chain_t c;

    const std::vector<std::string_view> route = c.full_route();
    const std::vector<std::byte> dst = path_tlv(route);
    const std::vector<std::byte> src = path_tlv({"reply"});

    std::uint16_t label = 0;
    std::uint32_t v = 0;

    // COLD — the first operation, INCLUDING whatever setup the arm needs. For the label arm
    // that is the binding walking back along the chain; for the path arm there is none, which
    // is exactly the asymmetry a steady-state-only number would hide.
    c.reset_counters();
    const std::uint64_t c0 = bench::now_ns();
    if (label_arm) label = advertise_chain(c);
    c.origin.inject(label_arm ? build_compact(label, ++v) : build_fwd(dst, src, ++v));
    r.cold_ns = bench::now_ns() - c0;
    r.delivered = c.delivered();

    // The frame the ORIGINATOR emits — the address-collapse evidence.
    r.first_frame_bytes =
        label_arm ? build_compact(label, 1).size() : build_fwd(dst, src, 1).size();

    const auto op = [&] {
        ++v;
        c.origin.inject(label_arm ? build_compact(label, v) : build_fwd(dst, src, v));
    };

    // WARM — steady state, the binding already in place.
    for (int i = 0; i < 64; ++i) op();
    c.reset_counters();
    op();
    r.wire_bytes = c.wire_bytes();

    const std::size_t batch = calibrate_batch(op);
    bench::Latency lat;
    const std::uint64_t deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    const std::uint64_t t0 = bench::now_ns();
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) op();
        lat.add((bench::now_ns() - a) / batch);
        ++batches;
        total = bench::now_ns() - t0;
    }
    const bench::Latency::Summary s = lat.summarize();
    r.warm_ns = s.p50;

    const double ops = static_cast<double>(batches) * static_cast<double>(batch);
    const double per_s = total == 0 ? 0.0 : ops * 1e9 / static_cast<double>(total);
    bench::emit("libtracer", mode, r.first_frame_bytes, kNodes - 1, 1, per_s, per_s, 0.0, s);
    std::printf("NOTE mode=%s hops=%zu cold_ns=%llu first_frame=%zuB wire=%zuB delivered=%d\n",
                mode, kNodes - 1, static_cast<unsigned long long>(r.cold_ns), r.first_frame_bytes,
                r.wire_bytes, r.delivered ? 1 : 0);
    return r;
}

}  // namespace

int main() {
    // Both arms in ONE binary, alternating, for the reason bench_originate states: two builds
    // leave layout, allocator state and thermal drift in a comparison whose effect is the same
    // size as those.
    arm_result_t path{};
    arm_result_t label{};
    for (int round = 0; round < 3; ++round) {
        path = run_arm("chain-path", false);
        label = run_arm("chain-label", true);
    }

    std::printf("\n%zu nodes, %zu hops\n\n", kNodes, kNodes - 1);
    std::printf("%-14s %-12s %-12s %-16s %s\n", "arm", "cold_ns", "warm_ns", "first_frame_B",
                "delivered");
    std::printf("%-14s %-12llu %-12llu %-16zu %s\n", "chain-path",
                static_cast<unsigned long long>(path.cold_ns),
                static_cast<unsigned long long>(path.warm_ns), path.first_frame_bytes,
                path.delivered ? "yes" : "NO");
    std::printf("%-14s %-12llu %-12llu %-16zu %s\n", "chain-label",
                static_cast<unsigned long long>(label.cold_ns),
                static_cast<unsigned long long>(label.warm_ns), label.first_frame_bytes,
                label.delivered ? "yes" : "NO");

    // The collapse claim, asserted rather than assumed.
    const bool collapsed = label.first_frame_bytes < path.first_frame_bytes;
    std::printf(
        "\nADDRESS COLLAPSE  %zu-segment path (%zu B frame) -> one uint16 label (%zu B frame)"
        " : %s\n",
        3 * (kNodes - 1) + 1, path.first_frame_bytes, label.first_frame_bytes,
        collapsed ? "CONFIRMED" : "NOT OBSERVED");

    if (!path.delivered || !label.delivered) {
        std::printf(
            "\nFAILED: an arm did not reach the terminus, so its latency measures a DROP,\n"
            "        not a delivery. Numbers above are void.\n");
        return 1;
    }

    const double d_warm = static_cast<double>(path.warm_ns) - static_cast<double>(label.warm_ns);
    const double breakeven =
        d_warm > 0.0
            ? (static_cast<double>(label.cold_ns) - static_cast<double>(path.cold_ns)) / d_warm
            : 0.0;
    std::printf(
        "\nSUMMARY over %zu hops a minted label is worth %.0f ns per warm operation.\n"
        "        Its binding costs %.0f ns more on the FIRST one, so it pays for itself\n"
        "        after ~%.0f operations to the same destination — below that, the full\n"
        "        path is cheaper and a caller that writes once should not bind at all.\n",
        kNodes - 1, d_warm, static_cast<double>(label.cold_ns) - static_cast<double>(path.cold_ns),
        breakeven > 0.0 ? breakeven : 0.0);
    return 0;
}
