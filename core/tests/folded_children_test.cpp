/**
 * @file
 * @brief L4 fold, Slice 0 — the folded `:children` projection is byte-identical to the
 *        materialized serialize, and is a genuine scatter-gather rope a cursor walks.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The differential oracle (modeled on op_resolve_view_test): for many graph shapes,
 * `graph_t::read_children_folded(v).flatten()` must equal the materialized
 * `read(v, ":children")` bytes exactly, while the folded rope is genuinely multi-link
 * (one outer POINT header + one link per member — NOT a single flat buffer) and walks
 * cleanly through the lazy `wire::tlv_view_t` cursor as a POINT of POINT{NAME} children.
 * The materialized tree stays the source of truth; the fold is a read-only projection.
 */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tlv_view.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::view::rope_t;
using tr::view::view_t;
using tr::wire::tlv_view_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

bool same_bytes(const view_t& a, const view_t& b) {
    return a.bytes().size() == b.bytes().size() &&
           std::memcmp(a.bytes().data(), b.bytes().data(), a.bytes().size()) == 0;
}

/** @brief Walk a rope as one POINT via the lazy cursor, counting its POINT children.
 *         Returns -1 on any grammar error or a non-POINT child (a test failure signal). */
long walk_point_children(const rope_t& r) {
    auto tv = tlv_view_t::over(r);
    if (!tv || tv->type() != type_t::POINT || !tv->structured()) return -1;
    auto kids = tv->children();
    long n = 0;
    for (;;) {
        auto next = kids.next();
        if (!next) return -1;           // grammar error mid-walk
        if (!next->has_value()) break;  // region cleanly exhausted
        if ((*next)->type() != type_t::POINT) return -1;
        ++n;
    }
    return n;
}

/** @brief The whole Slice-0 contract for one parent: fold == materialized, and the fold
 *         is a real N+1-link rope the cursor walks. */
void assert_parent(graph_t& g, std::string_view parent) {
    std::printf("Parent %.*s:\n", static_cast<int>(parent.size()), parent.data());

    const auto pk = path_t::parse(parent);
    if (!pk) {
        check(false, "parent path parses");
        return;
    }
    const auto h = g.find(pk->key());
    if (!h) {
        check(false, "parent resolves to a handle");
        return;
    }

    const auto materialized = g.read_children_materialized(*h);  // the flat oracle
    const auto folded = g.read_children_folded(*h);              // the folded projection
    if (!materialized || !folded) {
        check(materialized.has_value(), "materialized :children read succeeds");
        check(folded.has_value(), "folded :children read succeeds");
        return;
    }

    // The PRODUCTION field read serves the fold (not the flat oracle): same links,
    // same bytes as read_children_folded.
    std::string field(parent);
    field += ":children";
    const auto production = g.read(path_t(field));
    check(production.has_value(), "production :children read succeeds");
    if (production) {
        check(production->link_count() == folded->link_count(),
              "production :children read IS the folded rope (link count)");
        check(same_bytes(production->flatten(), folded->flatten()),
              "production :children read matches the fold byte-for-byte");
    }

    const view_t mv = materialized->flatten();
    const view_t fv = folded->flatten();
    check(same_bytes(mv, fv), "folded.flatten() is byte-identical to materialized read_children");

    // Genuine scatter-gather: outer POINT header link + one link per member. The child
    // count is taken from the materialized bytes (the oracle), so this holds regardless
    // of how many children are registered vs. placeholder.
    const long n_children = walk_point_children(*materialized);
    check(n_children >= 0, "materialized listing walks as a POINT of POINT children");
    if (n_children >= 0)
        check(folded->link_count() == 2 * static_cast<std::size_t>(n_children) + 1,
              "folded rope is 2N+1 links (outer header + per-child header + borrowed name)");

    // The lazy cursor walks the FOLDED ROPE itself (not a flattened copy) to the same shape.
    const long n_folded = walk_point_children(*folded);
    check(n_folded == n_children, "tlv_view cursor walks the folded rope to the same child count");
}

void test_several_children() {
    graph_t g;
    (void)g.register_vertex(path_t("/a"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/a/x"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/a/y"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/a/z"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/a/x/deep"),
                            role_t::STORED_VALUE);  // only DIRECT children list
    assert_parent(g, "/a");
}

void test_no_children() {
    graph_t g;
    (void)g.register_vertex(path_t("/leaf"), role_t::STORED_VALUE);
    assert_parent(g, "/leaf");  // header-only rope, len 0 — the boundary case
}

void test_one_child() {
    graph_t g;
    (void)g.register_vertex(path_t("/p"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/p/only"), role_t::STORED_VALUE);
    assert_parent(g, "/p");
}

void test_varied_name_lengths() {
    graph_t g;
    (void)g.register_vertex(path_t("/c"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/c/s"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/c/medium_name"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/c/a_rather_long_child_name_0123456789"), role_t::STORED_VALUE);
    assert_parent(g, "/c");
}

void test_in_band_created_children() {
    // Children born via the :children[] SPEC write — the same path production the wire uses.
    graph_t g;
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, "stored_value");
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, "temp");
    std::vector<std::byte> spec;
    tr::wire::emit_tlv(spec, type_t::SPEC, tr::wire::opt_t{.pl = true}, body);
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(spec.size());
    std::memcpy(seg->bytes.data(), spec.data(), spec.size());
    (void)g.write(path_t("/dev:children[]"), view_t::over(std::move(seg)));
    assert_parent(g, "/dev");
}

}  // namespace

int main() {
    test_several_children();
    test_no_children();
    test_one_child();
    test_varied_name_lengths();
    test_in_band_created_children();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
