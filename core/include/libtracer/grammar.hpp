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
#include <memory_resource>
#include <span>
#include <vector>

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
 * seam (`parse_header`) so the identical rules serve a rope cursor
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
     * The CRC-feed seam (`parse_header`): a contiguous source yields exactly
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
 * @brief When `parse_header` checks a CRC trailer (ADR-0053 §4).
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
 * @param  crc_policy CRC-trailer policy (@ref crc_check_t). Defaults to `VERIFY`
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

/**
 * @brief Drive the iterative TLV descent, modelling each node through @p sink
 *        (ADR-0048 §1 — the ONE structural walk).
 *
 * The recursion-free open-node stack machine that turns validated headers into a
 * tree, shared by both materializing decoders: the owning `tlv_t` tree
 * (`frame.cpp decode`) and the terminus arena (`tlv_arena.cpp decode_into`).
 * ADR-0048 §1 unified the header *grammar* (@ref parse_header); this unifies the
 * *descent* that was still hand-written twice, held equal only by the
 * decode↔decode_into equivalence test. Only the SINK differs — grafting owning
 * children vs appending pre-order arena nodes.
 *
 * Recursion is forbidden (a malicious deep frame must not overflow a small MCU
 * call stack, docs/reference/01 §Iterative parsing requirement): the walk keeps
 * an explicit cursor/pos stack drawn from @p stack_mr, so a slab-bound decode
 * (the terminus arena) stays heap-free by passing its own resource.
 *
 * The sink models each node through three hooks (all balanced LIFO):
 * - `on_open(const header_t&, const Cursor& node)` — a structured TLV opens;
 * - `on_leaf(const header_t&, const Cursor& node)` — an opaque TLV;
 * - `on_close()`                                    — the current open node's
 *   children are complete.
 * `node` is a cursor over that TLV's OWN bytes (offset 0 = its first byte), from
 * which the sink extracts the spans it wants (payload / wire / trailer) using the
 * header's offsets.
 *
 * @tparam Cursor A byte-source cursor (@ref span_cursor, or the rope cursor).
 * @tparam Sink   A type providing the three hooks above.
 * @param root      The cursor positioned at the frame's first byte.
 * @param sink      The node model (built as the walk visits).
 * @param stack_mr  Backs the walk's internal cursor stack (pass the arena's
 *                  resource to keep a slab-bound decode heap-free; the default
 *                  resource for an ordinary heap decode).
 * @param max_depth The nesting cap: a child at depth `>= max_depth` is rejected
 *                  with `TLV_NESTING_TOO_DEEP` (callers pass `wire::kMaxDepth`).
 * @param crc_policy CRC-trailer policy forwarded to @ref parse_header.
 * @return Nothing on success, or the first `err_t` the grammar rejects with
 *         (including `FRAME_INVALID` for trailing bytes after the root).
 */
template <class Cursor, class Sink>
[[nodiscard]] std::expected<void, err_t> walk(const Cursor& root, Sink& sink,
                                              std::pmr::memory_resource& stack_mr,
                                              std::size_t max_depth,
                                              crc_check_t crc_policy = crc_check_t::VERIFY) {
    const auto rh = parse_header(root, crc_policy);
    if (!rh) return std::unexpected(rh.error());
    if (rh->total != root.size()) return std::unexpected(err_t::FRAME_INVALID);  // trailing bytes
    if (!rh->opt.pl) {
        sink.on_leaf(*rh, root);  // opaque root: done
        return {};
    }
    sink.on_open(*rh, root);

    // One open structured node's traversal state: a cursor over its children
    // region and the walk position within it.
    struct frame_t {
        Cursor body;
        std::size_t pos = 0;
    };
    std::pmr::vector<frame_t> stack(&stack_mr);
    // Reserve for the typical FWD nesting (~3-4), not max_depth: a full-depth
    // reserve would draw ~1 KiB on EVERY decode, which a 16 KB-slab arena cannot
    // spare; deeper frames grow (bounded by the cap).
    stack.reserve(8);
    stack.push_back(frame_t{root.region(rh->header, rh->length), 0});

    while (!stack.empty()) {
        frame_t& top = stack.back();
        if (top.pos == top.body.size()) {
            sink.on_close();  // node complete — seal / graft
            stack.pop_back();
            continue;
        }
        // A child of the open node sits at depth == stack.size(); reject at the cap.
        if (stack.size() >= max_depth) return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);
        const auto ch =
            parse_header(top.body.region(top.pos, top.body.size() - top.pos), crc_policy);
        if (!ch) return std::unexpected(ch.error());
        const Cursor node = top.body.region(top.pos, ch->total);  // the child's own bytes
        top.pos += ch->total;
        if (ch->opt.pl) {
            sink.on_open(*ch, node);
            stack.push_back(frame_t{node.region(ch->header, ch->length), 0});
        } else {
            sink.on_leaf(*ch, node);
        }
    }
    return {};
}

}  // namespace tr::wire::grammar
