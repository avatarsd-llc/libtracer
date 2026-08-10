/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/tlv_view.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <utility>

namespace tr::wire {

namespace {

/**
 * @brief `rope_t::subrope(off, len)` resuming from a known link anchor (#917).
 *
 * `subrope` takes an absolute offset and re-walks the chain from link 0 to reach it,
 * so calling it once per child costs Θ(children × links). The lazy child iterator
 * already holds the anchor of the child it is about to yield, so it carves from there
 * and the whole sweep costs O(links). Identical result — the covering links, trimmed
 * with `subview`, refcounted, never copied.
 */
[[nodiscard]] view::rope_t subrope_at(std::span<const view::view_t> links, std::size_t li,
                                      std::size_t intra, std::size_t len) {
    view::rope_t out;
    std::size_t remaining = len;
    for (; remaining > 0 && li < links.size(); ++li, intra = 0) {
        const std::size_t have = links[li].length;
        // A zero-length link (or an anchor sitting exactly at a link's end) contributes
        // nothing and is stepped over — the same skip `subrope`'s own walk performs.
        const std::size_t avail = have > intra ? have - intra : 0;
        const std::size_t take = std::min(remaining, avail);
        if (take > 0) out.append(links[li].subview(intra, take));
        remaining -= take;
    }
    return out;
}

}  // namespace

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
    if (pos_ == len_) return std::nullopt;

    const std::span<const view::view_t> links = body_.links();

    // Parse exactly ONE child header (CRC deferred). Containment: the cursor
    // region ends at the parent's body end, so a child whose declared total
    // overruns it is FRAME_TRUNCATED — the lazy analogue of decode()'s
    // subspan-bounded parse_one. The cursor RESUMES at the anchor the previous
    // call left, so it re-enters the chain at this child instead of walking to it
    // from link 0 (#917).
    const grammar::rope_cursor cur = grammar::rope_cursor::at(links, li_, intra_, len_ - pos_);
    const auto h = grammar::parse_header(cur, grammar::crc_check_t::DEFER);
    if (!h) {
        // Child boundaries beyond a malformed header are unknowable: poison.
        poisoned_ = h.error();
        return std::unexpected(*poisoned_);
    }

    tlv_view_t child(subrope_at(links, li_, intra_, h->total), *h);
    // Step the anchor over the child just yielded — the only walk this call pays,
    // and it visits each link at most once across the whole sweep.
    pos_ += h->total;
    intra_ += h->total;
    while (li_ < links.size() && intra_ >= links[li_].length) {
        intra_ -= links[li_].length;
        ++li_;
    }
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
