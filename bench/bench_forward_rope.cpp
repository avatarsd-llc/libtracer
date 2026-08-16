/**
 * @file
 * @brief What a forward hop costs when the frame arrives as a ROPE — the arm no bench reached.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_forward_demux` drives `on_frame_impl`, the SPAN arm: one contiguous buffer. A
 * transport that delivers ropes (ADR-0053 §5) takes `on_frame_rope` instead, where the frame's
 * TLV headers may STRADDLE link boundaries and every peek is a walk with stitching rather than
 * a pointer add. Nothing measured that path, so changes to it could only be argued
 * structurally — which is exactly what happened to the duplicate-`dst`-peek removal (#666),
 * merged on a code-reading argument with its magnitude unknown (#668).
 *
 * ## The axis
 *
 * **Link count**, at a fixed registry size and a fixed frame. A one-link rope is the control:
 * same bytes, same route, same registry, but nothing straddles, so it isolates the cost of the
 * rope shape itself from the cost of the routing decision. Sweeping to one-link-per-byte gives
 * the worst case a real reassembly path can produce.
 *
 * The cut points are spread EVENLY across the frame rather than placed to hit header
 * boundaries. Placing them adversarially would measure a case a transport does not generate;
 * spreading them is what an MTU-sized or pbuf-chained arrival actually looks like, and at high
 * link counts every header straddles anyway.
 *
 * ## What this is not
 *
 * Not a throughput number: the hop is driven synchronously on this thread through the inbound
 * link's own receiver, so what is reported is router cost with no socket, no thread handoff and
 * no egress I/O. And the rope's per-link segments are built ONCE, outside the timed window —
 * a rope-delivering transport owns its buffers already, so charging their allocation to the hop
 * would measure the fixture rather than the router.
 *
 * Emits one RESULT row per link count in bench_common's shared format, `fanout` carrying the
 * link count, so collate.py and the perf history pick it up unchanged.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Accumulates egress lengths so the compiler cannot elide the send. */
std::size_t sink = 0;

/** @brief Seconds per measured point (env-overridable), as bench_forward_demux defines it. */
[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return 1.0;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : 1.0;
}

/** @brief A transport that only counts what it was handed — no I/O, no allocation. */
struct capture_transport_t : transport_t {
    std::size_t sends = 0;
    void send(std::span<const std::byte> f) override {
        ++sends;
        sink += f.size();
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        ++sends;
        for (const auto& s : iov) sink += s.size();
    }
};

/**
 * @brief An inbound link that delivers ROPES (ADR-0053 §5) — what puts the hop on the arm
 *        under test.
 */
struct rope_transport_t : capture_transport_t {
    [[nodiscard]] bool delivers_ropes() const override { return true; }
    /** @brief Push an inbound rope up this link's installed receiver, as a real one does. */
    void deliver(tr::view::rope_t frame) { rx_.deliver_rope(std::move(frame)); }
};

void emit_path(std::vector<std::byte>& out, std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) {
        tr::wire::emit_tlv(
            body, type_t::NAME, opt_t{},
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(s.data()), s.size()));
    }
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
}

std::vector<std::byte> make_fwd(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src,
                                std::span<const std::byte> payload) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    emit_path(body, dst);
    emit_path(body, src);
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, payload);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/** @brief A one-segment-per-link rope over @p bytes, split into @p links even pieces. */
[[nodiscard]] tr::view::rope_t rope_of(std::span<const std::byte> bytes, std::size_t links) {
    tr::view::rope_t r;
    if (links == 0) links = 1;
    if (links > bytes.size()) links = bytes.size();
    std::size_t at = 0;
    for (std::size_t i = 0; i < links; ++i) {
        const std::size_t end = bytes.size() * (i + 1) / links;
        if (end > at) {
            tr::view::segment_ptr_t seg = tr::view::heap_alloc(end - at);
            std::memcpy(seg->bytes.data(), bytes.data() + at, end - at);
            r.append(tr::view::view_t::over(std::move(seg)));
            at = end;
        }
    }
    return r;
}

/**
 * @brief Time one rope forward hop whose frame is split across @p links segments.
 *
 * A frame arrives on "in" addressed to "out": the hop resolves the mount, strips it from
 * `dst`, prepends the inbound run to `src`, and sends on "out". No terminus, no local vertex.
 */
void run_point(std::size_t links) {
    graph_t graph;
    fwd_router_t router(graph);
    rope_transport_t in_link;
    capture_transport_t out_link;

    // A small, fixed registry. The scan is NOT the axis here — bench_forward_demux owns that —
    // so the target is registered first and the descent hits on its first candidate. What
    // varies is only the rope's shape.
    router.add_child("net/ws-client/out", out_link);
    router.add_child("net/ws-server/in", in_link);

    const std::byte payload[4] = {std::byte{0xDE}, std::byte{0xAD}, std::byte{0xBE},
                                  std::byte{0xEF}};
    const std::vector<std::byte> frame =
        make_fwd({"net", "ws-client", "out", "sensor", "temp"}, {"reply"},
                 std::span<const std::byte>(payload, 4));

    // Built ONCE, outside the timed window. A rope-delivering transport already owns its
    // buffers; charging their allocation to the hop would measure this fixture, not the router.
    // The rope is copied per hop (a refcount bump per link) because deliver_rope consumes it —
    // that copy is what a real transport's handoff costs too.
    const tr::view::rope_t proto = rope_of(frame, links);
    const auto hop = [&] { in_link.deliver(proto); };

    // Window-floored, NOT plateau-calibrated (#1358): the plateau rule compares two timed
    // quantities, so a disturbed reading latches a batch of 2 or 4 where 128 was due, and this
    // point reads ~8% high at those batches. A window floor makes the batch a function of the
    // hop's own cost, so two executions of the same binary agree.
    const std::size_t batch = bench::calibrate_batch_for_window(hop);

    bench::Latency lat;
    const std::uint64_t deadline_ns = static_cast<std::uint64_t>(budget_seconds() * 1e9);
    const std::uint64_t t0 = bench::now_ns();
    std::size_t batches = 0;
    std::uint64_t total = 0;
    while (total < deadline_ns) {
        const std::uint64_t a = bench::now_ns();
        for (std::size_t i = 0; i < batch; ++i) hop();
        lat.add((bench::now_ns() - a) / batch);
        ++batches;
        total = bench::now_ns() - t0;
    }

    const double hops = static_cast<double>(batches) * static_cast<double>(batch);
    const double hops_per_s = total == 0 ? 0.0 : hops * 1e9 / static_cast<double>(total);
    const bench::Latency::Summary s = lat.summarize();
    bench::emit("libtracer", "fwd-rope-hop", frame.size(), links, proto.link_count(), hops_per_s,
                hops_per_s, 0.0, s);

    std::printf("NOTE links=%zu (rope has %zu) batch=%zu samples=%zu\n", links, proto.link_count(),
                batch, batches);
    // A hop that forwarded NOTHING measures the reject path, which would look like a win.
    if (out_link.sends == 0) std::printf("WARN links=%zu forwarded NOTHING\n", links);
}

}  // namespace

int main() {
    std::printf("FWD forward hop over a MULTI-LINK ROPE (on_frame_rope; #668):\n");
    for (const std::size_t links : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8},
                                    std::size_t{16}, std::size_t{32}, std::size_t{64}}) {
        run_point(links);
    }
    std::printf("\n(sink=%zu)\n", sink);
    return 0;
}
