/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The FWD-plane frame view (ADR-0038 inv. #1/#2, ADR-0053 ④b): the offset-dispatch
 * cluster the forward hop reads a frame by — one top-level header read as ABSOLUTE
 * offsets, the forward-vs-terminus peeks (first `dst` segment, op discriminant),
 * the control-frame head peek, the fixed-capacity stack byte-writer, and the
 * shrunk-dst / grown-src head rebuild. Everything is templated over the grammar
 * `Cursor` concept (grammar.hpp), so the identical logic serves a contiguous
 * `span_cursor` and a link-walking `rope_cursor` — offsets, never spans, so every
 * result is source-agnostic and the caller re-slices from its own cursor.
 *
 * Extracted from fwd_router.cpp so the dispatch rules are unit-testable directly
 * (hand-built frames, no live transports) — the length_prefix_framer precedent.
 * The router delegates mechanically; frames are byte-identical.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "libtracer/grammar.hpp"
#include "libtracer/op_resolve.hpp"

/**
 * @file
 * @brief The FWD forward-plane offset-dispatch frame view (ADR-0038 / ADR-0053 ④b).
 */

namespace tr::net {

/**
 * @brief One top-level TLV header read in isolation (NO descent) — the byte
 *        offsets the zero-copy forward rebuild needs.
 *
 * Kept as ABSOLUTE offsets into the source so the rebuild can re-slice
 * src/payload as views (no copy). It is a thin ADAPTER over the ONE wire grammar
 * (`grammar::parse_header`, ADR-0048 §1): the length math is not mirrored here —
 * this only turns the grammar's relative `header_t` into the absolute
 * `body_off = pos + header` the forward plane reads by. CRC is DEFERRED (the
 * forward hop never walks a payload; the terminus / next hop verifies). One
 * deliberate difference from the pre-grammar reader: the grammar rejects a
 * `type == 0x00` or reserved-opt-bit header up front, so a malformed frame is
 * dropped at this hop instead of forwarded — every caller already rejected such
 * a header by its type check, so well-formed traffic is byte-identical.
 */
struct fwd_hdr_t {
    wire::type_t type{};        /**< @brief The TLV type code. */
    wire::opt_t opt{};          /**< @brief The decoded `opt` bits. */
    std::size_t header_len = 0; /**< @brief 4 (u16 length) or 6 (u32 length). */
    std::size_t body_off = 0;   /**< @brief Absolute offset of the body within the source. */
    std::size_t body_len = 0;   /**< @brief Body (children/payload) length, trailer excluded. */
    std::size_t total = 0;      /**< @brief header_len + body_len + trailer. */
};

/**
 * @brief Read ONE TLV header at absolute offset @p pos of @p cur (no descent).
 *
 * Templated over the grammar `Cursor` concept (ADR-0053 ④b): the forward plane
 * reads its dispatch offsets through the SAME byte-source seam the one grammar
 * validates through — `span_cursor` for the contiguous path, the rope cursor for
 * a scatter-gather frame, with no per-cursor offset math. `cur.region(pos, …)`
 * narrows either source in O(1) before the header parse.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 * @param  pos    Absolute offset of the header to read.
 * @retval std::nullopt @p pos is out of range or the grammar rejects the header.
 */
template <class Cursor>
[[nodiscard]] std::optional<fwd_hdr_t> read_fwd_header(const Cursor& cur, std::size_t pos) {
    if (pos > cur.size()) return std::nullopt;
    const auto h = wire::grammar::parse_header(cur.region(pos, cur.size() - pos),
                                               wire::grammar::crc_check_t::DEFER);
    if (!h) return std::nullopt;
    return fwd_hdr_t{.type = h->type,
                     .opt = h->opt,
                     .header_len = h->header,
                     .body_off = pos + h->header,
                     .body_len = h->length,
                     .total = h->total};
}

/** @brief The most `dst` segments the mount descent ever inspects: net / module / name / peer. */
inline constexpr std::size_t kMountPeekMax = 4;

/**
 * @brief The leading `dst` segments of a FWD, read by OFFSET with no allocation.
 *
 * The strip-K generalization of @ref peek_fwd_first_dst_seg (ADR-0061): a mount path is
 * `/net/<module>/<name>[/<peer>]`, so the demux needs the first few segments rather than
 * exactly one. Returns each segment's `[body_off, body_len)` in order, up to
 * @ref kMountPeekMax; a shorter `dst` simply yields fewer. Offsets, not spans, so the
 * result is source-agnostic — the caller re-slices from its own cursor (contiguous or
 * rope). Empty iff the frame is not a structured FWD with an op VALUE and a non-empty dst.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 */
template <class Cursor>
[[nodiscard]] std::size_t peek_fwd_dst_segs(
    const Cursor& cur, std::array<std::pair<std::size_t, std::size_t>, kMountPeekMax>& out) {
    const auto fwd_h = read_fwd_header(cur, 0);
    if (!fwd_h || fwd_h->type != wire::type_t::FWD || !fwd_h->opt.pl) return 0;
    const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;
    const auto op_h = read_fwd_header(cur, fwd_h->body_off);
    if (!op_h || op_h->type != wire::type_t::VALUE) return 0;
    const std::size_t dst_pos = fwd_h->body_off + op_h->total;
    if (dst_pos >= body_end) return 0;
    const auto dst_h = read_fwd_header(cur, dst_pos);
    if (!dst_h || dst_h->type != wire::type_t::PATH || dst_h->body_len == 0) return 0;
    const std::size_t dst_end = dst_h->body_off + dst_h->body_len;
    std::size_t pos = dst_h->body_off;
    std::size_t n = 0;
    while (n < kMountPeekMax && pos < dst_end) {
        const auto seg_h = read_fwd_header(cur, pos);
        if (!seg_h || seg_h->type != wire::type_t::NAME) break;
        out[n++] = {seg_h->body_off, seg_h->body_len};
        pos += seg_h->total;
    }
    return n;
}

/**
 * @brief The forward dispatch decision, read by OFFSET with no allocation
 *        (ADR-0038 inv. #1, ADR-0039).
 *
 * A FWD whose first `dst` segment names a transport child is a forward hop that
 * never needs the decoded tree. Returns the `[body_off, body_len)` of the first
 * dst-segment NAME iff the frame is a structured FWD with an op VALUE + a
 * non-empty dst PATH; nullopt otherwise (malformed, non-FWD, or empty dst ⇒ the
 * caller falls back to the full-decode terminus path). Offsets, not a span, so
 * the result is source-agnostic — the caller re-slices the segment bytes from
 * its own cursor (contiguous or rope).
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 */
template <class Cursor>
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> peek_fwd_first_dst_seg(
    const Cursor& cur) {
    const auto fwd_h = read_fwd_header(cur, 0);
    if (!fwd_h || fwd_h->type != wire::type_t::FWD || !fwd_h->opt.pl) return std::nullopt;
    const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;
    // child[0] = op VALUE
    const auto op_h = read_fwd_header(cur, fwd_h->body_off);
    if (!op_h || op_h->type != wire::type_t::VALUE) return std::nullopt;
    // child[1] = dst PATH
    const std::size_t dst_pos = fwd_h->body_off + op_h->total;
    if (dst_pos >= body_end) return std::nullopt;
    const auto dst_h = read_fwd_header(cur, dst_pos);
    if (!dst_h || dst_h->type != wire::type_t::PATH || dst_h->body_len == 0) return std::nullopt;
    // dst.child[0] = first segment NAME
    const auto seg_h = read_fwd_header(cur, dst_h->body_off);
    if (!seg_h || seg_h->type != wire::type_t::NAME) return std::nullopt;
    return std::pair{seg_h->body_off, seg_h->body_len};
}

/**
 * @brief Read the FWD op discriminant (child[0], a VALUE u8) by OFFSET.
 *
 * The terminus split (REPLY → originator sink vs request → arena resolve)
 * without a decode.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 * @retval std::nullopt Not a structured FWD, or its op VALUE is missing/empty.
 */
template <class Cursor>
[[nodiscard]] std::optional<graph::fwd_op_t> peek_fwd_op(const Cursor& cur) {
    const auto fwd_h = read_fwd_header(cur, 0);
    if (!fwd_h || fwd_h->type != wire::type_t::FWD || !fwd_h->opt.pl) return std::nullopt;
    const auto op_h = read_fwd_header(cur, fwd_h->body_off);
    if (!op_h || op_h->type != wire::type_t::VALUE || op_h->body_len == 0) return std::nullopt;
    return static_cast<graph::fwd_op_t>(cur.byte_at(op_h->body_off));
}

/**
 * @brief A control frame (ADVERTISE / COMPACT / HANDLE_NACK) peeked off any
 *        cursor without a decoded tree (ADR-0055 §2).
 *
 * Carries the `type`, the `u16` label (child[0] VALUE, LE), and the
 * `[off, total)` of child[1] — the route (ADVERTISE) / payload (COMPACT)
 * sub-TLV, or `{0, 0}` for a bare-label HANDLE_NACK. Source-agnostic (offsets,
 * not spans), so the caller re-slices from its own cursor (ADR-0053 ④b/⑥).
 */
struct control_head_t {
    wire::type_t type = wire::type_t::VALUE; /**< @brief The control frame's outer TLV type. */
    std::uint16_t label = 0;      /**< @brief The u16 route-handle label (child[0], LE). */
    std::size_t child1_off = 0;   /**< @brief Offset of child[1]; 0 ⇒ none (bare-label NACK). */
    std::size_t child1_total = 0; /**< @brief header + body + trailer of child[1]. */
};

/**
 * @brief Peek a control frame's head (type + label + child[1] window) by OFFSET.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 * @retval std::nullopt Malformed, or not a structured ADVERTISE / COMPACT /
 *         HANDLE_NACK leading with a ≥2-byte VALUE label.
 */
template <class Cursor>
[[nodiscard]] std::optional<control_head_t> peek_control(const Cursor& cur) {
    const auto outer = read_fwd_header(cur, 0);
    if (!outer || !outer->opt.pl) return std::nullopt;
    if (outer->type != wire::type_t::ADVERTISE && outer->type != wire::type_t::COMPACT &&
        outer->type != wire::type_t::HANDLE_NACK)
        return std::nullopt;
    const std::size_t body_end = outer->body_off + outer->body_len;
    const auto label_h = read_fwd_header(cur, outer->body_off);
    if (!label_h || label_h->type != wire::type_t::VALUE || label_h->body_len < 2)
        return std::nullopt;
    // The label VALUE is a 2-byte LE u16; stitch it a byte at a time so a value that
    // straddles a link boundary reads the same as a contiguous one.
    const auto label = static_cast<std::uint16_t>(
        cur.byte_at(label_h->body_off) |
        (static_cast<std::uint16_t>(cur.byte_at(label_h->body_off + 1)) << 8));
    control_head_t head{outer->type, label, 0, 0};
    const std::size_t c1 = outer->body_off + label_h->total;
    if (c1 < body_end) {
        if (const auto c1_h = read_fwd_header(cur, c1)) {
            head.child1_off = c1;
            head.child1_total = c1_h->total;
        }
    }
    return head;
}

/**
 * @brief A fixed-capacity stack byte-writer — the zero-heap head builder for the
 *        forward hop (ADR-0038 inv. #2).
 *
 * The zero-heap counterpart of the old vector-based header builder: "the fresh
 * header bytes … a stack std::array, not a std::vector". Bounded by the wire
 * header widths + one NAME (kMaxSegmentBytes), so @p N is a small compile-time
 * constant; a write past capacity clamps to empty (the caller treats an empty
 * head as a drop — never a buffer overrun).
 *
 * @tparam N The writer's stack capacity in bytes.
 */
template <std::size_t N>
class stack_writer {
   public:
    /** @brief Append a structured TLV header (`pl` set, `ll` auto-widened) for @p body_len. */
    void header(wire::type_t type, std::size_t body_len) {
        wire::opt_t opt{.pl = true};
        if (body_len > 0xFFFFu) opt.ll = true;
        const std::size_t width = opt.ll ? 4u : 2u;
        if (len_ + 2 + width > N) {
            overflow_ = true;
            return;
        }
        buf_[len_++] = static_cast<std::byte>(std::to_underlying(type));
        buf_[len_++] = static_cast<std::byte>(opt.encode());
        for (std::size_t i = 0; i < width; ++i)
            buf_[len_++] = static_cast<std::byte>((body_len >> (8 * i)) & 0xFF);
    }
    /** @brief Append a complete NAME TLV over @p s (type, opt=0, u16 len, bytes). */
    void name(std::string_view s) {
        if (len_ + 4 + s.size() > N || s.size() > 0xFFFFu) {
            overflow_ = true;
            return;
        }
        buf_[len_++] = static_cast<std::byte>(std::to_underlying(wire::type_t::NAME));
        buf_[len_++] = std::byte{0};
        buf_[len_++] = static_cast<std::byte>(s.size() & 0xFF);
        buf_[len_++] = static_cast<std::byte>((s.size() >> 8) & 0xFF);
        for (char c : s) buf_[len_++] = static_cast<std::byte>(c);
    }
    /** @brief Copy opaque @p bytes verbatim (the op TLV). */
    void raw(std::span<const std::byte> bytes) {
        if (len_ + bytes.size() > N) {
            overflow_ = true;
            return;
        }
        for (std::byte b : bytes) buf_[len_++] = b;
    }
    /**
     * @brief The written bytes.
     * @retval empty A write overflowed @p N — the caller must drop the frame.
     */
    [[nodiscard]] std::span<const std::byte> span() const {
        return overflow_ ? std::span<const std::byte>{}
                         : std::span<const std::byte>(buf_.data(), len_);
    }
    /** @brief False ⇔ a write overflowed @p N. */
    [[nodiscard]] bool ok() const noexcept { return !overflow_; }

   private:
    std::array<std::byte, N> buf_{}; /**< @brief The fixed stack buffer. */
    std::size_t len_ = 0;            /**< @brief Bytes written so far. */
    bool overflow_ = false;          /**< @brief A write exceeded @p N. */
};

/** @brief Capacity of the forward hop's first head: FWD hdr(≤6) + op TLV(small) + PATH hdr(≤6). */
inline constexpr std::size_t kFwdHead1Cap = 64;
/** @brief Capacity of the forward hop's second head: PATH hdr(≤6) + one NAME(≤4+segment). */
// The grown src PATH header alone — 2 type/opt bytes plus a 2- or 4-byte length. This is a
// STRUCTURAL bound (the widest TLV header the format has), not a budget: the prepended mount
// NAMEs are emitted as bare 4-byte headers and their bytes are referenced from the caller's
// storage by @ref fwd_rebuild_t::gather, never copied in here. An earlier revision copied them
// into this buffer, which silently made the buffer size a cap on how long a connection NAME
// could be — a synthetic limit on user-chosen data (forbidden by RFC-0006/0007 + ADR-0051), and
// one whose breach was a dropped LEGAL frame rather than a clean rejection. A mount path is now
// bounded only by the wire's own `u16` NAME length and by the outgoing frame fitting the link's
// `max_frame`/MTU.
inline constexpr std::size_t kFwdSrcHdrCap = 6;

/**
 * @brief Upper bound on the regions @ref fwd_rebuild_t::gather emits for a CONTIGUOUS source.
 *
 * Structural, derived from the layout rather than budgeted: the six fixed regions (head1,
 * remaining dst, optional selector, src header, original src body, tail) plus one
 * header-and-bytes PAIR per prepended mount segment. A rope source may split any region
 * further and so gathers into a growable container instead.
 */
inline constexpr std::size_t kFwdMaxIov = 6 + 2 * kMountPeekMax;

/**
 * @brief The rebuilt forward-hop frame: fresh stack heads + the untouched source
 *        regions to interleave (ADR-0038 inv. #2 — ZERO heap on the forward hop).
 *
 * Produced by @ref rebuild_fwd_forward. Layout of the outgoing frame is
 * `head1 · rem_dst · sel · head2 · src_body · tail`, where every non-head region
 * is an `[off, len)` window into the SOURCE cursor — the emit order is fixed by
 * @ref gather so the bytes a downstream child receives are byte-identical to the
 * pre-extraction router.
 */
struct fwd_rebuild_t {
    stack_writer<kFwdHead1Cap> head1;  /**< @brief FWD header + op (copied) + shrunk dst header. */
    stack_writer<kFwdSrcHdrCap> head2; /**< @brief The grown src PATH header (headers only). */
    /** @brief Bare 4-byte NAME headers for the prepended mount segments, in order. */
    std::array<std::array<std::byte, 4>, kMountPeekMax> mount_hdr;
    /** @brief The mount segments themselves, REFERENCED not copied — they must outlive
     *         @ref gather (the registry owns them for the duration of the hop). */
    std::array<std::string_view, kMountPeekMax> mount;
    std::size_t mount_n = 0;      /**< @brief How many mount segments are prepended to src. */
    std::size_t rem_dst_off = 0;  /**< @brief Remaining dst body after the stripped segment. */
    std::size_t rem_dst_len = 0;  /**< @brief Length of the remaining dst body. */
    std::size_t sel_pos = 0;      /**< @brief The optional FIELD selector TLV; 0 len ⇒ none. */
    std::size_t sel_total = 0;    /**< @brief Total bytes of the selector TLV. */
    std::size_t src_body_off = 0; /**< @brief The original src PATH body. */
    std::size_t src_body_len = 0; /**< @brief Length of the original src body. */
    std::size_t tail_off = 0;     /**< @brief Bytes after src (payload etc.). */
    std::size_t tail_len = 0;     /**< @brief Length of the tail region. */

    /** @brief True ⇔ both heads fit their stack buffers (else the caller drops). */
    [[nodiscard]] bool ok() const { return head1.ok() && head2.ok(); }

    /**
     * @brief Emit the outgoing frame's regions, in wire order, through @p push.
     *
     * Written ONCE over the cursor seam: each source region is emitted via
     * `for_each_span`, which yields exactly one sub-span for a contiguous source
     * and one per straddled link for a rope — so only the caller's iov container
     * varies (a stack array for the span path, a pmr vector for the rope path).
     * At most 6 regions for a contiguous source.
     *
     * @tparam Cursor A grammar byte-source cursor (span or rope) — the SAME
     *                source @ref rebuild_fwd_forward read the offsets from.
     * @tparam Push   Callable taking one `std::span<const std::byte>`.
     */
    template <class Cursor, class Push>
    void gather(const Cursor& cur, Push&& push) const {
        push(head1.span());
        if (rem_dst_len > 0) cur.for_each_span(rem_dst_off, rem_dst_len, push);
        if (sel_total > 0) cur.for_each_span(sel_pos, sel_total, push);
        push(head2.span());
        // The prepended mount run: each segment's header from our own storage, its bytes
        // straight from the caller's string — no copy, so a long NAME costs nothing here.
        for (std::size_t i = 0; i < mount_n; ++i) {
            push(std::span<const std::byte>(mount_hdr[i]));
            push(std::span<const std::byte>(reinterpret_cast<const std::byte*>(mount[i].data()),
                                            mount[i].size()));
        }
        if (src_body_len > 0) cur.for_each_span(src_body_off, src_body_len, push);
        if (tail_len > 0) cur.for_each_span(tail_off, tail_len, push);
    }
};

/**
 * @brief The forward hop's head rebuild, read entirely by OFFSET — no decoded
 *        tree (ADR-0038 inv. #1).
 *
 * Layout: `FWD{ op VALUE, dst PATH, FIELD? sel, src PATH, tail }` — strips @p strip_k
 * leading dst segments (shrink), grows src by @p inbound_mount (unless the op is
 * REPLY: a reply accumulates no return route, RFC-0004 §B), and synthesizes the
 * two fresh stack heads. The caller scatter-gathers the result via
 * @ref fwd_rebuild_t::gather — no payload copy, zero heap.
 *
 * **strip-K and the symmetric return route (ADR-0061 + its erratum).** A mount is
 * addressed by its full path `/net/<module>/<name>[/<peer>]`, so a hop consumes K
 * segments rather than one, and `src` grows by that SAME run — not by a single NAME.
 * Growing by a bare name would make the return route ambiguous the moment connection
 * names are per-module-scoped (`/net/ws-client/foo` vs `/net/tcp-client/foo`), because a
 * reply's `dst` IS the accumulated `src`. Prepending the whole mount keeps
 * routing-address `==` vertex-path in BOTH directions, so a reply resolves through the
 * identical descent as a forward.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur           The cursor positioned at the inbound FWD frame's first byte.
 * @param  inbound_mount This node's mount path for the link the frame arrived on, one
 *                       string per segment (e.g. `{"net", "ws-client", "foo"}`).
 * @param  strip_k       How many leading dst segments this hop consumes.
 * @retval std::nullopt The frame is not a well-formed forwardable FWD (wrong
 *         type/shape, or fewer than @p strip_k dst segments) — the caller falls to its
 *         terminus path.
 * @note   A returned rebuild may still have `!ok()` (an oversized op TLV
 *         overflowed a head) — the caller must check and drop, never overrun.
 */
template <class Cursor>
[[nodiscard]] std::optional<fwd_rebuild_t> rebuild_fwd_forward(
    const Cursor& cur, std::span<const std::string_view> inbound_mount, std::size_t strip_k) {
    const auto fwd_h = read_fwd_header(cur, 0);
    if (!fwd_h || fwd_h->type != wire::type_t::FWD) return std::nullopt;
    const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;

    std::size_t pos = fwd_h->body_off;
    const auto op_h = read_fwd_header(cur, pos);
    if (!op_h || op_h->type != wire::type_t::VALUE || op_h->body_len == 0) return std::nullopt;
    const std::size_t op_pos = pos;
    const bool is_reply =
        static_cast<graph::fwd_op_t>(cur.byte_at(op_h->body_off)) == graph::fwd_op_t::REPLY;
    pos += op_h->total;

    const auto dst_h = read_fwd_header(cur, pos);
    if (!dst_h || dst_h->type != wire::type_t::PATH) return std::nullopt;
    pos += dst_h->total;

    fwd_rebuild_t r;
    if (pos < body_end) {
        const auto peek = read_fwd_header(cur, pos);
        if (peek && peek->type == wire::type_t::FIELD) {
            r.sel_pos = pos;
            r.sel_total = peek->total;
            pos += peek->total;
        }
    }

    const auto src_h = read_fwd_header(cur, pos);
    if (!src_h || src_h->type != wire::type_t::PATH) return std::nullopt;
    pos += src_h->total;

    r.tail_off = pos;
    r.tail_len = body_end > pos ? body_end - pos : 0;
    r.src_body_off = src_h->body_off;
    r.src_body_len = src_h->body_len;

    // The K leading dst segments (NAMEs) this hop consumes.
    const std::size_t dst_end = dst_h->body_off + dst_h->body_len;
    std::size_t strip_at = dst_h->body_off;
    for (std::size_t i = 0; i < strip_k; ++i) {
        if (strip_at >= dst_end) return std::nullopt;  // dst shorter than the mount
        const auto seg_h = read_fwd_header(cur, strip_at);
        if (!seg_h || seg_h->type != wire::type_t::NAME) return std::nullopt;
        strip_at += seg_h->total;
    }
    r.rem_dst_off = strip_at;
    r.rem_dst_len = dst_end - strip_at;

    // The inbound mount path appended to src (grow) — empty for a REPLY (no accumulation).
    // The only bounds here are the format's own: a mount is at most @ref kMountPeekMax
    // segments (net / module / name / peer — RFC-0014's addressing arity, structural), and a
    // NAME's length field is a `u16`. Beyond that a mount path is limited only by the frame
    // fitting the link's `max_frame`/MTU — there is no buffer budget to exceed.
    if (inbound_mount.size() > kMountPeekMax) return std::nullopt;
    std::size_t inbound_name_len = 0;
    if (!is_reply) {
        for (const std::string_view seg : inbound_mount) {
            if (seg.size() > 0xFFFFu) return std::nullopt;  // exceeds the NAME length field
            inbound_name_len += 4u + seg.size();
        }
    }

    const std::size_t new_dst_body = r.rem_dst_len;
    const std::size_t new_src_body = src_h->body_len + inbound_name_len;
    const std::size_t new_dst_total = (new_dst_body > 0xFFFFu ? 6u : 4u) + new_dst_body;
    const std::size_t new_src_total = (new_src_body > 0xFFFFu ? 6u : 4u) + new_src_body;
    const std::size_t new_fwd_body =
        op_h->total + new_dst_total + r.sel_total + new_src_total + r.tail_len;

    // head1: FWD header + op (copied) + new (shrunk) dst header. head2: new (grown)
    // src header + the prepended inbound NAME. Both fixed stack buffers — ZERO heap
    // on the forward hop (ADR-0038 inv. #2). An overflow (a malformed op TLV larger
    // than the buffer) yields an empty span ⇒ the caller drops, never a buffer overrun.
    r.head1.header(wire::type_t::FWD, new_fwd_body);
    cur.for_each_span(op_pos, op_h->total, [&](std::span<const std::byte> s) { r.head1.raw(s); });
    r.head1.header(wire::type_t::PATH, new_dst_body);

    r.head2.header(wire::type_t::PATH, new_src_body);
    if (!is_reply) {
        for (const std::string_view seg : inbound_mount) {
            auto& h = r.mount_hdr[r.mount_n];
            h[0] = static_cast<std::byte>(std::to_underlying(wire::type_t::NAME));
            h[1] = std::byte{0};
            h[2] = static_cast<std::byte>(seg.size() & 0xFF);
            h[3] = static_cast<std::byte>((seg.size() >> 8) & 0xFF);
            r.mount[r.mount_n] = seg;
            ++r.mount_n;
        }
    }

    return r;
}

/**
 * @brief Single-NAME convenience overload — a flat, one-segment mount (strip-1).
 *
 * The pre-ADR-0061 shape, kept for callers whose link identity is a bare NAME.
 */
template <class Cursor>
[[nodiscard]] std::optional<fwd_rebuild_t> rebuild_fwd_forward(const Cursor& cur,
                                                               std::string_view inbound_name) {
    const std::string_view one[1] = {inbound_name};
    return rebuild_fwd_forward(cur, std::span<const std::string_view>(one), 1);
}

}  // namespace tr::net
