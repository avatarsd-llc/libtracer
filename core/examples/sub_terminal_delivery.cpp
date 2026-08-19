/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief Delivery terminates at the target — a chain of subscriptions does not relay
 *        (RFC-0007 / ADR-0051).
 *
 * `subscribe(src, target)` binds a target vertex: a write to `src` is DELIVERED at
 * `target` as an ordinary write (stored, ACL-checked, wakes `await`) and stops there.
 * The target's own `:subscribers[]` are not fanned out to, so `A -> B` plus
 * `B -> C` does not carry A's write to C, and a mutual `X <-> Y` pair cannot loop.
 * Propagation past a target is the act of the LOGIC behind it — a HANDLER re-emitting on
 * its own execution — never the runtime's.
 *
 * Runs under ctest as `example_sub_terminal_delivery`; it self-checks and returns non-zero
 * on any mismatch.
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
    const tr::graph::vertex_handle_t a = g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    const tr::graph::vertex_handle_t b = g.register_vertex(path_t("/b"), role_t::STORED_VALUE);

    int relayed = 0;
    auto on_b = [&](const tr::view::rope_t&) { ++relayed; };
    (void)g.subscribe(path_t("/a"), path_t("/b"));  // A -> B: a target-vertex edge
    (void)g.subscribe(path_t("/b"), on_b);          // an observer on B's OWN subscribers

    (void)g.write(a, value_byte(0x55));

    const auto stored = g.read(b);
    const int at_b = stored ? std::to_integer<int>((*stored)->only().bytes()[0]) : -1;
    std::printf("write /a = 0x55 -> /b stores 0x%02x; B's own subscribers fired %d time(s)\n", at_b,
                relayed);

    bool ok = true;
    check(ok, at_b == 0x55, "the delivery landed AT B as an ordinary write");
    check(ok, relayed == 0, "B does not relay to its own subscribers — delivery ends at B");
    std::printf("RESULT %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
