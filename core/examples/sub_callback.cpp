/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief Subscribe to ONE vertex — the delivery-callback contract, and nothing else.
 *
 * `subscribe(src, callable)` is host-SDK sugar over the wire subscription (a
 * `SUBSCRIBER` field-write into `src:subscribers[]`, ADR-0049). It returns a
 * `subscription_t` handle, and from then on every write to `src` invokes the callback
 * **inline on the writing thread**, once per write, with the written rope value.
 *
 * Runs under ctest as `example_sub_callback`; it self-checks and returns non-zero on any
 * mismatch.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "libtracer/tracer.hpp"

namespace {

using tr::graph::path_t;
using tr::graph::role_t;

/** @brief A one-byte VALUE view over @p b (one heap segment). */
tr::view::view_t value_byte(std::uint8_t b) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(1);
    seg->bytes[0] = std::byte{b};
    return tr::view::view_t::over(std::move(seg));
}

/** @brief Record a failed expectation on @p ok and report it. */
void check(bool& ok, bool cond, const char* what) {
    if (!cond) {
        std::printf("  [FAIL] %s\n", what);
        ok = false;
    }
}

}  // namespace

int main() {
    tr::graph::graph_t g;
    const tr::graph::vertex_handle_t temp =
        g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    int deliveries = 0;
    std::uint8_t last = 0;
    // The callback is bound BY ADDRESS (lvalues only) and is the edge's `ctx`, so it must
    // outlive the subscription — here, main's frame.
    auto on_temp = [&](const tr::view::rope_t& v) {
        ++deliveries;
        last = std::to_integer<std::uint8_t>(v.only().bytes()[0]);
        std::printf("  delivery %d: %u\n", deliveries, last);
    };
    const auto sub = g.subscribe(path_t("/sensor/temp"), on_temp);

    std::printf("subscribe(/sensor/temp) -> %s\n", sub ? "ok" : "error");
    for (const std::uint8_t v : {std::uint8_t{7}, std::uint8_t{8}, std::uint8_t{9}})
        (void)g.write(temp, value_byte(v));

    bool ok = true;
    check(ok, sub.has_value(), "subscribe returns a subscription_t handle");
    check(ok, deliveries == 3, "one delivery per write, none extra");
    check(ok, last == 9, "the callback sees the written value");
    check(ok, g.own_subs(temp) == 1, "the vertex carries exactly one own subscriber slot");
    std::printf("RESULT %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
