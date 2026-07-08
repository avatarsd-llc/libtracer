/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/rope_decode.hpp"

#include <array>
#include <memory_resource>

namespace tr::wire {
namespace {

/**
 * @brief A `grammar::walk` sink that models nothing — `validate_rope`'s sink.
 *
 * The walk itself already performs the whole strict validation (every header,
 * every trailer CRC, trailing-bytes reject) — ADR-0048 §1's ONE grammar and ONE
 * descent, no second hand-written rope walk to hold equal by test.
 */
struct null_sink_t {
    /** @brief A structured TLV opened — nothing to model. */
    void on_open(const grammar::header_t&, const grammar::rope_cursor&) noexcept {}
    /** @brief An opaque TLV visited — nothing to model. */
    void on_leaf(const grammar::header_t&, const grammar::rope_cursor&) noexcept {}
    /** @brief The open node's children completed — nothing to seal. */
    void on_close() noexcept {}
};

}  // namespace

std::expected<void, err_t> check_frame(const view::rope_t& r) {
    // A device link is not CPU-addressable, and the root trailer CRC reads payload
    // bytes — a heterogeneous rope cannot be checked on the CPU (docs/adr/0024);
    // lower/flatten it via its device path first.
    if (!r.all_host()) return std::unexpected(err_t::FRAME_INVALID);
    const grammar::rope_cursor rc{r};
    // VERIFY (the default) = the root trailer CRC when opt.CR is set — a linear
    // link-by-link scan through the incremental CRC feed, never a tree walk.
    const auto root = grammar::parse_header(rc);
    if (!root) return std::unexpected(root.error());
    if (root->total != rc.size()) return std::unexpected(err_t::FRAME_INVALID);  // trailing bytes
    return {};
}

std::expected<void, err_t> validate_rope(const view::rope_t& r) {
    // Same all-host precondition as check_frame: the grammar reads payload bytes
    // to verify each CRC.
    if (!r.all_host()) return std::unexpected(err_t::FRAME_INVALID);
    // The walk stack starts inline and spills to the heap: this strict validator
    // is a host-side opt-in, so its RFC-0006 depth bound is the default resource
    // — matching decode(flatten(r)), whose verdict it must reproduce exactly.
    null_sink_t sink;
    std::array<grammar::walk_frame_t<grammar::rope_cursor>, 8> slots;
    grammar::walk_stack_t<grammar::rope_cursor> stack(slots, std::pmr::get_default_resource());
    return grammar::walk(grammar::rope_cursor{r}, sink, stack);
}

}  // namespace tr::wire
