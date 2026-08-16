/**
 * @file
 * @brief What does the transport iovec spill actually COST, and how often is it reached?
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `bench_transport_iov` answered WHERE the spill sits (17 caller spans). It did not answer the
 * two questions a decision needs: how often a real forward hop reaches that width, and what one
 * spill costs when it happens. Without both, "fix the spill" is an unpriced item.
 *
 * @section arms What this measures
 *
 *  - **A — span census.** The REAL rope forward arm (`on_frame_rope`), swept over inbound rope
 *    link count, recording the egress span count the router hands the transport. This is the
 *    frequency question, measured rather than argued: the span arm is hard-capped at
 *    `kFwdMaxIov` (9) and DROPS above it, so only a rope ingress can ever reach 17.
 *  - **B — spill point.** The `prefixed_iov_t` shape from `transport_tcp.cpp`, replicated here
 *    so the allocation can be counted and timed without a syscall in the way. Confirms the
 *    width at which the first `operator new` fires.
 *  - **C — isolated gather latency.** Interleaved A/B of that shape at 16 spans (inline) vs 17
 *    (spill), with a **control** pair at 8 vs 9 spans — both inline, same +1 span delta, so the
 *    control isolates the marginal per-span cost from the allocation.
 *  - **D — real transport latency.** The same 16-vs-17 A/B through a live
 *    `udp_transport_t::send(iov)` over loopback, so the allocation is priced against the
 *    syscall it actually sits behind. Same 8-vs-9 control.
 *  - **E — the always-allocate path.** `transport_t::send(iov)`'s DEFAULT (flatten into a
 *    `std::vector<std::byte>`, then `send(span)`) is what `transport_can` and any embedder
 *    transport that does not override the gather gets: one allocation plus a full payload copy
 *    on EVERY frame, at every width. Priced against the inline gather so the two items can be
 *    ranked against each other rather than only against zero.
 *
 * @section discipline Measurement discipline
 *
 * Arms C, D and E interleave (base, cand, base, cand, ...) across `kReps` repetitions, report
 * the MEDIAN, and print min/max so an overlap check can be read off directly. A median ratio
 * alone is not a result: a claim is made only when the arms' ranges are disjoint.
 */

#include <sys/uio.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "bench_common.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_frame_view.hpp"
#include "libtracer/graph.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"

namespace {

using tr::graph::graph_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Repetitions per interleaved arm. The discipline floor is 9; 13 leaves margin. */
constexpr std::size_t kReps = 13;

/** @brief Accumulator the optimizer cannot see through. */
volatile std::size_t g_sink = 0;

/** @brief One arm's per-op timings across the repetitions, in nanoseconds. */
struct arm_t {
    std::string name;
    std::vector<double> per_op;

    [[nodiscard]] double median() {
        std::sort(per_op.begin(), per_op.end());
        return per_op[per_op.size() / 2];
    }
    [[nodiscard]] double lo() const { return *std::min_element(per_op.begin(), per_op.end()); }
    [[nodiscard]] double hi() const { return *std::max_element(per_op.begin(), per_op.end()); }
};

/**
 * @brief Print two interleaved arms and state whether their ranges are DISJOINT.
 *
 * A median ratio is reported for scale, but the verdict line is the overlap check: unless
 * `min(cand) > max(base)` or `max(cand) < min(base)`, the honest answer is NO CLAIM.
 */
void verdict(const char* what, arm_t& base, arm_t& cand) {
    const double bm = base.median();
    const double cm = cand.median();
    std::printf("\n%s\n", what);
    std::printf("  %-22s median %8.2f ns   [min %8.2f , max %8.2f]  n=%zu\n", base.name.c_str(), bm,
                base.lo(), base.hi(), base.per_op.size());
    std::printf("  %-22s median %8.2f ns   [min %8.2f , max %8.2f]  n=%zu\n", cand.name.c_str(), cm,
                cand.lo(), cand.hi(), cand.per_op.size());
    const bool disjoint = cand.lo() > base.hi() || cand.hi() < base.lo();
    std::printf("  delta median %+.2f ns (%.3fx)   ranges %s\n", cm - bm, bm == 0 ? 0.0 : cm / bm,
                disjoint ? "DISJOINT -> claim allowed" : "OVERLAP -> NO CLAIM");
}

/* ------------------------------------------------------------------ arm A: span census */

/** @brief Records the egress SPAN COUNT of every frame the router hands it. */
struct census_transport_t : transport_t {
    std::vector<std::size_t> widths;
    void send(std::span<const std::byte> f) override {
        widths.push_back(1);
        g_sink += f.size();
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        widths.push_back(iov.size());
        for (const auto& s : iov) g_sink += s.size();
    }
};

/** @brief An inbound link on the ROPE arm — the only arm that can exceed `kFwdMaxIov`. */
struct rope_in_t : census_transport_t {
    [[nodiscard]] bool delivers_ropes() const override { return true; }
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

/** @brief A rope over @p bytes cut into fixed-size links of @p link_bytes — the shape a
 *         reassembling transport produces (CAN: one link per 8-byte data field). */
[[nodiscard]] tr::view::rope_t rope_by_link_size(std::span<const std::byte> bytes,
                                                 std::size_t link_bytes) {
    tr::view::rope_t r;
    if (link_bytes == 0) link_bytes = bytes.size();
    for (std::size_t at = 0; at < bytes.size(); at += link_bytes) {
        const std::size_t n = std::min(link_bytes, bytes.size() - at);
        tr::view::segment_ptr_t seg = tr::view::heap_alloc(n);
        std::memcpy(seg->bytes.data(), bytes.data() + at, n);
        r.append(tr::view::view_t::over(std::move(seg)));
    }
    return r;
}

/** @brief One census point: the egress span count AND the router cost of the hop that made it. */
struct census_row_t {
    std::size_t spans = 0;
    std::size_t links = 0;
    double hop_ns = 0.0;
};

/**
 * @brief Forward a rope frame of @p payload_bytes cut at @p link_bytes and report both the
 *        egress span count and the median per-hop router cost.
 *
 * The hop cost is the DENOMINATOR the spill has to be read against: a frame wide enough to
 * spill is, by construction, a frame whose rope the router already walked link by link.
 * Reporting the spill without it would price a term against zero instead of against the work
 * it rides on.
 */
[[nodiscard]] census_row_t census_point(std::size_t payload_bytes, std::size_t link_bytes) {
    graph_t graph;
    fwd_router_t router(graph);
    rope_in_t in_link;
    census_transport_t out_link;
    router.add_child("net/ws-client/out", out_link);
    router.add_child("net/ws-server/in", in_link);

    const std::vector<std::byte> payload(payload_bytes, std::byte{0x5A});
    const std::vector<std::byte> frame = make_fwd({"net", "ws-client", "out", "sensor", "temp"},
                                                  {"reply"}, std::span<const std::byte>(payload));
    // Built ONCE outside the timed loop: a rope-delivering transport owns its buffers already,
    // so charging their allocation to the hop would measure the fixture, not the router.
    const tr::view::rope_t proto = rope_by_link_size(frame, link_bytes);
    in_link.deliver(proto);
    if (out_link.widths.empty()) return census_row_t{0, proto.link_count(), 0.0};

    constexpr std::size_t kBatch = 2000;
    std::vector<double> reps;
    for (std::size_t r = 0; r < kReps; ++r) {
        const std::uint64_t t = bench::now_ns();
        for (std::size_t i = 0; i < kBatch; ++i) in_link.deliver(proto);
        reps.push_back(static_cast<double>(bench::now_ns() - t) / static_cast<double>(kBatch));
        out_link.widths.clear();  // keep the census vector from growing into the measurement
    }
    std::sort(reps.begin(), reps.end());
    // Span count comes from the caller's un-timed pass; this pass reports only the hop cost.
    return census_row_t{std::size_t{0}, proto.link_count(), reps[reps.size() / 2]};
}

void arm_a_span_census() {
    std::printf(
        "\n== A. EGRESS SPAN CENSUS + HOP COST - the REAL rope forward arm (on_frame_rope) ==\n"
        "   frame = FWD dst[net/ws-client/out/sensor/temp] src[reply] + payload\n"
        "   link_bytes 0/1500 = a stream/datagram arrival. length_prefix_framer allocates ONE\n"
        "                       exactly-sized segment per frame, so tcp/udp/ws-unfragmented\n"
        "                       ingress is a ONE-link rope whatever the payload size.\n"
        "   link_bytes 8      = classic-CAN reassembly (one rope link per CAN data field), and\n"
        "                       equally an 8-byte-per-fragment WebSocket continuation message.\n"
        "   link_bytes 1      = ADVERSARIAL: a WS peer choosing one-byte continuation frames.\n"
        "                       transport_ws's assembler appends one owning link per fragment\n"
        "                       with no fragment-count cap, so the peer picks the link count.\n\n");
    std::printf("   %-9s %-8s %-11s %-7s %-13s %-11s %s\n", "payload", "frame_B", "link_bytes",
                "links", "egress_spans", "hop_ns", "vs 17 spans");
    for (const std::size_t payload :
         {std::size_t{4}, std::size_t{16}, std::size_t{32}, std::size_t{48}, std::size_t{64},
          std::size_t{256}, std::size_t{1024}}) {
        for (const std::size_t lb :
             {std::size_t{0}, std::size_t{1500}, std::size_t{64}, std::size_t{8}, std::size_t{1}}) {
            // Two passes: one un-timed for the span count (the census transport records it),
            // one timed. Kept separate so the recording vector never sits in the timed loop.
            graph_t g2;
            fwd_router_t r2(g2);
            rope_in_t in2;
            census_transport_t out2;
            r2.add_child("net/ws-client/out", out2);
            r2.add_child("net/ws-server/in", in2);
            const std::vector<std::byte> pl(payload, std::byte{0x5A});
            const std::vector<std::byte> fr =
                make_fwd({"net", "ws-client", "out", "sensor", "temp"}, {"reply"},
                         std::span<const std::byte>(pl));
            in2.deliver(rope_by_link_size(fr, lb));
            const std::size_t w = out2.widths.empty() ? 0 : out2.widths.front();

            const census_row_t row = census_point(payload, lb);
            std::printf("   %-9zu %-8zu %-11zu %-7zu %-13zu %-11.1f %s\n", payload, fr.size(), lb,
                        row.links, w, row.hop_ns,
                        w == 0    ? "DROPPED (no egress)"
                        : w >= 17 ? "SPILLS"
                                  : "inline");
        }
    }
    std::printf(
        "\n   NOTE the CONTIGUOUS (span) arm cannot appear here: fwd_router caps it at\n"
        "        kFwdMaxIov = %zu and DROPS above, so it can never reach 17.\n",
        tr::net::kFwdMaxIov);
}

/* --------------------------------------------- arms B/C: the prefixed_iov_t shape, isolated */

/** @brief The `transport_tcp.cpp` anonymous-namespace gather, replicated verbatim in shape so
 *         it can be counted and timed with no syscall in the way (that struct is TU-private). */
struct prefixed_iov_replica_t {
    static constexpr std::size_t kMaxInlineIov = 16;
    std::array<std::byte, 4> prefix;
    std::array<::iovec, kMaxInlineIov + 1> inline_vec;
    std::vector<::iovec> heap_vec;
    ::iovec* vec = nullptr;
    std::size_t n = 0;

    explicit prefixed_iov_replica_t(std::span<const std::span<const std::byte>> iov) {
        std::size_t total = 0;
        for (const std::span<const std::byte>& s : iov) total += s.size();
        tr::detail::store_le(prefix, static_cast<std::uint32_t>(total));
        vec = inline_vec.data();
        if (iov.size() + 1 > inline_vec.size()) {
            heap_vec.resize(iov.size() + 1);
            vec = heap_vec.data();
        }
        vec[0] = ::iovec{prefix.data(), prefix.size()};
        n = 1;
        for (const std::span<const std::byte>& s : iov) {
            if (s.empty()) continue;
            vec[n++] = ::iovec{const_cast<std::byte*>(s.data()), s.size()};
        }
    }
};

thread_local std::size_t g_allocs = 0;
thread_local std::size_t g_alloc_bytes = 0;
thread_local bool g_counting = false;

void* counted_alloc(std::size_t size) {
    if (g_counting) {
        ++g_allocs;
        g_alloc_bytes += size;
    }
    return std::malloc(size == 0 ? 1 : size);
}

/** @brief Build @p width spans over @p store, reusing the caller's vector (no fixture alloc). */
void fill_iov(std::vector<std::span<const std::byte>>& iov,
              const std::vector<std::vector<std::byte>>& store, std::size_t width) {
    iov.clear();
    for (std::size_t i = 0; i < width; ++i) iov.emplace_back(store[i]);
}

void arm_b_spill_point(const std::vector<std::vector<std::byte>>& store) {
    std::printf(
        "\n== B. SPILL POINT - prefixed_iov_t shape, allocations counted per gather ==\n"
        "   %-8s %-8s %s\n",
        "spans", "allocs", "bytes");
    std::vector<std::span<const std::byte>> iov;
    iov.reserve(32);
    for (std::size_t w = 14; w <= 20; ++w) {
        fill_iov(iov, store, w);
        g_allocs = 0;
        g_alloc_bytes = 0;
        g_counting = true;
        {
            const prefixed_iov_replica_t rec(iov);
            g_sink += rec.n;
        }
        g_counting = false;
        std::printf("   %-8zu %-8zu %-8zu%s\n", w, g_allocs, g_alloc_bytes,
                    g_allocs > 0 && w == 17 ? "   <- FIRST heap allocation" : "");
    }
}

void arm_c_gather_latency(const std::vector<std::vector<std::byte>>& store) {
    constexpr std::size_t kBatch = 20000;
    std::vector<std::span<const std::byte>> a;
    std::vector<std::span<const std::byte>> b;
    std::vector<std::span<const std::byte>> ca;
    std::vector<std::span<const std::byte>> cb;
    a.reserve(32);
    b.reserve(32);
    ca.reserve(32);
    cb.reserve(32);

    const auto time_one = [&](const std::vector<std::span<const std::byte>>& iov) {
        const std::uint64_t t = bench::now_ns();
        for (std::size_t i = 0; i < kBatch; ++i) {
            const prefixed_iov_replica_t rec(iov);
            g_sink += rec.n;
        }
        return static_cast<double>(bench::now_ns() - t) / static_cast<double>(kBatch);
    };

    arm_t inline16{"16 spans (inline)", {}};
    arm_t spill17{"17 spans (SPILL)", {}};
    arm_t ctl8{"8 spans (control)", {}};
    arm_t ctl9{"9 spans (control)", {}};

    fill_iov(a, store, 16);
    fill_iov(b, store, 17);
    fill_iov(ca, store, 8);
    fill_iov(cb, store, 9);

    // Warm both shapes before the first timed rep so neither arm pays a cold allocator.
    (void)time_one(a);
    (void)time_one(b);

    for (std::size_t r = 0; r < kReps; ++r) {  // INTERLEAVED: base, cand, base, cand, ...
        inline16.per_op.push_back(time_one(a));
        spill17.per_op.push_back(time_one(b));
        ctl8.per_op.push_back(time_one(ca));
        ctl9.per_op.push_back(time_one(cb));
    }

    std::printf("\n== C. ISOLATED GATHER LATENCY - no syscall, allocation visible ==");
    verdict("   TREATMENT 16 -> 17 spans (crosses the spill)", inline16, spill17);
    verdict("   CONTROL   8 -> 9 spans (both inline, same +1 span)", ctl8, ctl9);
}

/* ---------------------------------------------------------- arm D: the real UDP transport */

void arm_d_real_transport(const std::vector<std::vector<std::byte>>& store) {
    constexpr std::size_t kBatch = 4000;
    // A bound sibling makes the datagram deliverable, so `send` takes its real sendmsg path
    // (a peerless udp_transport_t returns early and would measure nothing).
    tr::net::udp_transport_t tx(47420, "127.0.0.1", 47421);
    tr::net::udp_transport_t rx(47421, "127.0.0.1", 47420);
    (void)rx;
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    std::vector<std::span<const std::byte>> a;
    std::vector<std::span<const std::byte>> b;
    std::vector<std::span<const std::byte>> ca;
    std::vector<std::span<const std::byte>> cb;
    a.reserve(32);
    b.reserve(32);
    ca.reserve(32);
    cb.reserve(32);
    fill_iov(a, store, 16);
    fill_iov(b, store, 17);
    fill_iov(ca, store, 8);
    fill_iov(cb, store, 9);

    const auto time_one = [&](const std::vector<std::span<const std::byte>>& iov) {
        const std::uint64_t t = bench::now_ns();
        for (std::size_t i = 0; i < kBatch; ++i)
            tx.send(std::span<const std::span<const std::byte>>(iov));
        return static_cast<double>(bench::now_ns() - t) / static_cast<double>(kBatch);
    };

    arm_t inline16{"16 spans (inline)", {}};
    arm_t spill17{"17 spans (SPILL)", {}};
    arm_t ctl8{"8 spans (control)", {}};
    arm_t ctl9{"9 spans (control)", {}};

    (void)time_one(a);
    (void)time_one(b);

    for (std::size_t r = 0; r < kReps; ++r) {
        inline16.per_op.push_back(time_one(a));
        spill17.per_op.push_back(time_one(b));
        ctl8.per_op.push_back(time_one(ca));
        ctl9.per_op.push_back(time_one(cb));
    }

    std::printf("\n== D1. REAL udp_transport_t::send(iov) - allocation behind the syscall ==");
    verdict("   TREATMENT 16 -> 17 spans (crosses the spill)", inline16, spill17);
    verdict("   CONTROL   8 -> 9 spans (both inline, same +1 span)", ctl8, ctl9);
}

/**
 * @brief The SAME A/B through `tcp_transport_t::send(iov)` — the named site
 *        (`transport_tcp.cpp` `prefixed_iov_t`), so the claim is not carried by UDP alone.
 *
 * A real stream socket, so writev is subject to flow control; the peer's own receive thread
 * drains, and both arms sit behind the identical socket, so any backpressure hits them equally.
 */
void arm_d2_real_tcp(const std::vector<std::vector<std::byte>>& store) {
    constexpr std::size_t kBatch = 2000;
    tr::net::tcp_transport_t server(47422);
    tr::net::tcp_transport_t client("127.0.0.1", 47422);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    std::vector<std::span<const std::byte>> a;
    std::vector<std::span<const std::byte>> b;
    std::vector<std::span<const std::byte>> ca;
    std::vector<std::span<const std::byte>> cb;
    a.reserve(32);
    b.reserve(32);
    ca.reserve(32);
    cb.reserve(32);
    fill_iov(a, store, 16);
    fill_iov(b, store, 17);
    fill_iov(ca, store, 8);
    fill_iov(cb, store, 9);

    const auto time_one = [&](const std::vector<std::span<const std::byte>>& iov) {
        const std::uint64_t t = bench::now_ns();
        for (std::size_t i = 0; i < kBatch; ++i)
            client.send(std::span<const std::span<const std::byte>>(iov));
        return static_cast<double>(bench::now_ns() - t) / static_cast<double>(kBatch);
    };

    arm_t inline16{"16 spans (inline)", {}};
    arm_t spill17{"17 spans (SPILL)", {}};
    arm_t ctl8{"8 spans (control)", {}};
    arm_t ctl9{"9 spans (control)", {}};

    (void)time_one(a);
    (void)time_one(b);

    for (std::size_t r = 0; r < kReps; ++r) {
        inline16.per_op.push_back(time_one(a));
        spill17.per_op.push_back(time_one(b));
        ctl8.per_op.push_back(time_one(ca));
        ctl9.per_op.push_back(time_one(cb));
    }

    std::printf("\n== D2. REAL tcp_transport_t::send(iov) - the named site, over loopback ==");
    verdict("   TREATMENT 16 -> 17 spans (crosses the spill)", inline16, spill17);
    verdict("   CONTROL   8 -> 9 spans (both inline, same +1 span)", ctl8, ctl9);
}

/* ------------------------------------------- arm E: the DEFAULT send(iov) - always allocates */

/** @brief A sink transport that does NOT override `send(iov)`, so it inherits the base class's
 *         flatten-into-a-vector default — exactly what `transport_can` and any embedder
 *         transport without native scatter-gather gets. */
struct flatten_sink_t : transport_t {
    void send(std::span<const std::byte> f) override { g_sink += f.size(); }
};

/** @brief The same sink with a native gather, so the two paths differ only in the flatten. */
struct gather_sink_t : transport_t {
    void send(std::span<const std::byte> f) override { g_sink += f.size(); }
    void send(std::span<const std::span<const std::byte>> iov) override {
        const prefixed_iov_replica_t rec(iov);
        g_sink += rec.n;
    }
};

void arm_e_default_path(const std::vector<std::vector<std::byte>>& store) {
    constexpr std::size_t kBatch = 20000;
    flatten_sink_t flat;
    gather_sink_t gather;
    std::vector<std::span<const std::byte>> iov;
    iov.reserve(32);
    fill_iov(iov, store, 9);  // the forward hop's own worst case: kFwdMaxIov spans

    const auto time_one = [&](transport_t& t) {
        const std::uint64_t s = bench::now_ns();
        for (std::size_t i = 0; i < kBatch; ++i)
            t.send(std::span<const std::span<const std::byte>>(iov));
        return static_cast<double>(bench::now_ns() - s) / static_cast<double>(kBatch);
    };

    arm_t g{"native gather (0 alloc)", {}};
    arm_t f{"DEFAULT flatten (1 alloc + copy)", {}};
    (void)time_one(gather);
    (void)time_one(flat);
    for (std::size_t r = 0; r < kReps; ++r) {
        g.per_op.push_back(time_one(gather));
        f.per_op.push_back(time_one(flat));
    }

    std::printf(
        "\n== E. THE ALWAYS-ALLOCATE PATH - transport_t::send(iov)'s DEFAULT, at 9 spans ==\n"
        "   transport_can and any embedder transport without a native gather land here on\n"
        "   EVERY frame, at EVERY width - no 17-span precondition.");
    verdict("   9 spans, no syscall either side", g, f);
}

}  // namespace

void* operator new(std::size_t size) {
    void* p = counted_alloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t size) {
    void* p = counted_alloc(size);
    if (!p) throw std::bad_alloc();
    return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return counted_alloc(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return counted_alloc(size);
}
void* operator new(std::size_t size, std::align_val_t) { return operator new(size); }
void* operator new[](std::size_t size, std::align_val_t) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }

int main() {
    std::printf(
        "iovec spill: how often is it reached, and what does one cost?\n"
        "reps per arm = %zu, interleaved; medians reported with an explicit overlap check.\n",
        kReps);

    // 8-byte spans throughout: the only thing that varies across an A/B is the span COUNT.
    const std::vector<std::vector<std::byte>> store(40, std::vector<std::byte>(8, std::byte{0xA5}));

    arm_a_span_census();
    arm_b_spill_point(store);
    arm_c_gather_latency(store);
    arm_d_real_transport(store);
    arm_d2_real_tcp(store);
    arm_e_default_path(store);

    std::printf("\n(sink=%zu)\n", static_cast<std::size_t>(g_sink));
    return 0;
}
