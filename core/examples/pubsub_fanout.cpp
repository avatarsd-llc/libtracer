/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief Pub/sub fan-out — one publisher, a growing set of subscribers, and the
 *        per-delivery dispatch cost as fan-out scales.
 *
 * Every `write` fans out to every subscriber as a refcount-bump clone of the same
 * rope value — no byte copy per subscriber (`docs/modules/graph.md`). This example
 * registers one `/sensor/temp` vertex, then for each fan-out width in {1, 8, 64}
 * attaches that many callbacks, writes @p kWrites values, and reports the
 * per-delivery latency and delivery throughput. It also field-writes a QoS
 * setting and reads it back structurally via `:schema`.
 *
 * The RESULT perf lines are informational (CI never flakes on timing); the
 * self-checks guard that EVERY subscriber saw EVERY write. Runs under ctest as
 * `example_pubsub_fanout`.
 */

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "libtracer/tracer.hpp"

namespace {

using clock_t_ = std::chrono::steady_clock;
using tr::graph::path_t;
using tr::graph::role_t;

/** @brief A 4-byte little-endian VALUE view over @p v (one heap segment). */
tr::view::view_t value_u32(std::uint32_t v) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(4);
    for (int i = 0; i < 4; ++i)
        seg->bytes[static_cast<std::size_t>(i)] =
            static_cast<std::byte>((v >> (8 * i)) & 0xFF);
    return tr::view::view_t::over(std::move(seg));
}

/** @brief Record a failed expectation on @p ok and report it. */
void check(bool& ok, bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ok = false;
    }
}

/** @brief Run one fan-out width: @p fanout subscribers, @p writes writes; returns ok. */
bool run_fanout(std::size_t fanout, std::uint32_t writes, bool& ok) {
    tr::graph::graph_t g;
    tr::graph::vertex_t* v = *g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    // One delivery counter shared by every callback — each fires inline per write.
    std::uint64_t deliveries = 0;
    for (std::size_t s = 0; s < fanout; ++s)
        (void)g.subscribe(path_t("/sensor/temp"),
                          [&deliveries](const tr::view::rope_t&) { ++deliveries; });

    auto t0 = clock_t_::now();
    for (std::uint32_t i = 0; i < writes; ++i) (void)g.write(v, value_u32(i));
    auto t1 = clock_t_::now();

    const std::uint64_t expected = std::uint64_t(fanout) * writes;
    check(ok, deliveries == expected, "every subscriber received every write");

    const double total_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    const double per_delivery_ns = total_ns / double(expected);
    const double deliv_per_s = double(expected) / (total_ns * 1e-9);
    std::printf("RESULT pubsub_fanout subs=%zu writes=%u deliveries=%llu "
                "per_delivery_ns=%.1f deliveries_Mps=%.2f\n",
                fanout, writes, static_cast<unsigned long long>(deliveries),
                per_delivery_ns, deliv_per_s / 1e6);
    return ok;
}

}  // namespace

int main() {
    constexpr std::uint32_t kWrites = 5000;
    bool ok = true;

    std::printf("fan-out dispatch: each write clones the rope per subscriber "
                "(refcount bump, no byte copy)\n");
    for (std::size_t fanout : {std::size_t{1}, std::size_t{8}, std::size_t{64}})
        run_fanout(fanout, kWrites, ok);

    // Field-write a QoS setting, then discover the vertex shape via :schema.
    tr::graph::graph_t g;
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.write(path_t("/sensor/temp:settings.deadline_ns"), value_u32(5000));
    auto schema = g.read(path_t("/sensor/temp:schema"));
    std::size_t schema_children = 0;
    if (schema)
        if (auto point = tr::wire::view_as_tlv(schema->only()))
            schema_children = point->children.size();
    std::printf(":schema resolves to a POINT with %zu children after the field-write\n",
                schema_children);
    check(ok, schema_children == 2, ":schema is a 2-child POINT");

    std::printf("%s\n", ok ? "pub/sub fan-out OK" : "pub/sub fan-out FAILED");
    return ok ? 0 : 1;
}
