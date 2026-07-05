/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/rope_decode.hpp"

#include <vector>

#include "libtracer/frame.hpp"  // kMaxDepth (the shared nesting cap)

namespace tr::wire {

std::expected<void, err_t> validate_rope(const view::rope_t& r) {
    // A device link is not CPU-addressable, and the grammar reads payload bytes to
    // verify the CRC — a heterogeneous rope cannot be validated on the CPU
    // (docs/adr/0024); lower/flatten it via its device path first.
    if (!r.all_host()) return std::unexpected(err_t::FRAME_INVALID);

    const grammar::rope_cursor rc{r};
    const auto root = grammar::parse_header(rc);
    if (!root) return std::unexpected(root.error());
    if (root->total != rc.size()) return std::unexpected(err_t::FRAME_INVALID);  // trailing bytes
    if (!root->opt.pl) return {};  // opaque root: nothing to descend

    // Iterative descent mirroring decode_into (docs/reference/01 §Iterative parsing
    // requirement) — validate each child header in turn, never recursing so a
    // maliciously deep frame cannot overflow a small MCU stack. Each open node
    // carries a cursor bounded to its children region plus the walk position.
    struct open_t {
        grammar::rope_cursor body;
        std::size_t pos = 0;
    };
    std::vector<open_t> stack;
    stack.push_back(open_t{rc.region(root->header, root->length), 0});

    while (!stack.empty()) {
        open_t& top = stack.back();
        const std::size_t region_len = top.body.size();
        if (top.pos == region_len) {
            stack.pop_back();  // node complete
            continue;
        }
        // A child of stack.back() sits at depth == stack.size(); reject at the cap.
        if (stack.size() >= kMaxDepth) return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);
        const grammar::rope_cursor child_cur = top.body.region(top.pos, region_len - top.pos);
        const auto child = grammar::parse_header(child_cur);
        if (!child) return std::unexpected(child.error());
        top.pos += child->total;  // reference `top` is unused after the push below
        if (child->opt.pl) {
            stack.push_back(open_t{child_cur.region(child->header, child->length), 0});
        }
    }
    return {};
}

}  // namespace tr::wire
