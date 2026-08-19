/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief One subscription edge covers a whole subtree — RFC-0005 vertical bubbling.
 *
 * Every subscription is a subtree subscription: an edge on `/dev` observes writes to
 * `/dev` AND to every descendant, so a composite needs one `SUBSCRIBER` rather than one
 * per leaf. The delivered value is the descendant's written TLV **as-is**, which is why
 * the subscriber cannot infer WHICH leaf produced it from the subscription — provenance
 * must ride in the delivered data (RFC-0003).
 *
 * Runs under ctest as `example_sub_subtree`; it self-checks and returns non-zero on any
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
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    const tr::graph::vertex_handle_t a =
        g.register_vertex(path_t("/dev/a/temp"), role_t::STORED_VALUE);
    const tr::graph::vertex_handle_t b =
        g.register_vertex(path_t("/dev/b/temp"), role_t::STORED_VALUE);

    int seen = 0;
    auto on_dev = [&](const tr::view::rope_t& v) {
        ++seen;
        // The value arrives as written; the edge carries no "which leaf" field.
        std::printf("  delivery %d: %u\n", seen,
                    std::to_integer<std::uint8_t>(v.only().bytes()[0]));
    };
    const auto sub = g.subscribe(path_t("/dev"), on_dev);  // ONE edge, on the parent

    (void)g.write(a, value_byte(0x11));
    (void)g.write(b, value_byte(0x22));

    bool ok = true;
    check(ok, sub.has_value(), "one subscribe on the parent");
    check(ok, seen == 2, "both descendants' writes bubbled to that one edge");
    check(ok, g.own_subs(a) == 0, "the leaf itself carries NO own subscriber slot");
    check(ok, g.has_subscribers(a), "but a delivery there would reach an ancestor subscriber");
    std::printf("RESULT %s (own_subs(/dev/a/temp)=%u, has_subscribers=%d)\n", ok ? "ok" : "FAILED",
                g.own_subs(a), static_cast<int>(g.has_subscribers(a)));
    return ok ? 0 : 1;
}
