/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/frame.hpp"

#include <algorithm>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/crc.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::wire {
namespace {

// Read `n` little-endian bytes at `off` (a thin span adaptor over detail::load_le).
std::uint64_t read_le(std::span<const std::byte> b, std::size_t off, std::size_t n) noexcept {
    return detail::load_le(b.subspan(off, n));
}

void write_le(std::vector<std::byte>& out, std::uint64_t v, std::size_t n) {
    detail::append_le(out, v, n);
}

// One TLV parsed in isolation — NO recursion into children. For a structured TLV
// (opt.pl) `children` is the PL payload region, left for decode() to walk with an
// explicit stack; for an opaque TLV `tlv.payload` holds the bytes. `total` is the
// full encoded size (header + length + trailer).
struct parsed_t {
    tlv_t tlv;
    std::size_t total = 0;
    std::span<const std::byte> children{};
};

std::expected<parsed_t, err_t> parse_one(std::span<const std::byte> buf) {
    // The header/trailer grammar (incl. the two-span CRC verify) lives once in
    // grammar::parse_header (ADR-0048 §1); here we only MODEL the result as a
    // tlv_t — extracting the payload span and reading the (already-verified)
    // trailer values into the owning tree.
    const auto h = grammar::parse_header(grammar::span_cursor{buf});
    if (!h) return std::unexpected(h.error());

    tlv_t tlv;
    tlv.type = h->type;
    tlv.opt = h->opt;
    const std::span<const std::byte> payload = buf.subspan(h->header, h->length);

    if (h->opt.ts || h->opt.cr) {
        trailer_t trailer;
        if (h->opt.ts) {
            timestamp_t t;
            t.relative = h->opt.tf;
            if (h->opt.tf) {
                t.value = static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(read_le(buf, h->header + h->length, 4)));
            } else {
                t.value = static_cast<std::int64_t>(read_le(buf, h->header + h->length, 8));
            }
            trailer.ts = t;
        }
        if (h->opt.cr) {
            // CRC already verified in parse_header; read the stored value to model it.
            const std::size_t crc_off = h->header + h->length + h->ts_size;
            crc_t c;
            if (h->opt.cw) {
                c.width = crc_t::width_t::CRC16_CCITT;
                c.value = static_cast<std::uint32_t>(read_le(buf, crc_off, 2));
            } else {
                c.width = crc_t::width_t::CRC32C;
                c.value = static_cast<std::uint32_t>(read_le(buf, crc_off, 4));
            }
            trailer.crc = c;
        }
        tlv.trailer = trailer;
    }

    parsed_t out;
    out.total = h->total;
    if (h->opt.pl) {
        out.children = payload;  // walked iteratively by decode(), not here
    } else {
        tlv.payload = payload;
    }
    out.tlv = std::move(tlv);
    return out;
}

}  // namespace

std::expected<tlv_t, err_t> decode(std::span<const std::byte> input) {
    // Iterative parse with an explicit work stack — recursion is forbidden so a
    // maliciously deep frame cannot overflow a small MCU call stack
    // (docs/reference/01-data-format.md §Iterative parsing requirement).
    auto root = parse_one(input);
    if (!root) return std::unexpected(root.error());
    if (root->total != input.size())
        return std::unexpected(err_t::FRAME_INVALID);    // trailing bytes
    if (!root->tlv.opt.pl) return std::move(root->tlv);  // opaque root: done

    // An open structured node whose children are still being parsed. `pos` is the
    // cursor within `payload`; `total` is the node's full size, used to advance the
    // parent's cursor when this node closes.
    struct open_t {
        tlv_t node;
        std::span<const std::byte> payload;
        std::size_t pos = 0;
        std::size_t total = 0;
    };
    std::vector<open_t> stack;
    stack.push_back(open_t{std::move(root->tlv), root->children, 0, root->total});

    while (true) {
        if (stack.back().pos == stack.back().payload.size()) {
            // Node complete — pop it and graft it onto its parent (or return if root).
            open_t done = std::move(stack.back());
            stack.pop_back();
            if (stack.empty()) return std::move(done.node);
            stack.back().node.children.push_back(std::move(done.node));
            stack.back().pos += done.total;
            continue;
        }
        // A child of stack.back() sits at depth == stack.size(); reject at the cap.
        if (stack.size() >= kMaxDepth) return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);
        auto child = parse_one(stack.back().payload.subspan(stack.back().pos));
        if (!child) return std::unexpected(child.error());
        if (child->tlv.opt.pl) {
            // Structured child: push and descend (invalidates the reference above,
            // so the loop re-fetches stack.back()).
            stack.push_back(open_t{std::move(child->tlv), child->children, 0, child->total});
        } else {
            stack.back().node.children.push_back(std::move(child->tlv));
            stack.back().pos += child->total;
        }
    }
}

std::vector<std::byte> encode(const tlv_t& tlv) {
    std::vector<std::byte> body;
    if (tlv.opt.pl) {
        for (const tlv_t& child : tlv.children) {
            const std::vector<std::byte> cb = encode(child);
            body.insert(body.end(), cb.begin(), cb.end());
        }
    } else {
        body.assign(tlv.payload.begin(), tlv.payload.end());
    }

    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(std::to_underlying(tlv.type)));
    out.push_back(static_cast<std::byte>(tlv.opt.encode()));
    write_le(out, body.size(), tlv.opt.ll ? 4u : 2u);
    out.insert(out.end(), body.begin(), body.end());

    std::vector<std::byte> ts_bytes;
    if (tlv.opt.ts) {
        const timestamp_t t = (tlv.trailer && tlv.trailer->ts)
                                  ? *tlv.trailer->ts
                                  : timestamp_t{.relative = tlv.opt.tf, .value = 0};
        if (tlv.opt.tf) {
            write_le(ts_bytes, static_cast<std::uint32_t>(static_cast<std::int32_t>(t.value)), 4);
        } else {
            write_le(ts_bytes, static_cast<std::uint64_t>(t.value), 8);
        }
        out.insert(out.end(), ts_bytes.begin(), ts_bytes.end());
    }
    if (tlv.opt.cr) {
        // CRC over body ++ ts_bytes via the two-span overloads — no `covered`
        // concatenation buffer (byte-identical: CRC is associative over the feed).
        if (tlv.opt.cw) {
            write_le(out, crc::crc16_ccitt(body, ts_bytes), 2);
        } else {
            write_le(out, crc::crc32c(body, ts_bytes), 4);
        }
    }
    return out;
}

std::vector<std::byte> path_key(const tlv_t& path) {
    // The canonical PATH-payload key = the concatenated NAME-child encodings. Emit each
    // NAME TLV in place (wire::emit_name appends `02 00 <len> <bytes>` directly) instead
    // of encode()-per-child into a temporary vector — one reserve + N appends, no per-
    // segment allocation. A PATH's children are plain NAMEs (opt 0, no trailer), so this
    // is byte-identical to encode(name); and it matches what path_t/register_vertex store
    // (which also use emit_name), so the vertex-map key round-trips exactly.
    std::vector<std::byte> key;
    std::size_t total = 0;
    for (const tlv_t& name : path.children) total += 4 + name.payload.size();
    key.reserve(total);
    for (const tlv_t& name : path.children) wire::emit_name(key, name.payload);
    return key;
}

bool equal(const tlv_t& a, const tlv_t& b) noexcept {
    if (a.type != b.type || a.opt != b.opt || a.trailer != b.trailer) return false;
    if (!std::ranges::equal(a.payload, b.payload)) return false;
    if (a.children.size() != b.children.size()) return false;
    for (std::size_t i = 0; i < a.children.size(); ++i) {
        if (!equal(a.children[i], b.children[i])) return false;
    }
    return true;
}

}  // namespace tr::wire
