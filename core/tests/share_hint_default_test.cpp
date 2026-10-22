/**
 * @file
 * @brief #2226 — the build-wide copy-or-share default (`config_t::kDefaultShareHint`), over a
 *        `COPY` build on each default root (slab pool, static arena, heap root).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Linked against the `core/tests/copy_default` copies of the core only. A vertex that declares
 * no hint copies even where the block source's trait would share (an exact-size receive block),
 * a vertex that declares `SHARE` shares, and a default policy returns the vertex to the build's
 * default without drawing an extension block. The default build's half of the contract (an
 * undeclared vertex answers `DEFAULT` and follows the trait) is `op_resolve_test`'s
 * `store/share-hint` case.
 */

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::op_resolver_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::share_hint_t;
using tr::testing::b_fwd;
using tr::testing::check;
using tr::testing::make_value;
using tr::wire::opt_t;
using tr::wire::type_t;

static_assert(tr::graph::kDefaultShareHint == share_hint_t::COPY,
              "share_hint_default_test links only the copy_default preset");

/** @brief A canonical PATH TLV of @p segs. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief The root this binary's core was built over, for the log line. */
const char* root_name() {
    if constexpr (tr::mem::kSlabPool) return "slab pool";
    if constexpr (tr::mem::kHeapRoot) return "heap root";
    return "static arena";
}

/**
 * @brief Build-wide `COPY`: an undeclared vertex copies, a `SHARE` vertex shares, and a default
 *        policy restores the build's default.
 */
void test_copy_build_wide() {
    std::printf("kDefaultShareHint = COPY on the %s (#2226):\n", root_name());
    graph_t g;
    op_resolver_t resolver(g);
    const tr::graph::vertex_handle_t v =
        g.register_vertex(*path_t::parse("/knob/gain"), role_t::STORED_VALUE);

    std::vector<std::byte> payload(32);
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<std::byte>(i);
    std::vector<std::byte> value;
    tr::wire::emit_tlv(value, type_t::VALUE, opt_t{}, payload);
    const auto fwd =
        b_fwd(fwd_op_t::WRITE, b_path({"knob", "gain"}), b_path({"reply-ep"}), {}, value);
    // An OWNING frame in a block of its exact size (what frame ingress draws): the block
    // source's trait shares it on every root, so only the hint can make it copy.
    const tr::view::view_t frame = make_value(fwd);
    // Whether the WRITE stored a reference to the frame's own block.
    const auto shares = [&] {
        const auto arena = tr::wire::decode_into(frame.bytes(), tr::mem::heap_source());
        check(arena.has_value() && resolver.resolve(*arena, {}, &frame).has_value(),
              "the WRITE replied");
        const auto rd = g.read(v);
        check(rd.has_value() && (*rd)->only().bytes().size() == value.size(),
              "the stored value is the payload TLV");
        return rd.has_value() && (*rd)->only().owner.get() == frame.owner.get();
    };

    check(g.share_hint(v) == share_hint_t::COPY, "an undeclared vertex answers the build's COPY");
    check(!shares(), "an undeclared vertex copies an exact-size receive block");

    (void)g.set_policy(v, {.share_hint = share_hint_t::SHARE});
    check(g.share_hint(v) == share_hint_t::SHARE, "the declaration takes SHARE");
    check(shares(), "a SHARE vertex keeps its receive block");

    (void)g.set_policy(v, {.share_hint = share_hint_t::DEFAULT});
    check(g.share_hint(v) == share_hint_t::COPY, "naming DEFAULT restores the build's COPY");
    check(!shares(), "... and the vertex copies again");

    // A default policy on a fresh vertex is the build's default already: nothing moves.
    const tr::graph::vertex_handle_t fresh =
        g.register_vertex(*path_t::parse("/knob/bias"), role_t::STORED_VALUE);
    check(g.set_policy(fresh, {}).has_value() && g.share_hint(fresh) == share_hint_t::COPY,
          "a default policy on a fresh vertex keeps the build's COPY");
    check(!std::bit_cast<tr::graph::vertex_t*>(fresh)->has_extension_block(),
          "... and draws no extension block");
    (void)g.set_policy(fresh, {.share_hint = share_hint_t::COPY});
    check(!std::bit_cast<tr::graph::vertex_t*>(fresh)->has_extension_block(),
          "declaring the build's own default draws none either");
}

}  // namespace

int main() {
    test_copy_build_wide();
    return tr::testing::summary("share_hint_default");
}
