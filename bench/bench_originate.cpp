/**
 * @file
 * @brief What a node pays to ORIGINATE a remote operation — the side no bench has measured.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Every existing forward-plane bench times a TRANSIT hop: a frame arrives on one link and
 * leaves on another. `bench_forward_demux` decomposes that hop into a 35 ns `dst` peek and an
 * 18 ns head rebuild out of 87 ns total. None of them times the node that STARTS the
 * conversation, and the originator is not the same shape as a transit hop:
 *
 *  - It has no inbound frame to read an address out of. It **writes** one, encoding the full
 *    `dst` and `src` PATH TLVs from scratch on every call.
 *  - It has no per-child receiver context, because there is no inbound child. `fwd_router.cpp`
 *    falls back to `registry_.entry_by_name(inbound_name)` — a table scan a transit hop with a
 *    wired receiver skips entirely.
 *
 * So the originator plausibly pays MORE than the transit hop that is carefully optimized, and
 * nothing measures it. This bench does, with the two arms that price the open question.
 *
 * **The question.** A resolve-once scheme (ADR-0062, and client-originated binding, #504) lets
 * a stable destination be addressed by a short minted label instead of a full path. A cache the
 * ORIGINATOR holds cannot shorten a transit node's parse — that work happens on bytes the
 * originator already sent — but it can change **what bytes it sends**, and that is the only
 * channel through which any cache reaches a parse cost. This bench measures that channel.
 *
 *  - `originate-path`  — build `FWD{op=WRITE, dst=PATH{…}, src=PATH{…}, VALUE}` and route it.
 *    What an application does today: no library API originates a remote read/write/await, so
 *    apps hand-build the frame and inject it into their own router.
 *  - `originate-label` — build `COMPACT{label, VALUE}` against a warmed binding and route it.
 *    The same operation to the same destination, addressed by a minted label.
 *
 * **The payload is rebuilt inside the timed window on purpose.** A real originator cannot cache
 * the whole frame, because the value changes on every write — only the ADDRESS is stable. Hoisting
 * frame construction out would measure a shape production never runs and would flatter the label
 * arm, since encoding a 2-byte label instead of a multi-segment PATH is exactly the work under
 * test. Both arms therefore build a fresh frame per iteration, and the ONLY difference between
 * them is how the destination is named.
 *
 * Both arms drive the installed link receiver (`deliver`), not the ctx-less `on_frame(name, …)`
 * entry, so the routing leg is the same shape `bench_forward_demux` timed and the numbers are
 * comparable to its 87 ns.
 *
 * NOT a wall-clock throughput number: delivery is synchronous on this thread into a counting
 * transport, so the reported latency is origination + routing with no socket and no thread
 * handoff.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <span>
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

/** @brief Per-point wall-clock budget in seconds; `LIBTRACER_BENCH_SECONDS` overrides. */
constexpr double kDefaultBudgetSeconds = 1.0;

[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

/** @brief <5% improvement from doubling ⇒ the clock is amortized (see bench_forward_demux). */
constexpr double kPlateau = 0.05;

/**
 * @brief The batch size at which per-op cost stops falling — CALIBRATED, not chosen.
 *
 * One operation costs tens of nanoseconds, the same order as `clock_gettime`, so timing each
 * one individually measures the clock. The right batch is a property of the host's clock, so
 * it is derived rather than hardcoded.
 */
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
 * @brief A transport that counts what it was handed — no I/O, no allocation.
 *
 * The scatter-gather `send` is overridden because the base class flattens an iov into a
 * temporary vector, which would put an allocation inside the timed window.
 */
struct capture_transport_t : transport_t {
    void deliver(std::span<const std::byte> f) { rx_.deliver_borrowed(f); }
    void send(std::span<const std::byte> f) override { sink += f.size(); }
    void send(std::span<const std::span<const std::byte>> iov) override {
        for (const auto& s : iov) sink += s.size();
    }
    /** @brief Accumulates lengths so the compiler cannot elide the egress. */
    std::size_t sink = 0;
};

/** @brief A NAME-only PATH TLV over @p segs. */
[[nodiscard]] std::vector<std::byte> path_tlv(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief A 4-byte VALUE TLV carrying @p v — the payload that changes on every write. */
void emit_value(std::vector<std::byte>& out, std::uint32_t v) {
    std::array<std::byte, 4> p{};
    for (std::size_t i = 0; i < 4; ++i) p[i] = static_cast<std::byte>((v >> (8 * i)) & 0xFFu);
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
}

/**
 * @brief Build `FWD{op=WRITE, dst, src, VALUE(v)}` — the frame an originator hand-builds today.
 *
 * `dst` and `src` are handed in already encoded so the arm measures what an originator can
 * REASONABLY hoist (the segment bytes are stable) while still paying to assemble them into the
 * frame — which it cannot hoist, because the payload changes. Encoding the segments from
 * `string_view`s inside the loop instead would make the arm look worse than the design it
 * represents.
 */
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

/** @brief Build `COMPACT{label, VALUE(v)}` — the same operation, addressed by a minted label. */
[[nodiscard]] std::vector<std::byte> build_compact(std::uint16_t label, std::uint32_t v) {
    std::vector<std::byte> payload;
    emit_value(payload, v);
    return tr::net::encode_compact(label, payload);
}

/**
 * @brief Time one arm: build a fresh frame naming @p mode's address, then route it.
 * @param label_arm Address by minted label (true) or by full path (false).
 * @return p50 nanoseconds per originated operation.
 */
[[nodiscard]] std::uint64_t run_arm(const char* mode, bool label_arm) {
    graph_t g;
    (void)g.register_vertex(*tr::graph::path_t::parse("/sink"), tr::graph::role_t::STORED_VALUE);
    fwd_router_t router{g};
    capture_transport_t self;
    capture_transport_t out;
    // "self" is the link an originating application injects through; "out" carries the hop.
    router.add_child("net/ws-server/self", self);
    router.add_child("net/ws-client/out", out);

    // The destination this node keeps writing to, and the return route for its reply.
    const std::vector<std::byte> dst = path_tlv({"net", "ws-client", "out", "sensor", "temp"});
    const std::vector<std::byte> src = path_tlv({"reply"});

    // Bind a label for the same destination, and WARM it — the cold first resolve is exactly
    // the frame this bench is not about.
    constexpr std::uint16_t kLabel = 0x0042;
    self.deliver(
        tr::net::encode_advertise(kLabel, path_tlv({"net", "ws-client", "out", "sensor", "temp"})));
    for (int i = 0; i < 64; ++i) self.deliver(build_compact(kLabel, 0));

    std::uint32_t v = 0;
    const auto op = [&] {
        ++v;  // the value changes every write, which is why the frame cannot be hoisted
        const std::vector<std::byte> frame =
            label_arm ? build_compact(kLabel, v) : build_fwd(dst, src, v);
        self.deliver(frame);
    };

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

    const double ops = static_cast<double>(batches) * static_cast<double>(batch);
    const double per_s = total == 0 ? 0.0 : ops * 1e9 / static_cast<double>(total);
    const std::size_t bytes =
        label_arm ? build_compact(kLabel, 1).size() : build_fwd(dst, src, 1).size();
    const bench::Latency::Summary s = lat.summarize();
    bench::emit("libtracer", mode, bytes, 1, 1, per_s, per_s, 0.0, s);
    std::printf("NOTE mode=%s frame_bytes=%zu batch=%zu sink=%zu\n", mode, bytes, batch,
                self.sink + out.sink);
    return s.p50;
}

}  // namespace

int main() {
    // Both arms in ONE binary, alternating, because two builds leave layout, allocator state
    // and thermal drift in the comparison — and on these shapes those are the same size as the
    // effect being measured.
    std::uint64_t path_ns = 0;
    std::uint64_t label_ns = 0;
    for (int round = 0; round < 3; ++round) {
        path_ns = run_arm("originate-path", false);
        label_ns = run_arm("originate-label", true);
    }

    const double delta = static_cast<double>(path_ns) - static_cast<double>(label_ns);
    std::printf("\n%-22s %s\n", "arm", "p50_ns");
    std::printf("%-22s %llu\n", "originate-path", static_cast<unsigned long long>(path_ns));
    std::printf("%-22s %llu\n", "originate-label", static_cast<unsigned long long>(label_ns));
    std::printf(
        "\nSUMMARY addressing a stable destination by minted label instead of full path\n"
        "        changes an originated operation by %.0f ns (%.2fx).\n"
        "        Compare against bench_forward_demux: an 87 ns transit hop, of which the\n"
        "        dst peek is 35 ns. This arm measures the ORIGINATE side, which nothing\n"
        "        else in the suite does.\n",
        delta, label_ns == 0 ? 0.0 : static_cast<double>(path_ns) / static_cast<double>(label_ns));
    return 0;
}
