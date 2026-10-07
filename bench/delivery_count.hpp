/**
 * @file
 * @brief How a libtracer bench row states what it DELIVERED — the observed figure, and the
 *        short-count guard that says so out loud when it falls short (#1481).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The fairness audit of PR #1480 removed `pub_s * fanout` from the CHARTED `inproc` rows:
 * a delivery figure derived by arithmetic is a claim, not a measurement, and it stays true
 * only for as long as nothing is shed. The rows this header serves are the ones that
 * audit deliberately left alone — `inproc-target-*` and `inproc-remote` — and the reason
 * to finish the job is that `write()` returns SUCCESS through every shed the graph has:
 * the wide fan-out truncates to its inline prefix when the overflow reserve fails, and
 * `dispatch_edge_target` drops a delivery on an unresolvable target, a denied fan-in gate
 * or a failed nothrow clone. In each case the publish loop finishes at full speed, so the
 * derived figure keeps rising while deliveries are being lost.
 *
 * Two ways to know, and a row uses whichever it HAS:
 *
 * - **Count at the consumer.** A handler target, a remote sink, a subscriber callback —
 *   anything that runs per delivery can increment. This is direct observation and is
 *   always preferred (@ref delivered_rate takes the count).
 * - **Subtract what the graph accounted as dropped.** A `STORED_VALUE` target keeps no
 *   such counter — a delivery there terminates in the target's LKV, and adding a counting
 *   subscriber to see it would change the topology being timed and therefore the number.
 *   `graph_t::delivery_drops()` is what makes this second route sound rather than a
 *   restatement of the arithmetic: RFC-0025 §4.4 makes silence the one forbidden
 *   behaviour, so every leg that sheds one of these deliveries counts it, and
 *   `want - dropped` is a figure the run OBSERVED (@ref deliveries_from_drops).
 *
 * Header rather than a body in `bench_libtracer.cpp` so `test_delivery_count` can drive
 * the same two functions the instrument publishes through, against a topology built to
 * shed on purpose.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "libtracer/mem_source.hpp"
#include "libtracer/tracer.hpp"

namespace bench {

/**
 * @brief Every delivery the graph accounted as dropped between two @ref
 *        tr::graph::graph_t::delivery_drops snapshots.
 *
 * All four causes summed, because all four are deliveries this process was asked to make
 * and did not: an unresolvable target, a fan-in refusal, a failed nothrow clone, and a
 * fan-out snapshot that could not widen past its inline prefix. The counters are relaxed
 * monotonic and read one at a time, so a snapshot taken while another thread delivers can
 * tear — harmless here, where both snapshots bracket a single-threaded timed loop.
 *
 * @param a The earlier snapshot.
 * @param b The later one.
 */
[[nodiscard]] inline std::uint64_t drops_between(
    const tr::graph::graph_t::delivery_drops_t& a,
    const tr::graph::graph_t::delivery_drops_t& b) noexcept {
    return (b.no_target - a.no_target) + (b.denied - a.denied) +
           (b.out_of_memory - a.out_of_memory) + (b.fan_out_truncated - a.fan_out_truncated);
}

/**
 * @brief Deliveries a row is known to have made, for a row whose deliveries terminate
 *        somewhere that keeps no counter.
 *
 * `want` is the arithmetic ceiling — publishes times fan-out — and this is the ONLY place
 * that number is allowed to appear in a published figure, with everything the graph
 * accounted as lost taken back out of it. Saturates at zero rather than wrapping: the
 * counters are shared by the whole graph, so a drop charged by something other than the
 * timed loop (a warmup write racing a retire, a second row in the same process) must
 * report "nothing survived", never `UINT64_MAX` deliveries.
 */
[[nodiscard]] inline std::uint64_t deliveries_from_drops(
    std::uint64_t want, const tr::graph::graph_t::delivery_drops_t& before,
    const tr::graph::graph_t::delivery_drops_t& after) noexcept {
    const std::uint64_t dropped = drops_between(before, after);
    return dropped >= want ? 0 : want - dropped;
}

/**
 * @brief The deliveries-per-second figure a row publishes, from @p got rather than @p want.
 *
 * Warns on a short count and publishes it anyway, which is the shape #1480 settled on for
 * the charted rows: dropping the row would lose a point from a long-running series over
 * exactly the event a reader most wants to see in it, so the honest lower number goes out
 * with a line on stderr beside it. The two agree exactly whenever the graph delivers in
 * full — dispatch is inline, so the publish loop IS the delivery loop — which is why this
 * moves no historical number, and why the day it does is the day it earned its keep.
 *
 * @param mode  The row's mode, so a warning names the series it belongs to.
 * @param S     Payload bytes — part of the row's key, printed with the warning so a short
 *              count identifies its own point.
 * @param F     Fan-out (subscribers per endpoint).
 * @param E     Endpoint count.
 * @param want  The arithmetic ceiling the count is compared against, never published.
 * @param got   Deliveries this run observed.
 * @param secs  The timed window; @p got over it is the figure returned.
 */
[[nodiscard]] inline double delivered_rate(const char* mode, std::size_t S, std::size_t F,
                                           std::size_t E, std::uint64_t want, std::uint64_t got,
                                           double secs) {
    if (got < want)
        std::fprintf(stderr, "[libtracer] mode=%s S=%zu F=%zu E=%zu delivered %llu/%llu (shed)\n",
                     mode, S, F, E, static_cast<unsigned long long>(got),
                     static_cast<unsigned long long>(want));
    return static_cast<double>(got) / secs;
}

/**
 * @brief The STREAM vertex every STREAM row writes to: depth 16, one counting edge.
 *
 * `eptype-stream`, `stream-w<T>`, `stream-spill` and `stream-defer` all write here, and all
 * publish the delivery rate from @ref recv, never from their write count (#1805). It lives in
 * this header so `test_delivery_count` builds the same topology with a ring source that
 * refuses, sheds every entry on purpose, and checks that the counted rate falls with it.
 */
struct stream_fixture_t {
    tr::graph::graph_t g;               /**< @brief The graph, on the default source. */
    tr::graph::vertex_handle_t v;       /**< @brief The STREAM vertex. */
    std::atomic<std::uint64_t> recv{0}; /**< @brief Deliveries the subscriber saw. */

    /**
     * @brief Register `/bench/stream`, a 16-deep ring and one counting subscriber.
     * @param ring_source The ring's own source; null (every bench row) keeps the graph's
     *                    default. The self-test passes one that refuses, to force a shed.
     */
    explicit stream_fixture_t(tr::mem::block_source_t* ring_source = nullptr)
        : v(g.register_vertex(*tr::graph::path_t::parse("/bench/stream"),
                              tr::graph::role_t::STREAM)) {
        (void)g.set_policy(
            v, {.retention = tr::graph::retention_t::N, .depth = 16, .ring_source = ring_source});
        (void)g.subscribe(
            *tr::graph::path_t::parse("/bench/stream"),
            [](void* c, const tr::graph::value_t&) {
                static_cast<std::atomic<std::uint64_t>*>(c)->fetch_add(1,
                                                                       std::memory_order_relaxed);
            },
            &recv);
    }
};

/**
 * @brief The `mixed` row's topology: 128 STORED_VALUE topics whose fan-out cycles 1, 2, 4, 8,
 *        16, every subscriber counting into @ref recv.
 *
 * Here rather than in `bench_libtracer.cpp` for the reason @ref stream_fixture_t is (#1905):
 * `test_delivery_count` builds the same topology over a source it can make refuse, and checks
 * that the row's counted rate falls when deliveries are lost. The row writes; the fixture only
 * holds the graph, the per-topic payloads and the counter.
 */
struct mixed_fixture_t {
    static constexpr std::size_t kTopics = 128;    /**< @brief Topic count, the row's `ep`. */
    tr::graph::graph_t g;                          /**< @brief The graph. */
    std::vector<tr::graph::vertex_handle_t> verts; /**< @brief One vertex per topic. */
    std::vector<std::size_t> fan;                  /**< @brief Each topic's fan-out. */
    std::vector<std::vector<std::byte>> tlvs;      /**< @brief Each topic's VALUE TLV. */
    std::atomic<std::uint64_t> recv{0};            /**< @brief Deliveries counted. */
    std::size_t total_fan = 0;                     /**< @brief Sum of @ref fan. */

    /** @brief Every subscriber's callback; `subscribe` keeps its address, so it is a member. */
    struct counter_t {
        std::atomic<std::uint64_t>* n; /**< @brief Where a delivery is counted. */
        /** @brief Count one delivery. */
        void operator()(const tr::graph::value_t&) const {
            n->fetch_add(1, std::memory_order_relaxed);
        }
    };
    counter_t cb{&recv}; /**< @brief The one callback all 128 topics' subscribers share. */

    /**
     * @brief Register the 128 topics and their counting subscribers.
     * @param make_tlv Builds topic e's VALUE TLV from its payload size, `sizes[e % 5]`.
     * @param sizes    The five payload sizes the topics cycle through.
     * @param src      The graph's source; the default is the graph's own default.
     */
    template <typename MakeTlv>
    mixed_fixture_t(MakeTlv&& make_tlv, std::span<const std::size_t, 5> sizes,
                    tr::mem::block_source_t& src = tr::mem::default_root())
        : g(src) {
        for (std::size_t e = 0; e < kTopics; ++e) {
            tr::graph::path_t path = *tr::graph::path_t::parse("/bench/m" + std::to_string(e));
            auto v = g.register_vertex(path, tr::graph::role_t::STORED_VALUE);
            const std::size_t F = std::size_t{1} << (e % 5);  // 1,2,4,8,16
            for (std::size_t f = 0; f < F; ++f) (void)g.subscribe(path, cb);
            verts.push_back(v);
            fan.push_back(F);
            total_fan += F;
            tlvs.push_back(make_tlv(sizes[e % 5]));
        }
    }

    /** @brief The arithmetic ceiling of @p msgs round-robin writes: their fan-outs summed. */
    [[nodiscard]] std::uint64_t want(std::size_t msgs) const {
        std::uint64_t w = 0;
        for (std::size_t i = 0; i < msgs; ++i) w += fan[i % kTopics];
        return w;
    }
};

/**
 * @brief One `inproc-mt` worker: its own graph, one STORED_VALUE vertex with one counting
 *        subscriber, and one borrowed view it writes again and again.
 *
 * Here for the same reason as @ref mixed_fixture_t (#1905): `test_delivery_count` builds a
 * worker over a source it can make refuse and checks that the counted figure falls.
 */
struct counting_writer_t {
    tr::graph::graph_t g;                        /**< @brief The worker's own graph. */
    std::optional<tr::graph::vertex_handle_t> v; /**< @brief Its one vertex, `/bench/mt`. */
    std::vector<std::byte> buf;                  /**< @brief The bytes @ref view borrows. */
    tr::view::view_t view;                       /**< @brief The view every write hands in. */
    std::atomic<std::uint64_t> recv{0};          /**< @brief Deliveries counted. */
    std::vector<std::uint64_t> lat;              /**< @brief The latency phase's samples. */

    /** @param src The graph's source; the default is the graph's own default. */
    explicit counting_writer_t(tr::mem::block_source_t& src = tr::mem::default_root()) : g(src) {}

    /**
     * @brief Copy @p tlv into the worker, register `/bench/mt` with its counting subscriber and
     *        borrow the copy, so each worker owns its segment and no refcount is shared.
     */
    [[gnu::always_inline]] void wire(const std::vector<std::byte>& tlv) {
        buf = tlv;
        v = g.register_vertex(*tr::graph::path_t::parse("/bench/mt"),
                              tr::graph::role_t::STORED_VALUE);
        (void)g.subscribe(
            *tr::graph::path_t::parse("/bench/mt"),
            [](void* ctx, const tr::graph::value_t&) {
                static_cast<counting_writer_t*>(ctx)->recv.fetch_add(1, std::memory_order_relaxed);
            },
            this);
        view = tr::view::view_t::over(tr::view::borrow_const(buf));
    }
};

}  // namespace bench
