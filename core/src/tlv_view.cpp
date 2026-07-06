/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/tlv_view.hpp"

#include <utility>

namespace tr::wire {

std::expected<tlv_view_t, err_t> tlv_view_t::over(view::rope_t frame) {
    // CPU-side lazy reads dereference link bytes, so a DEVICE link is rejected
    // up front — same rule (and same code) as validate_rope.
    if (!frame.all_host()) return std::unexpected(err_t::FRAME_INVALID);

    // The bounds anchor (ADR-0053 §4): root header with the CRC walk DEFERRED,
    // then the exact-total check decode() applies ("trailing bytes").
    const auto h = grammar::parse_header(grammar::rope_cursor{frame}, grammar::crc_check_t::DEFER);
    if (!h) return std::unexpected(h.error());
    if (h->total != frame.total_length()) return std::unexpected(err_t::FRAME_INVALID);

    return tlv_view_t(std::move(frame), *h);
}

std::expected<std::optional<tlv_view_t>, err_t> tlv_view_t::children_t::next() {
    if (poisoned_) return std::unexpected(*poisoned_);
    const std::size_t len = body_.total_length();
    if (pos_ == len) return std::nullopt;

    // Parse exactly ONE child header (CRC deferred). Containment: the cursor
    // region ends at the parent's body end, so a child whose declared total
    // overruns it is FRAME_TRUNCATED — the lazy analogue of decode()'s
    // subspan-bounded parse_one.
    const grammar::rope_cursor cur = grammar::rope_cursor{body_}.region(pos_, len - pos_);
    const auto h = grammar::parse_header(cur, grammar::crc_check_t::DEFER);
    if (!h) {
        // Child boundaries beyond a malformed header are unknowable: poison.
        poisoned_ = h.error();
        return std::unexpected(*poisoned_);
    }

    tlv_view_t child(body_.subrope(pos_, h->total), *h);
    pos_ += h->total;
    return std::optional<tlv_view_t>(std::move(child));
}

std::expected<void, err_t> tlv_view_t::verify() const {
    if (!hdr_.opt.cr) return {};
    // Re-run the header parse in VERIFY mode: the CRC feed (body ++ timestamp,
    // link-by-link, no concatenation) plus the stored-value compare live once,
    // in the grammar core — this is that same code path, just deferred to the
    // access point (ADR-0053 §4).
    const auto h = grammar::parse_header(grammar::rope_cursor{wire_}, grammar::crc_check_t::VERIFY);
    if (!h) return std::unexpected(h.error());
    return {};
}

std::optional<timestamp_t> tlv_view_t::timestamp() const {
    if (!hdr_.opt.ts) return std::nullopt;
    const grammar::rope_cursor cur{wire_};
    const std::size_t off = hdr_.header + hdr_.length;
    timestamp_t t;
    t.relative = hdr_.opt.tf;
    if (hdr_.opt.tf) {
        t.value = static_cast<std::int32_t>(static_cast<std::uint32_t>(cur.load_le(off, 4)));
    } else {
        t.value = static_cast<std::int64_t>(cur.load_le(off, 8));
    }
    return t;
}

std::expected<tlv_view_t::materialized_t, err_t> tlv_view_t::materialize(
    mem::mem_backend_t& backend) const {
    view::view_t flat = wire_.flatten(backend);
    if (flat.empty() && hdr_.total != 0) {
        return std::unexpected(err_t::FRAME_INVALID);  // allocation failed
    }
    auto tree = decode(flat.bytes());
    if (!tree) return std::unexpected(tree.error());
    return materialized_t{std::move(flat), std::move(*tree)};
}

}  // namespace tr::wire
