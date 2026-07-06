/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The one wire-grammar core (ADR-0048 §1): the TLV header/trailer rules —
 * type-0x00 reject, reserved-bit reject, `LL` length width, trailer sizing, the
 * two-span CRC — parsed + validated in ONE place, read through a small
 * chunk-cursor so the same rules serve every byte source. Both materializing
 * decoders funnel through it: the owning `tlv_t` tree (frame.cpp `decode`) and
 * the terminus arena (tlv_arena.cpp `decode_into`). Previously this grammar was
 * forked (`parse_one` vs `parse_header`), held byte-for-byte equal only by the
 * decode<->decode_into equivalence test — every future rule was a two-file edit.
 *
 * The cursor is the byte-SOURCE seam, not an access-model one: `span_cursor` is
 * the contiguous case (today's path, zero new cost); the rope cursor
 * (link-walking with a straddled-header scratch) plugs in here for the
 * rope-aware decode ADR-0048 §1 commits to, without touching these rules.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "libtracer/byteorder.hpp"
#include "libtracer/crc.hpp"
#include "libtracer/error.hpp"
#include "libtracer/tlv.hpp"

/**
 * @file
 * @brief The shared L2/L3 (`tr::wire`) TLV header/trailer grammar (ADR-0048 §1).
 */

// A dedicated `grammar` sub-namespace (not `detail`) so wire-layer code keeps
// resolving `detail::` to the layer-free `tr::detail` byte helpers.
namespace tr::wire::grammar {

/**
 * @brief The contiguous byte-source cursor — the grammar's only source today.
 *
 * A thin adaptor over one `std::span`: the grammar reads its bytes through this
 * seam (@ref parse_header) so the identical rules serve a rope cursor
 * (link-walking, ADR-0048 §1) once that lands, with no rule change here.
 */
struct span_cursor {
    std::span<const std::byte> buf; /**< @brief The bytes this cursor reads over. */

    /** @brief Number of bytes available from the TLV's start. */
    [[nodiscard]] std::size_t size() const noexcept { return buf.size(); }
    /**
     * @brief A sub-cursor over the `[off, off + len)` window of this cursor.
     *
     * The contiguous analogue of @ref rope_cursor::region — a plain `subspan`, so
     * the same cursor-generic code (a forward-plane header read, a child descent)
     * narrows either source with one call.
     */
    [[nodiscard]] span_cursor region(std::size_t off, std::size_t len) const noexcept {
        return span_cursor{buf.subspan(off, len)};
    }
    /** @brief The unsigned byte at offset @p off. */
    [[nodiscard]] std::uint8_t byte_at(std::size_t off) const noexcept {
        return std::to_integer<std::uint8_t>(buf[off]);
    }
    /** @brief Load @p n little-endian bytes at @p off as a u64 (byteorder.hpp). */
    [[nodiscard]] std::uint64_t load_le(std::size_t off, std::size_t n) const noexcept {
        return tr::detail::load_le(buf.subspan(off, n));
    }
    /**
     * @brief Visit the @p n bytes at @p off as contiguous sub-spans, in order.
     *
     * The CRC-feed seam (@ref parse_header): a contiguous source yields exactly
     * one span, so this is a straight call; the rope cursor yields one span per
     * straddled link, letting the identical feed cross a link boundary with no
     * concatenation buffer.
     */
    template <class Fn>
    void for_each_span(std::size_t off, std::size_t n, Fn&& fn) const {
        fn(buf.subspan(off, n));
    }
};

/**
 * @brief When @ref parse_header checks a CRC trailer (ADR-0053 §4).
 *
 * `VERIFY` is the eager decoders' policy (and the default — every pre-existing
 * caller is unchanged): the trailer is checked during the header parse, which
 * walks the whole payload. `DEFER` is the lazy tier's policy: sizing and bounds
 * are validated but the payload is never touched, so iterating past a sibling
 * costs O(header) — integrity is checked by whichever consumer *accesses* the
 * TLV (`tlv_view_t::verify`), per the end-to-end argument.
 */
enum class crc_check_t : std::uint8_t {
    VERIFY, /**< @brief Check the CRC trailer now (walks the payload). */
    DEFER,  /**< @brief Skip the CRC walk; integrity is the accessor's to check. */
};

/**
 * @brief A validated TLV header + trailer: the sink-neutral parse result.
 *
 * Offsets/sizes only (no payload span), so each sink extracts the spans it wants
 * — the owning tree its `payload`/`trailer`, the arena its trailer-excluded
 * `wire` + `body` — from the TLV's own bytes. All offsets are relative to the
 * TLV's start.
 */
struct header_t {
    type_t type{};            /**< @brief The TLV type code (never 0x00). */
    opt_t opt{};              /**< @brief The decoded `opt` bits. */
    std::size_t header = 0;   /**< @brief Header length: 4, or 6 with the `LL` bit. */
    std::size_t length = 0;   /**< @brief Body (payload / children region) length. */
    std::size_t ts_size = 0;  /**< @brief Timestamp-trailer bytes (0 / 4 / 8). */
    std::size_t crc_size = 0; /**< @brief CRC-trailer bytes (0 / 2 / 4). */
    std::size_t total = 0;    /**< @brief Full encoded size: header + body + trailer. */
};

/**
 * @brief Parse + validate ONE TLV header and its trailer at offset 0 of @p cur.
 *
 * Applies the whole grammar — minimum size, `type == 0x00` reject, reserved-bit
 * reject, `LL` length width, trailer sizing, and the two-span CRC (payload ++
 * timestamp, fed without concatenation) — but does **not** recurse into a
 * structured payload's children; the sink's iterative walk does that. On success
 * the trailer has already been CRC-verified; the caller only re-reads the stored
 * timestamp/CRC bytes it wants to model.
 *
 * @tparam Cursor A byte-source cursor (@ref span_cursor, or the rope cursor).
 * @param  cur    The cursor positioned at the TLV's first byte.
 * @param  crc    CRC-trailer policy (@ref crc_check_t). Defaults to `VERIFY`
 *                (the eager decoders' behavior); the lazy tier passes `DEFER`
 *                so skipping a sibling never walks its payload (ADR-0053 §4).
 * @return The validated @ref header_t, or the `err_t` the grammar rejects with
 *         (`FRAME_TRUNCATED` / `FRAME_INVALID` / `FRAME_CRC_FAIL`).
 */
template <class Cursor>
[[nodiscard]] std::expected<header_t, err_t> parse_header(
    const Cursor& cur, crc_check_t crc_policy = crc_check_t::VERIFY) {
    const std::size_t avail = cur.size();
    if (avail < 4) return std::unexpected(err_t::FRAME_TRUNCATED);

    const std::uint8_t type_b = cur.byte_at(0);
    const std::uint8_t opt_b = cur.byte_at(1);
    if (type_b == 0x00) return std::unexpected(err_t::FRAME_INVALID);
    if (opt_t::reserved_set(opt_b)) return std::unexpected(err_t::FRAME_INVALID);

    const opt_t opt = opt_t::decode(opt_b);
    const std::size_t header = opt.ll ? 6u : 4u;
    if (avail < header) return std::unexpected(err_t::FRAME_TRUNCATED);

    const std::uint64_t length = cur.load_le(2, opt.ll ? 4u : 2u);
    const std::size_t ts_size = opt.ts ? (opt.tf ? 4u : 8u) : 0u;
    const std::size_t crc_size = opt.cr ? (opt.cw ? 2u : 4u) : 0u;
    const std::size_t total = static_cast<std::size_t>(header + length + ts_size + crc_size);
    if (avail < total) return std::unexpected(err_t::FRAME_TRUNCATED);

    if (opt.cr && crc_policy == crc_check_t::VERIFY) {
        const std::size_t pay_len = static_cast<std::size_t>(length);
        const std::size_t crc_off = header + pay_len + ts_size;
        // CRC feed = payload ++ timestamp bytes, fed incrementally across whatever
        // contiguous chunks the cursor yields — one span for a contiguous source,
        // one per straddled link for a rope. Byte-identical to the two-span
        // crc*(a, b) (the CRC is associative over the feed) with no `covered`
        // concatenation buffer; a rope payload never has to flatten to be checked.
        if (opt.cw) {
            crc::crc16_ccitt_state crc;
            const auto feed = [&crc](std::span<const std::byte> s) { crc.feed(s); };
            cur.for_each_span(header, pay_len, feed);
            cur.for_each_span(header + pay_len, ts_size, feed);
            if (crc.value() != static_cast<std::uint16_t>(cur.load_le(crc_off, 2))) {
                return std::unexpected(err_t::FRAME_CRC_FAIL);
            }
        } else {
            crc::crc32c_state crc;
            const auto feed = [&crc](std::span<const std::byte> s) { crc.feed(s); };
            cur.for_each_span(header, pay_len, feed);
            cur.for_each_span(header + pay_len, ts_size, feed);
            if (crc.value() != static_cast<std::uint32_t>(cur.load_le(crc_off, 4))) {
                return std::unexpected(err_t::FRAME_CRC_FAIL);
            }
        }
    }

    return header_t{
        .type = static_cast<type_t>(type_b),
        .opt = opt,
        .header = header,
        .length = static_cast<std::size_t>(length),
        .ts_size = ts_size,
        .crc_size = crc_size,
        .total = total,
    };
}

}  // namespace tr::wire::grammar
