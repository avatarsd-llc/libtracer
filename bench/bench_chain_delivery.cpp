/**
 * @file
 * @brief The STEADY-STATE stream delivery over the PAIR chain — what one sample costs per hop.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A stream sample is delivered as `FWD{op=WRITE, dst = PATH{PAIR...}, src = <empty PATH>}`: the
 * producer's stored reverse chain, one PAIR element per hop (RFC-0024 §7.1, RFC-0029 §4.2). Each
 * hop reads its own element (a bounds check and a generation compare against its vertex map),
 * consumes it and sends the rest on. No hop holds anything per flow: there is no label table, no
 * binding and no setup frame, so the first sample costs what the millionth costs. This bench
 * replaces `bench_compact_delivery`, which timed the label-switched form this chain replaced
 * (#1951, stage 6 of #1938).
 *
 * Two modes, because a hop does one of two things with a sample:
 *   - `chain-terminus` — the element names a local vertex; the hop dereferences it and writes.
 *     The `src` is empty, so the write is unacknowledged and emits nothing (#1491).
 *   - `chain-forward`  — the element names a connection vertex; the hop consumes it, grows `src`
 *     by its own return element and sends the frame on that link.
 *
 * Every frame enters through the child's receiver ctx, the production path (#1808), and every
 * link is a connection vertex made by `transport_vertex_t`, which is what a PAIR hop
 * dereferences. Allocations are counted around one frame, so the RAM axis is exact rather than
 * inferred. Batch-amortized and self-calibrating for the same reason the other benches are: one
 * delivery is close enough to `clock_gettime` that per-op timing measures the clock.
 *
 * The `link-state` NOTE rows answer the per-link RAM question the label tables raised: what a
 * forwarding hop holds per link to attach it, and what one stream flow adds to that.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../core/tests/fwd_frame_builder.hpp"  // host-only frame builders
#include "../core/tests/pair_body.hpp"          // host-only PAIR element encoder
#include "bench_common.hpp"
#include "bench_process.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_vertex.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::net::transport_vertex_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Payload sizes swept — the value each sample carries. */
constexpr std::size_t kPayloadSizes[] = {4, 64, 512};

/** @brief Link counts the `link-state` census is taken at. */
constexpr std::size_t kLinkCounts[] = {1, 32};

constexpr double kDefaultBudgetSeconds = 1.0;

[[nodiscard]] double budget_seconds() {
    const char* const env = std::getenv("LIBTRACER_BENCH_SECONDS");
    if (env == nullptr) return kDefaultBudgetSeconds;
    const double v = std::strtod(env, nullptr);
    return v > 0.0 ? v : kDefaultBudgetSeconds;
}

std::size_t g_allocs = 0;
std::size_t g_bytes = 0;
bool g_arm = false;

}  // namespace

// The full allocating and deallocating set, for the reasons bench_forward_heap documents: the
// heap backend allocates through the nothrow forms (#801), and a set with a hole hands a
// sanitizer's operator a pointer this file `malloc`ed (#793).
namespace {
void* counted(std::size_t n) {
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    return std::malloc(n == 0 ? 1 : n);
}

/** @brief The aligned counted allocation (#801): `aligned_alloc` only when over-aligned. */
void* counted_aligned(std::size_t n, std::size_t align) {
    if (align <= alignof(std::max_align_t)) return counted(n);
    if (g_arm) {
        ++g_allocs;
        g_bytes += n;
    }
    const std::size_t rounded = ((n == 0 ? 1 : n) + align - 1) / align * align;
    return std::aligned_alloc(align, rounded);
}
}  // namespace

void* operator new(std::size_t n) {
    void* const p = counted(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    void* const p = counted_aligned(n, static_cast<std::size_t>(a));
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return counted_aligned(n, static_cast<std::size_t>(a));
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

/**
 * @brief A link that swallows what it is handed (no I/O, no allocation in the window), and
 *        hands an inbound frame up the way a real link's receive thread does.
 */
struct sink_link_t : transport_t {
    std::size_t sends = 0;
    void send(std::span<const std::byte>) override { ++sends; }
    void send(std::span<const std::span<const std::byte>>) override { ++sends; }
    /** @brief Deliver @p frame to the receiver `add_child` installed: the per-child ctx. */
    void inject(std::span<const std::byte> frame) const { rx_.deliver_borrowed(frame); }
    /** @brief Whether a borrowed-span receiver (and no rope receiver) is installed. */
    [[nodiscard]] bool has_span_receiver() const noexcept {
        return rx_.has_any() && !rx_.has_rope();
    }
};

/** @brief One node: a graph, its router and the `/net` tree its links are connection vertices of.
 */
struct node_t {
    graph_t g;
    fwd_router_t r{g};
    transport_vertex_t net{g, r};

    /** @brief Attach @p link as the connection vertex `/net/<mod>/<name>`. */
    [[nodiscard]] bool bind(const char* mod, const std::string& name, transport_t& link) {
        net.provide_link(mod, name, link);
        (void)net.register_module(mod, mod, tr::net::conn_role_t::DIAL);
        return g
            .write(path_t(std::string("/net/") + mod + "/conn"), tr::net::conn_spec_t(name).view())
            .has_value();
    }

    /** @brief The PAIR element naming the vertex at @p p on this node, if it exists. */
    [[nodiscard]] std::optional<tr::wire::path_pair_t> pair_of(const std::string& p) const {
        const auto v = g.find(path_t(p).key());
        return v ? g.vertex_slot(*v) : std::nullopt;
    }
};

/** @brief Stop the run: the frame did not take the production path this bench claims. */
[[noreturn]] void path_broken(const char* mode, const char* why) {
    std::fprintf(stderr, "FAIL mode=%s: %s\n", mode, why);
    std::fflush(stdout);
    std::exit(1);
}

std::vector<std::byte> path_of(std::span<const std::byte> body) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

std::vector<std::byte> value_tlv(std::size_t n) {
    const std::vector<std::byte> payload(n, std::byte{0xAB});
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(payload));
    return out;
}

/** @brief A stream sample as the producer emits it: `FWD{WRITE, dst, src = <empty PATH>}`. */
std::vector<std::byte> sample_frame(std::span<const std::byte> dst_body, std::size_t payload) {
    return tr::testing::b_fwd_raw_op(static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE),
                                     path_of(dst_body), path_of({}), {}, value_tlv(payload));
}

/** @brief Count deliveries at the terminus, so the bench asserts the write landed. */
void count_delivery(void* ctx, const tr::graph::value_t&) { ++*static_cast<std::size_t*>(ctx); }

/**
 * @brief One measured point: N stream samples over an established chain.
 * @param payload  The VALUE payload each sample carries.
 * @param terminus true ⇒ the element names a local vertex; false ⇒ a connection vertex.
 * @param budget   The timed loop's budget in seconds.
 */
void run_point(std::size_t payload, bool terminus, double budget) {
    const char* const mode = terminus ? "chain-terminus" : "chain-forward";
    node_t n;
    sink_link_t up;
    sink_link_t down;
    std::size_t delivered = 0;
    if (!n.bind("up", "o", up)) path_broken(mode, "connection creation refused (up)");
    std::vector<std::byte> dst;
    if (terminus) {
        (void)n.g.register_vertex(*path_t::parse("/sink"), role_t::STREAM);
        (void)n.g.subscribe(*path_t::parse("/sink"), &count_delivery, &delivered);
        const auto sink = n.pair_of("/sink");
        if (!sink) path_broken(mode, "no sink slot");
        tr::testing::emit_path_pair(dst, *sink);
    } else {
        if (!n.bind("dn", "far", down)) path_broken(mode, "connection creation refused (dn)");
        const auto conn = n.pair_of("/net/dn/far");
        if (!conn) path_broken(mode, "no connection-vertex slot");
        tr::testing::emit_path_pair(dst, *conn);
        // The next hop's element rides on verbatim; this hop never reads it.
        tr::testing::emit_path_pair(dst, tr::wire::path_pair_t{.index = 7, .generation = 1});
    }
    const std::vector<std::byte> frame = sample_frame(dst, payload);
    const auto deliver = [&] { up.inject(frame); };

    // Assert the path before timing it: the receiver is installed, ONE injected frame lands on
    // exactly the leg its element names, nothing answers it, and no hop resolved a link by NAME.
    if (!up.has_span_receiver()) path_broken(mode, "add_child installed no span receiver");
    const std::size_t sends0 = down.sends;
    deliver();
    if (down.sends - sends0 != (terminus ? 0U : 1U))
        path_broken(mode, "one injected frame did not reach exactly its own leg");
    if (up.sends != 0) path_broken(mode, "an unacknowledged sample drew a reply");
    if (terminus && delivered != 1) path_broken(mode, "the terminus frame wrote nothing to /sink");
    if (n.r.reply_name_lookups() != 0)
        path_broken(mode, "a frame resolved its link by NAME, not through the receiver ctx");

    for (int i = 0; i < 64; ++i) deliver();

    // RAM axis: allocations around exactly one delivery, outside the timed window.
    g_allocs = 0;
    g_bytes = 0;
    g_arm = true;
    deliver();
    g_arm = false;
    const std::size_t allocs = g_allocs;
    const std::size_t bytes = g_bytes;

    // Window-calibrated batches, per-op picoseconds, every window >= 20 µs (#1804).
    const bench::batch_timing_t t =
        bench::time_batches(deliver, static_cast<std::uint64_t>(budget * 1e9));
    const double mbps = t.ops_per_s * static_cast<double>(payload) / 1e6;
    bench::emit_batch("libtracer", mode, payload, 1, 1, t.ops_per_s, t.ops_per_s, mbps, t);
    std::printf("NOTE mode=%s payload=%zu allocs=%zu bytes=%zu\n", mode, payload, allocs, bytes);
}

/**
 * @brief A block source that counts what is outstanding on it, over the process net source.
 *
 * Injected as the router's long-lived link-state plane, which is where per-link state lives
 * (receive contexts, bus token caches, and before #1951 the label tables). A pool source hands
 * out slab space without a heap call, so the live-heap figure cannot see this state; the source
 * itself can.
 */
struct counting_source_t final : tr::mem::block_source_t {
    std::size_t outstanding = 0;
    counting_source_t() : block_source_t("counting") {}
    void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        void* const p = tr::mem::net_source().try_alloc(bytes, align);
        if (p != nullptr) outstanding += bytes;
        return p;
    }
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        outstanding -= bytes;
        tr::mem::net_source().release(p, bytes, align);
    }
};

/**
 * @brief The per-link RAM a forwarding hop keeps: @p links inbound links, each then carrying
 *        the first sample of one stream flow to the same outbound connection vertex.
 *
 * `attach_B` is what attaching a link costs on the link-state plane; `flow_B` is what the first
 * sample of a flow added on top of it. A stateless hop reads `flow_B=0`.
 */
void run_link_state(std::size_t links) {
    counting_source_t src;
    {
        graph_t g;
        fwd_router_t r{g, tr::net::router_planes_t{.label_src = &src}};
        transport_vertex_t net{g, r};
        const auto bind = [&](const char* mod, const std::string& name, transport_t& link) {
            net.provide_link(mod, name, link);
            (void)net.register_module(mod, mod, tr::net::conn_role_t::DIAL);
            if (!g.write(path_t(std::string("/net/") + mod + "/conn"),
                         tr::net::conn_spec_t(name).view()))
                path_broken("link-state", "connection creation refused");
        };
        sink_link_t down;
        bind("dn", "far", down);
        const std::size_t base = src.outstanding;
        std::vector<std::unique_ptr<sink_link_t>> ups;
        for (std::size_t i = 0; i < links; ++i) {
            ups.push_back(std::make_unique<sink_link_t>());
            bind("up", "o" + std::to_string(i), *ups.back());
        }
        const std::size_t attached = src.outstanding;
        const auto v = g.find(path_t("/net/dn/far").key());
        const auto conn = v ? g.vertex_slot(*v) : std::nullopt;
        if (!conn) path_broken("link-state", "no connection-vertex slot");
        std::vector<std::byte> dst;
        tr::testing::emit_path_pair(dst, *conn);
        tr::testing::emit_path_pair(dst, tr::wire::path_pair_t{.index = 7, .generation = 1});
        const std::vector<std::byte> frame = sample_frame(dst, 64);
        for (const auto& u : ups) u->inject(frame);
        if (down.sends != links) path_broken("link-state", "a sample was not forwarded");
        const std::size_t flowed = src.outstanding;
        std::printf(
            "NOTE mode=link-state links=%zu attach_B_per_link=%zu flow_B_per_link=%zu "
            "sizeof_router=%zu\n",
            links, (attached - base) / links, (flowed - attached) / links, sizeof(fwd_router_t));
    }
}

}  // namespace

int main(int /*argc*/, char** argv) {
    bench::pin_allocator_state(argv);  // fixed allocator state (#1803)
    bench::emit_clock_floor();         // the run's clock floor, ahead of its rows (#1804)
    bench::emit_alloc_state();         // and the allocator settings they run under (#1903)
    const std::size_t start_kb = bench::peak_rss_kb();  // no heap op, no file, ahead of a row
    std::printf("# Steady-state stream delivery over the PAIR chain (RFC-0024 §7.1)\n");
    for (const std::size_t p : kPayloadSizes) {
        run_point(p, /*terminus=*/true, budget_seconds());
        run_point(p, /*terminus=*/false, budget_seconds());
    }
    // The payload ladder (#1806), after every existing row, at a quarter of the budget each.
    for (const std::size_t p : bench::kPayloadLadder) {
        if (std::find(std::begin(kPayloadSizes), std::end(kPayloadSizes), p) !=
            std::end(kPayloadSizes))
            continue;
        run_point(p, /*terminus=*/true, budget_seconds() / 4);
        run_point(p, /*terminus=*/false, budget_seconds() / 4);
    }
    bench::emit_family_rss("chain-delivery", start_kb);  // after the last row (#1908)
    for (const std::size_t l : kLinkCounts) run_link_state(l);
    return 0;
}
