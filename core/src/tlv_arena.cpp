/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/tlv_arena.hpp"

#include "libtracer/byteorder.hpp"
#include "libtracer/crc.hpp"

namespace tr::wire {
namespace {

constexpr std::uint8_t u8(std::byte b) noexcept { return std::to_integer<std::uint8_t>(b); }

// One TLV header parsed + trailer-validated in isolation — the arena twin of
// frame.cpp's parse_one, producing spans instead of a tlv_t. Kept byte-for-byte
// equivalent in what it accepts/rejects (the decode <-> decode_into equivalence
// test runs both over every conformance vector).
struct parsed_t {
    type_t type{};
    opt_t opt{};
    std::size_t total = 0;              // header + body + trailer
    std::span<const std::byte> wire{};  // header + body (trailer excluded)
    std::span<const std::byte> body{};  // payload / children region
};

std::expected<parsed_t, error_t> parse_header(std::span<const std::byte> buf) {
    if (buf.size() < 4) return std::unexpected(error_t::FRAME_TRUNCATED);

    const std::uint8_t type_b = u8(buf[0]);
    const std::uint8_t opt_b = u8(buf[1]);
    if (type_b == 0x00) return std::unexpected(error_t::FRAME_INVALID);
    if (opt_t::reserved_set(opt_b)) return std::unexpected(error_t::FRAME_INVALID);

    const opt_t opt = opt_t::decode(opt_b);
    const std::size_t header = opt.ll ? 6u : 4u;
    if (buf.size() < header) return std::unexpected(error_t::FRAME_TRUNCATED);

    const std::uint64_t length = detail::load_le(buf.subspan(2, opt.ll ? 4u : 2u));
    const std::size_t ts_size = opt.ts ? (opt.tf ? 4u : 8u) : 0u;
    const std::size_t crc_size = opt.cr ? (opt.cw ? 2u : 4u) : 0u;
    const std::size_t total = header + length + ts_size + crc_size;
    if (buf.size() < total) return std::unexpected(error_t::FRAME_TRUNCATED);

    const std::span<const std::byte> body = buf.subspan(header, length);
    if (opt.cr) {
        const std::span<const std::byte> ts_bytes = buf.subspan(header + length, ts_size);
        const std::size_t crc_off = header + length + ts_size;
        // CRC over body ++ ts_bytes via the two-span overloads (same as decode).
        if (opt.cw) {
            const auto stored =
                static_cast<std::uint16_t>(detail::load_le(buf.subspan(crc_off, 2)));
            if (crc::crc16_ccitt(body, ts_bytes) != stored)
                return std::unexpected(error_t::FRAME_CRC_FAIL);
        } else {
            const auto stored =
                static_cast<std::uint32_t>(detail::load_le(buf.subspan(crc_off, 4)));
            if (crc::crc32c(body, ts_bytes) != stored)
                return std::unexpected(error_t::FRAME_CRC_FAIL);
        }
    }

    return parsed_t{
        .type = static_cast<type_t>(type_b),
        .opt = opt,
        .total = total,
        .wire = buf.first(header + length),
        .body = body,
    };
}

// A bare canonical NAME: type 0x02 with opt byte 0x00 — exactly the
// `02 00 <u16 len>` header path_key emits, so a PATH made only of these has a
// body byte-identical to its canonical vertex-map key (ADR-0041 §3).
bool is_canonical_name(const parsed_t& p) noexcept {
    return p.type == type_t::NAME && p.opt == opt_t{};
}

}  // namespace

std::expected<tlv_arena_t, error_t> decode_into(std::span<const std::byte> input,
                                                std::pmr::memory_resource& mr) {
    // Iterative parse with an explicit open-node stack, mirroring decode()'s
    // walk (docs/reference/01 §Iterative parsing requirement) — but appending
    // pre-order arena nodes instead of grafting owning children.
    auto root = parse_header(input);
    if (!root) return std::unexpected(root.error());
    if (root->total != input.size())
        return std::unexpected(error_t::FRAME_INVALID);  // trailing bytes

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
        if (stack.size() >= kMaxDepth) return std::unexpected(error_t::TLV_NESTING_TOO_DEEP);
        const auto child = parse_header(top.payload.subspan(top.pos));
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
