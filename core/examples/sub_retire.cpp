/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief Retiring a producer drops its subscriptions — and delivers nothing (RFC-0009 §B).
 *
 * `retire(vh)` makes a vertex logically absent and **re-virginizes** it (§B.6): its `:acl`,
 * stored value, history and `:subscribers[]` are cleared, so a later revive of the same
 * address inherits nothing of the retired owner. It wakes no `await` and fans out nothing
 * (§B.5) — a subscriber learns a producer went away from the absence of writes, not from a
 * retirement event. Edges that live on an ANCESTOR are untouched: they belong to a vertex
 * that was not retired.
 *
 * This is the only owner-side eviction the reference implementation has. The per-subscriber
 * heartbeat that reference 04 §Liveness loss describes is NOT implemented — no `:liveness.*`
 * field exists in either direction (#586) — and the transport-level counterpart is
 * `graph_t::evict_link_edges` (RFC-0009 §D), which needs a link and so is not shown here.
 *
 * Runs under ctest as `example_sub_retire`; it self-checks and returns non-zero on any
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
    (void)g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    tr::graph::vertex_handle_t leaf = g.register_vertex(path_t("/p/leaf"), role_t::STORED_VALUE);

    int on_leaf_seen = 0, on_parent_seen = 0;
    auto on_leaf = [&](const tr::view::rope_t&) { ++on_leaf_seen; };
    auto on_parent = [&](const tr::view::rope_t&) { ++on_parent_seen; };
    (void)g.subscribe(path_t("/p/leaf"), on_leaf);  // an edge ON the doomed vertex
    (void)g.subscribe(path_t("/p"), on_parent);     // an edge on its surviving ancestor
    (void)g.write(leaf, value_byte(0x01));

    const std::uint32_t gen_before = g.retire_generation(leaf);
    (void)g.retire(leaf);
    const int leaf_after_retire = on_leaf_seen, parent_after_retire = on_parent_seen;

    // Revive the same address and write again: the retired owner's edge is gone, the
    // ancestor's is not.
    (void)g.try_register_vertex(path_t("/p/leaf"), role_t::STORED_VALUE);
    (void)g.write(path_t("/p/leaf"), value_byte(0x02));
    std::printf("leaf edge: %d -> %d deliveries; ancestor edge: %d -> %d; generation %u -> %u\n",
                leaf_after_retire, on_leaf_seen, parent_after_retire, on_parent_seen, gen_before,
                g.retire_generation(leaf));

    bool ok = true;
    check(ok, leaf_after_retire == 1 && parent_after_retire == 1, "retirement delivered nothing");
    check(ok, g.own_subs(leaf) == 0, "the revived vertex carries none of the retired subscribers");
    check(ok, on_leaf_seen == 1, "the retired vertex's own edge did not survive the revive");
    check(ok, on_parent_seen == 2, "the ancestor's edge did — it was never retired");
    check(ok, g.retire_generation(leaf) != gen_before, "the retirement generation moved");
    std::printf("RESULT %s\n", ok ? "ok" : "FAILED");
    return ok ? 0 : 1;
}
