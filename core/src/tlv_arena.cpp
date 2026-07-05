/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/tlv_arena.hpp"

#include "libtracer/grammar.hpp"

namespace tr::wire {
namespace {

// One TLV header parsed via the shared grammar (grammar.hpp, ADR-0048 §1),
// reshaped into the arena's span view. `wire` is header+body (trailer excluded,
// so a whole-TLV copy is trailer-less at rest); `body` is the payload / children
// region. The header/trailer rules + two-span CRC verify are no longer forked
// here — this is the arena twin of frame.cpp's parse_one, both delegating.
struct parsed_t {
    type_t type{};
    opt_t opt{};
    std::size_t total = 0;              // header + body + trailer
    std::span<const std::byte> wire{};  // header + body (trailer excluded)
    std::span<const std::byte> body{};  // payload / children region
};

std::expected<parsed_t, err_t> parse_node(std::span<const std::byte> buf) {
    const auto h = grammar::parse_header(grammar::span_cursor{buf});
    if (!h) return std::unexpected(h.error());
    return parsed_t{
        .type = h->type,
        .opt = h->opt,
        .total = h->total,
        .wire = buf.first(h->header + h->length),
        .body = buf.subspan(h->header, h->length),
    };
}

// A bare canonical NAME: type 0x02 with opt byte 0x00 — exactly the
// `02 00 <u16 len>` header path_key emits, so a PATH made only of these has a
// body byte-identical to its canonical vertex-map key (ADR-0041 §3).
bool is_canonical_name(const parsed_t& p) noexcept {
    return p.type == type_t::NAME && p.opt == opt_t{};
}

}  // namespace

std::expected<tlv_arena_t, err_t> decode_into(std::span<const std::byte> input,
                                              std::pmr::memory_resource& mr) {
    // Iterative parse with an explicit open-node stack, mirroring decode()'s
    // walk (docs/reference/01 §Iterative parsing requirement) — but appending
    // pre-order arena nodes instead of grafting owning children.
    auto root = parse_node(input);
    if (!root) return std::unexpected(root.error());
    if (root->total != input.size())
        return std::unexpected(err_t::FRAME_INVALID);  // trailing bytes

    tlv_arena_t arena(mr);
    auto& nodes = arena.nodes_;

    const auto push_node = [&nodes](const parsed_t& p) {
        nodes.push_back(arena_tlv_t{
            .type = p.type,
            .opt = p.opt,
            .wire = p.wire,
            .body = p.body,
            .end = static_cast<std::uint32_t>(nodes.size() + 1),  // opaque default
            .canonical_path = false,
        });
    };

    push_node(*root);
    if (!root->opt.pl) return arena;  // opaque root: done

    // An open structured node whose children are still being appended. `pos` is
    // the cursor within the node's children region; `names_only` tracks the
    // ADR-0041 §3 canonical-PATH property over its direct children.
    struct open_t {
        std::uint32_t index = 0;
        std::span<const std::byte> payload{};
        std::size_t pos = 0;
        bool names_only = true;
    };
    std::pmr::vector<open_t> stack(&mr);
    // Reserve for the typical FWD nesting (~3-4), not kMaxDepth: a full-depth
    // reserve would draw ~1.3 KiB from the resource on EVERY decode, which a
    // 16 KB-slab node cannot spare; deeper frames grow (bounded by the cap).
    stack.reserve(8);
    stack.push_back(open_t{.index = 0, .payload = root->body});

    while (!stack.empty()) {
        open_t& top = stack.back();
        if (top.pos == top.payload.size()) {
            // Node complete — seal its subtree extent and the canonical flag.
            arena_tlv_t& node = nodes[top.index];
            node.end = static_cast<std::uint32_t>(nodes.size());
            node.canonical_path = node.type == type_t::PATH && top.names_only;
            stack.pop_back();
            continue;
        }
        // A child of stack.back() sits at depth == stack.size(); reject at the cap.
        if (stack.size() >= kMaxDepth) return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);
        const auto child = parse_node(top.payload.subspan(top.pos));
        if (!child) return std::unexpected(child.error());
        top.pos += child->total;
        if (!is_canonical_name(*child)) top.names_only = false;
        const auto index = static_cast<std::uint32_t>(nodes.size());
        push_node(*child);
        if (child->opt.pl) stack.push_back(open_t{.index = index, .payload = child->body});
    }
    return arena;
}

}  // namespace tr::wire
