/**
 * @file
 * @brief Self-test for `delivery_count.hpp` — the two functions the `inproc-target-*` and
 *        `inproc-remote` rows now publish their delivery figure through (#1481).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Written from the failing direction, because the defect was invisible in the happy path.
 * `pub_s * fanout` and a counted figure agree EXACTLY whenever the graph delivers in full,
 * which it does on an idle host at every width the sweep runs — so a test that only builds
 * a healthy topology passes identically before and after the fix and guards nothing.
 *
 * So the topology below sheds ON PURPOSE. Four path-target subscriber edges are admitted
 * against a source, one of them naming a target vertex that was never registered:
 * `dispatch_edge_target` resolves it to nothing, counts a `no_target` drop and returns,
 * and the `write()` that fanned out to it still returns SUCCESS. That is the whole shape
 * of the bug in miniature — the publish loop completes at full speed while a quarter of
 * the deliveries never happen — and the expectation is the one the issue names: the
 * COUNTED figure must come out strictly below the DERIVED one, and land exactly on the
 * three-quarters that did arrive. The positive twin registers all four targets and must
 * then agree with the arithmetic, so a `deliveries_from_drops` that simply returned `want`
 * (the pre-fix figure) fails the first case rather than passing both.
 *
 * No timing and no sweep: this drives the graph for a few thousand writes.
 *
 *     ./build/test_delivery_count       # exit 0 = every expectation held
 */
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "delivery_count.hpp"
#include "libtracer/tracer.hpp"

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::view::view_t;

namespace {

/** @brief Expectations that did not hold; the process exit status. */
int g_failures = 0;

/** @brief Record one expectation. */
void check(bool ok, const char* what) {
    if (ok) {
        std::printf("ok    %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

/** @brief Per-message owned heap view — `bench_libtracer.cpp`'s allocating path, verbatim. */
[[nodiscard]] view_t owned_view(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief A VALUE TLV carrying @p payload bytes. */
[[nodiscard]] std::vector<std::byte> value_tlv(std::size_t payload) {
    const std::vector<std::byte> p(payload, std::byte{0xAB});
    tr::wire::tlv_t t{};
    t.type = tr::wire::type_t::VALUE;
    t.payload = p;
    return tr::wire::encode(t);
}

/**
 * @brief A SUBSCRIBER TLV naming a single-segment target path (the wire subscribe form).
 *
 * The same shape `run_inproc_target` admits its edges with — a `SUBSCRIBER` whose `PATH`
 * child is the target key — which is what gives the edge a non-null `target_key` and so
 * sends its deliveries down `dispatch_edge_target` rather than a callback.
 */
[[nodiscard]] view_t subscriber_tlv(std::string_view target_segment) {
    std::vector<std::byte> body;
    (void)tr::wire::emit_path_segment(body, target_segment);
    const tr::wire::tlv_t path{.type = tr::wire::type_t::PATH, .payload = body};
    tr::wire::tlv_t sub{.type = tr::wire::type_t::SUBSCRIBER};
    sub.opt.pl = true;
    sub.children.push_back(path);
    return owned_view(tr::wire::encode(sub));
}

/** @brief What one run of the miniature `inproc-target-stored` topology observed. */
struct run_t {
    std::uint64_t want = 0;    /**< @brief The DERIVED figure: publishes x fan-out. */
    std::uint64_t counted = 0; /**< @brief What `deliveries_from_drops` reports instead. */
    std::uint64_t dropped = 0; /**< @brief What the graph accounted as shed in the window. */
    bool admitted = false;     /**< @brief Every edge admitted and every write succeeded. */
};

/**
 * @brief Run @p msgs writes over 4 path-target edges, registering only @p live targets.
 *
 * `live == 4` is the healthy topology; anything less leaves that many edges naming a
 * vertex that does not exist, which is a delivery lost per edge per write with a
 * successful `write()` on top of it.
 */
[[nodiscard]] run_t run(std::size_t live, std::size_t msgs) {
    constexpr std::size_t kFan = 4;
    graph_t g;
    for (std::size_t f = 0; f < live; ++f)
        (void)g.register_vertex(*path_t::parse("/t" + std::to_string(f)), role_t::STORED_VALUE);
    const path_t src_path = *path_t::parse("/bench/target-src");
    const tr::graph::vertex_handle_t src = g.register_vertex(src_path, role_t::STORED_VALUE);

    const path_t sub_path = *path_t::parse("/bench/target-src:subscribers[]");
    std::size_t admitted = 0;
    for (std::size_t f = 0; f < kFan; ++f)
        if (g.write(sub_path, subscriber_tlv("t" + std::to_string(f))).has_value()) ++admitted;

    run_t out;
    out.want = static_cast<std::uint64_t>(msgs) * kFan;
    if (admitted != kFan) return out;

    const std::vector<std::byte> tlv = value_tlv(64);
    const graph_t::delivery_drops_t before = g.delivery_drops();
    bool all_ok = true;
    for (std::size_t i = 0; i < msgs; ++i)
        if (!g.write(src, owned_view(tlv)).has_value()) all_ok = false;
    const graph_t::delivery_drops_t after = g.delivery_drops();

    out.admitted = all_ok;
    out.dropped = bench::drops_between(before, after);
    out.counted = bench::deliveries_from_drops(out.want, before, after);
    return out;
}

/** @brief The defect, in one case: a shed fan-out must lower the published figure. */
void a_shed_delivery_counts_below_the_arithmetic() {
    const run_t r = run(/*live=*/3, /*msgs=*/1000);
    check(r.admitted, "every edge admitted and every write returned success");
    check(r.want == 4000, "the DERIVED figure is publishes x fan-out");
    check(r.dropped == 1000, "the unresolvable target's 1000 deliveries are accounted");
    check(r.counted < r.want, "the COUNTED figure is strictly below the derived one");
    check(r.counted == 3000, "...and equals exactly the deliveries that arrived");
}

/** @brief The positive twin: with nothing shed, the two figures must agree. */
void a_healthy_fan_out_counts_the_arithmetic() {
    const run_t r = run(/*live=*/4, /*msgs=*/1000);
    check(r.admitted, "every edge admitted and every write returned success");
    check(r.dropped == 0, "a fully-resolvable fan-out sheds nothing");
    check(r.counted == r.want, "so the counted figure agrees with the arithmetic exactly");
}

/** @brief The rate a row publishes comes from what it counted, not from the ceiling. */
void the_published_rate_is_the_counted_one() {
    const double shed = bench::delivered_rate("test", 64, 4, 1, 4000, 3000, 2.0);
    check(shed == 1500.0, "delivered_rate divides the COUNT by the window");
    const double full = bench::delivered_rate("test", 64, 4, 1, 4000, 4000, 2.0);
    check(full == 2000.0, "...and reaches the arithmetic figure only when nothing is shed");
}

/**
 * @brief More drops than the window asked for saturates at zero.
 *
 * The counters belong to the whole graph, so a drop charged by something outside the timed
 * loop can exceed it. Unsigned wrap would publish `UINT64_MAX` deliveries — a defect
 * strictly worse than the one being fixed.
 */
void more_drops_than_deliveries_saturates() {
    graph_t::delivery_drops_t before{};
    graph_t::delivery_drops_t after{};
    after.no_target = 10;
    check(bench::deliveries_from_drops(4, before, after) == 0,
          "a drop tally past the ceiling reports zero, never a wrapped count");
}

}  // namespace

int main() {
    a_shed_delivery_counts_below_the_arithmetic();
    a_healthy_fan_out_counts_the_arithmetic();
    the_published_rate_is_the_counted_one();
    more_drops_than_deliveries_saturates();
    std::printf("%s\n",
                g_failures == 0 ? "test_delivery_count: PASS" : "test_delivery_count: FAIL");
    return g_failures == 0 ? 0 : 1;
}
