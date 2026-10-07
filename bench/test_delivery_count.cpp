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
#include <new>
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

/** @brief A ring source that funds nothing: every STREAM admission against it sheds. */
class refusing_source_t final : public tr::mem::block_source_t {
   public:
    refusing_source_t() : tr::mem::block_source_t("refusing") {}

    /** @brief Refuse, and count the refusal. */
    [[nodiscard]] void* try_alloc(std::size_t /*bytes*/, std::size_t /*align*/) noexcept override {
        ++refusals;
        return nullptr;
    }

    /** @brief Nothing was ever served, so nothing comes back. */
    void release(void* /*p*/, std::size_t /*bytes*/, std::size_t /*align*/) noexcept override {}

    std::uint64_t refusals = 0; /**< @brief try_alloc calls refused. */
};

/** @brief @p msgs writes to the `eptype-stream` fixture; returns what its subscriber counted. */
[[nodiscard]] std::uint64_t stream_writes(bench::stream_fixture_t& fx, std::size_t msgs) {
    const std::vector<std::byte> tlv = value_tlv(64);
    fx.recv.store(0);
    for (std::size_t i = 0; i < msgs; ++i) (void)fx.g.write(fx.v, owned_view(tlv));
    return fx.recv.load();
}

/**
 * @brief A forced STREAM shed moves the `eptype-stream` delivery rate (#1805).
 *
 * The row used to publish `pub_s` as its delivery rate. Here the same fixture runs over a
 * best-effort ring whose source refuses every entry: each write still returns, and each entry
 * is shed. The counted rate must fall below the publish rate; the healthy twin must match it.
 */
void a_shed_stream_moves_the_eptype_stream_rate() {
    constexpr std::size_t kMsgs = 1000;
    bench::stream_fixture_t healthy;
    const std::uint64_t ok = stream_writes(healthy, kMsgs);
    check(ok == kMsgs, "a healthy STREAM ring delivers every write");
    check(bench::delivered_rate("eptype-stream", 64, 1, 1, kMsgs, ok, 1.0) == kMsgs,
          "...so the counted rate equals the publish rate");

    refusing_source_t refusing;
    bench::stream_fixture_t shed(&refusing);
    const std::uint64_t got = stream_writes(shed, kMsgs);
    check(refusing.refusals > 0, "the ring source was asked and refused");
    check(got < kMsgs, "a shedding STREAM ring delivers fewer than it was written");
    check(bench::delivered_rate("eptype-stream", 64, 1, 1, kMsgs, got, 1.0) < kMsgs,
          "...and the counted rate falls below the publish rate");
}

/**
 * @brief A source that serves from the default root until told to refuse, and then refuses
 *        every request: a topology is built on it healthy, then made to lose deliveries.
 */
class switchable_source_t final : public tr::mem::block_source_t {
   public:
    switchable_source_t() : tr::mem::block_source_t("switchable") {}

    /** @brief Serve from the default root, or refuse once @ref refuse is set. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse) {
            ++refusals;
            return nullptr;
        }
        return tr::mem::default_root().try_alloc(bytes, align);
    }

    /** @brief Hand back what the default root served. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tr::mem::default_root().release(p, bytes, align);
    }

    bool refuse = false;        /**< @brief Refuse every request from now on. */
    std::uint64_t refusals = 0; /**< @brief try_alloc calls refused. */
};

/** @brief The `mixed` row's five payload sizes (`bench::kSizes`). */
constexpr std::size_t kMixedSizes[] = {1, 8, 64, 1024, 8192};

/**
 * @brief @p msgs writes, round-robin over the `mixed` topics, each one allowed to fail; a
 *        write the graph cannot fund is a delivery lost, whichever way it reports it.
 */
[[nodiscard]] std::uint64_t mixed_writes(bench::mixed_fixture_t& fx, std::size_t msgs) {
    fx.recv.store(0);
    for (std::size_t i = 0; i < msgs; ++i) {
        const std::size_t e = i % bench::mixed_fixture_t::kTopics;
        try {
            (void)fx.g.write(fx.verts[e], owned_view(fx.tlvs[e]));
        } catch (const std::bad_alloc&) {  // the graph's pmr adapter reports a refusal so
        }
    }
    return fx.recv.load();
}

/**
 * @brief Lost deliveries move the `mixed` delivery rate (#1905).
 *
 * The same topology the row writes, on a source that is switched to refuse once it is built.
 * The counted rate must fall below the arithmetic ceiling; the healthy twin must reach it.
 */
void a_lost_delivery_moves_the_mixed_rate() {
    constexpr std::size_t kMsgs = 1280;
    const std::span<const std::size_t, 5> sizes(kMixedSizes);
    bench::mixed_fixture_t healthy(value_tlv, sizes);
    const std::uint64_t want = healthy.want(kMsgs);
    const std::uint64_t ok = mixed_writes(healthy, kMsgs);
    check(ok == want, "a healthy mixed topology delivers its whole fan-out");

    switchable_source_t src;
    bench::mixed_fixture_t lossy(value_tlv, sizes, src);
    src.refuse = true;
    const std::uint64_t got = mixed_writes(lossy, kMsgs);
    check(src.refusals > 0, "the mixed graph's source was asked and refused");
    check(got < want, "a refusing mixed graph delivers less than its fan-out");
    check(bench::delivered_rate("mixed", 0, 6, 128, want, got, 1.0) < static_cast<double>(want),
          "...and the counted mixed rate falls below the arithmetic one");
}

/**
 * @brief Lost deliveries move the `inproc-mt` delivery rate (#1905).
 *
 * One worker as the row builds it, on a source switched to refuse once it is wired. The
 * worker's count must fall short of its writes; the healthy twin must match them.
 */
void a_lost_delivery_moves_the_inproc_mt_rate() {
    constexpr std::size_t kMsgs = 1000;
    const std::vector<std::byte> tlv = value_tlv(64);
    const auto writes = [&](bench::counting_writer_t& w) {
        w.recv.store(0);
        for (std::size_t i = 0; i < kMsgs; ++i) {
            try {
                (void)w.g.write(*w.v, w.view);
            } catch (const std::bad_alloc&) {
            }
        }
        return w.recv.load();
    };
    bench::counting_writer_t healthy;
    healthy.wire(tlv);
    check(writes(healthy) == kMsgs, "a healthy inproc-mt worker delivers every write");

    switchable_source_t src;
    bench::counting_writer_t lossy(src);
    lossy.wire(tlv);
    src.refuse = true;
    const std::uint64_t got = writes(lossy);
    check(src.refusals > 0, "the worker graph's source was asked and refused");
    check(got < kMsgs, "a refusing inproc-mt worker delivers fewer than it was written");
    check(bench::delivered_rate("inproc-mt1", 64, 1, 1, kMsgs, got, 1.0) < kMsgs,
          "...and the counted inproc-mt rate falls below the publish rate");
}

}  // namespace

int main() {
    a_shed_stream_moves_the_eptype_stream_rate();
    a_lost_delivery_moves_the_mixed_rate();
    a_lost_delivery_moves_the_inproc_mt_rate();
    a_shed_delivery_counts_below_the_arithmetic();
    a_healthy_fan_out_counts_the_arithmetic();
    the_published_rate_is_the_counted_one();
    more_drops_than_deliveries_saturates();
    std::printf("%s\n",
                g_failures == 0 ? "test_delivery_count: PASS" : "test_delivery_count: FAIL");
    return g_failures == 0 ? 0 : 1;
}
