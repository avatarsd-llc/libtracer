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
#include <vector>

#include "libtracer/grammar.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/tlv_emit.hpp"

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
[[nodiscard]] std::optional<fwd_hdr_t> read_fwd_header(
    const Cursor& cur, std::size_t pos,
    wire::grammar::crc_check_t crc = wire::grammar::crc_check_t::DEFER) {
    if (pos > cur.size()) return std::nullopt;
    const auto h = wire::grammar::parse_header(cur.region(pos, cur.size() - pos), crc);
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
 * @brief What a `dst` peek already learned about a FWD frame, so the head rebuild need not
 *        re-derive it (ADR-0038 inv. #1 — the forward hop parses each header ONCE).
 *
 * A forward hop used to walk the same TLV headers twice: `peek_fwd_dst_segs` read the FWD
 * header, the op VALUE, the dst PATH and every leading dst segment to decide where the frame
 * goes — then @ref rebuild_fwd_forward threw all of it away and re-read the identical bytes to
 * build the outgoing heads. Profiling a 1-link hop put ~88% of it in header parsing, most of it
 * that duplicate. Carrying the offsets forward is what removes it.
 *
 * Offsets, not spans, for the same reason the peeks are: the source may be a rope, so a caller
 * re-slices from its own cursor. Filled by the `peek_fwd_dst_segs` overload that takes one;
 * @ref strip_at is filled by the CALLER once the mount descent has decided how many segments
 * this hop consumes, since the peek runs before that is known.
 */
struct fwd_pre_t {
    bool valid = false;       /**< @brief False ⇒ nothing was learned; rebuild parses itself. */
    std::size_t body_end = 0; /**< @brief End of the FWD body. */
    std::size_t op_pos = 0;   /**< @brief Offset of the op VALUE TLV. */
    std::size_t op_total = 0; /**< @brief Its total size. */
    std::size_t op_body_off = 0; /**< @brief Its body — read to test for REPLY. */
    /** @brief Its body length. Carried rather than re-checked so the rebuild keeps its own
     *         `body_len == 0` rejection: the peek does NOT reject an empty op (such a frame
     *         falls through to the terminus decode today), and making the peek stricter would
     *         silently turn a dropped frame into a terminus one. */
    std::size_t op_body_len = 0;
    std::size_t dst_body_off = 0; /**< @brief First byte of the dst PATH body. */
    std::size_t dst_end = 0;      /**< @brief End of the dst PATH body. */
    std::size_t after_dst = 0;    /**< @brief First byte after the dst PATH TLV. */
    /** @brief Where the surviving `dst` starts after this hop consumes its leading segments —
     *         i.e. the end of segment `strip_k - 1`, or @ref dst_body_off when nothing is
     *         stripped. Filled by the caller after the mount descent; leaving it 0 with
     *         `valid` set would silently forward an unshrunk dst, so the rebuild treats a
     *         `strip_at` below @ref dst_body_off as "not supplied" and walks the segments. */
    std::size_t strip_at = 0;
};

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
 * @brief `peek_fwd_dst_segs`, additionally recording what it parsed into @p pre.
 *
 * Identical result and identical rejections — it only stops throwing the offsets away, so the
 * head rebuild can skip re-reading the same four headers. @p pre is left `valid = false` on
 * every path that returns 0, so a caller cannot pass stale offsets to the rebuild.
 */
template <class Cursor>
[[nodiscard]] std::size_t peek_fwd_dst_segs(
    const Cursor& cur, std::array<std::pair<std::size_t, std::size_t>, kMountPeekMax>& out,
    fwd_pre_t& pre) {
    pre = fwd_pre_t{};
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
    pre.valid = true;
    pre.body_end = body_end;
    pre.op_pos = fwd_h->body_off;
    pre.op_total = op_h->total;
    pre.op_body_off = op_h->body_off;
    pre.op_body_len = op_h->body_len;
    pre.dst_body_off = dst_h->body_off;
    pre.dst_end = dst_end;
    pre.after_dst = dst_pos + dst_h->total;
    pre.strip_at = dst_h->body_off;  // caller overwrites once strip_k is known
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
[[nodiscard]] std::optional<control_head_t> peek_control(
    const Cursor& cur, wire::grammar::crc_check_t crc = wire::grammar::crc_check_t::DEFER) {
    const auto outer = read_fwd_header(cur, 0, crc);
    if (!outer || !outer->opt.pl) return std::nullopt;
    if (outer->type != wire::type_t::ADVERTISE && outer->type != wire::type_t::COMPACT &&
        outer->type != wire::type_t::HANDLE_NACK)
        return std::nullopt;
    // Trailing bytes after the root are a malformed frame, not a prefix to ignore — the
    // same rejection `grammar::walk` makes (grammar.hpp:334-335). Without it a peer could
    // append arbitrary bytes past a well-formed root and have them silently accepted.
    if (outer->total != cur.size()) return std::nullopt;
    const std::size_t body_end = outer->body_off + outer->body_len;
    const auto label_h = read_fwd_header(cur, outer->body_off, crc);
    // The label must be an OPAQUE VALUE: a structured (pl=1) one would mean its body is a
    // child run, not a u16, and reading two bytes out of it would be reading a header.
    if (!label_h || label_h->type != wire::type_t::VALUE || label_h->opt.pl ||
        label_h->body_len < 2)
        return std::nullopt;
    // The label VALUE is a 2-byte LE u16; stitch it a byte at a time so a value that
    // straddles a link boundary reads the same as a contiguous one.
    const auto label = static_cast<std::uint16_t>(
        cur.byte_at(label_h->body_off) |
        (static_cast<std::uint16_t>(cur.byte_at(label_h->body_off + 1)) << 8));
    control_head_t head{outer->type, label, 0, 0};
    const std::size_t c1 = outer->body_off + label_h->total;
    if (c1 < body_end) {
        if (const auto c1_h = read_fwd_header(cur, c1, crc)) {
            // The child must FIT the parent body. Bounding only its start let a malformed
            // child overrun into a root trailer, so its `total` could swallow CRC bytes.
            if (c1 + c1_h->total <= body_end) {
                head.child1_off = c1;
                head.child1_total = c1_h->total;
            }
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
 * Structural, and now counted from @ref fwd_rebuild_t::gather's actual emit sequence rather than
 * budgeted — one region per `push`, in wire order:
 *
 *   1. `head1`            5. `mount_tlv`        (ONE span, whatever the mount's width)
 *   2. `rem_dst`          6. `extra_hdr`        \_ at most one PAIR, for a dynamically
 *   3. `sel`              7. `extra_seg`        /  named bus peer
 *   4. `head2`            8. `src_body`
 *                         9. `tail`
 *
 * A rope source may split any region further and so gathers into a growable container instead;
 * this bound is the CONTIGUOUS arm's, and only that arm uses a stack array.
 *
 * It previously read `6 + 2 * kMountPeekMax` = **14**, describing a header-and-bytes pair per
 * prepended mount segment. **That emission has not happened since #508**, which made the mount run
 * one precomputed span — so the constant was over-provisioned by 5 and, worse, was the wrong
 * SHAPE: tied to mount width when the region count has been independent of it for some time.
 *
 * The shape mattered. Had the 2026-07-30 mount-depth ruling been implemented by re-deriving this
 * as `6 + 2 * depth`, it would have crossed **17** at depth 6 — and 17 is exactly where both
 * shipping transports fall back to a heap-allocated iovec table (`transport_udp.cpp`,
 * `transport_tcp.cpp`, `kMaxInlineIov = 16`; measured by `bench_transport_iov`). That would have
 * put a per-frame allocation on every deep-mount forward hop while `bench_forward_heap` still
 * reported `allocs=0`, because that gate drives a stub link which never assembles an iovec.
 *
 * At 9 the headroom to the transport spill is **8 regions**. Keep the mount one span and this
 * constant does not move when the descent is uncapped.
 */
inline constexpr std::size_t kFwdMaxIov = 9;

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
    stack_writer<kFwdSrcHdrCap> head2; /**< @brief The grown src PATH header. */
    /** @brief The inbound mount as ALREADY-ENCODED NAME TLVs, emitted as ONE span and never
     *         copied. Precomputed once per child (#508), so a hop does no per-segment work. */
    std::span<const std::byte> mount_tlv;
    /** @brief A 4-byte NAME header for @ref extra_seg. */
    std::array<std::byte, 4> extra_hdr;
    /** @brief One dynamically-named trailing mount segment — a bus PEER, whose name is not
     *         known until the frame arrives and so cannot be precomputed. Empty means none;
     *         referenced, not copied, so it must outlive @ref gather. */
    std::string_view extra_seg;
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
     * At most @ref kFwdMaxIov regions for a contiguous source — see that constant
     * for the region-by-region count. (This line previously said "at most 6",
     * which omitted `head2`, `mount_tlv` and the peer pair.)
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
        // The prepended mount: ONE span for the precomputed run, plus at most a
        // header-and-bytes pair for a dynamically-named peer. Nothing is copied.
        if (!mount_tlv.empty()) push(mount_tlv);
        if (!extra_seg.empty()) {
            push(std::span<const std::byte>(extra_hdr));
            push(std::span<const std::byte>(reinterpret_cast<const std::byte*>(extra_seg.data()),
                                            extra_seg.size()));
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
 * @param  mount_tlv     This node's mount path for the link the frame arrived on, ALREADY
 *                       ENCODED as a run of NAME TLVs (precomputed per child, #508).
 * @param  extra_seg     One further mount segment whose name is only known now — a bus PEER.
 *                       Empty when the mount is fully precomputed.
 * @param  strip_k       How many leading dst segments this hop consumes.
 * @retval std::nullopt The frame is not a well-formed forwardable FWD (wrong
 *         type/shape, or fewer than @p strip_k dst segments) — the caller falls to its
 *         terminus path.
 * @note   A returned rebuild may still have `!ok()` (an oversized op TLV
 *         overflowed a head) — the caller must check and drop, never overrun.
 */
template <class Cursor>
[[nodiscard]] std::optional<fwd_rebuild_t> rebuild_fwd_forward(const Cursor& cur,
                                                               std::span<const std::byte> mount_tlv,
                                                               std::string_view extra_seg,
                                                               std::size_t strip_k,
                                                               const fwd_pre_t* pre = nullptr) {
    // The frame's leading headers were already parsed by the `dst` peek that routed this hop.
    // When the caller hands them over, re-reading them is pure duplicated work — profiling put
    // ~88% of a 1-link forward hop in header parsing, most of it exactly this. There is still
    // ONE rebuild: the branch below only chooses where the four offsets come from, and every
    // rejection the self-parsing path applies is applied to the carried values too (see the
    // `op_body_len` note on fwd_pre_t — the peek deliberately does not reject an empty op, so
    // that check has to live here or a malformed frame would change fate).
    std::size_t body_end = 0;
    std::size_t op_pos = 0;
    std::size_t op_total = 0;
    std::size_t op_body_off = 0;
    std::size_t dst_body_off = 0;
    std::size_t dst_end = 0;
    std::size_t pos = 0;

    if (pre != nullptr && pre->valid) {
        if (pre->op_body_len == 0) return std::nullopt;
        body_end = pre->body_end;
        op_pos = pre->op_pos;
        op_total = pre->op_total;
        op_body_off = pre->op_body_off;
        dst_body_off = pre->dst_body_off;
        dst_end = pre->dst_end;
        pos = pre->after_dst;
    } else {
        const auto fwd_h = read_fwd_header(cur, 0);
        if (!fwd_h || fwd_h->type != wire::type_t::FWD) return std::nullopt;
        body_end = fwd_h->body_off + fwd_h->body_len;

        pos = fwd_h->body_off;
        const auto op_h = read_fwd_header(cur, pos);
        if (!op_h || op_h->type != wire::type_t::VALUE || op_h->body_len == 0) return std::nullopt;
        op_pos = pos;
        op_total = op_h->total;
        op_body_off = op_h->body_off;
        pos += op_h->total;

        const auto dst_h = read_fwd_header(cur, pos);
        if (!dst_h || dst_h->type != wire::type_t::PATH) return std::nullopt;
        dst_body_off = dst_h->body_off;
        dst_end = dst_h->body_off + dst_h->body_len;
        pos += dst_h->total;
    }
    const bool is_reply =
        static_cast<graph::fwd_op_t>(cur.byte_at(op_body_off)) == graph::fwd_op_t::REPLY;

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

    // The K leading dst segments (NAMEs) this hop consumes. The peek already walked exactly
    // these, so a caller that recorded where they end hands the answer over; `strip_at` below
    // `dst_body_off` means it did not, and the walk runs as before.
    std::size_t strip_at = dst_body_off;
    if (pre != nullptr && pre->valid && pre->strip_at >= dst_body_off) {
        strip_at = pre->strip_at;
        if (strip_at > dst_end) return std::nullopt;  // dst shorter than the mount
    } else {
        for (std::size_t i = 0; i < strip_k; ++i) {
            if (strip_at >= dst_end) return std::nullopt;  // dst shorter than the mount
            const auto seg_h = read_fwd_header(cur, strip_at);
            if (!seg_h || seg_h->type != wire::type_t::NAME) return std::nullopt;
            strip_at += seg_h->total;
        }
    }
    r.rem_dst_off = strip_at;
    r.rem_dst_len = dst_end - strip_at;

    // The inbound mount path appended to src (grow) — empty for a REPLY (no accumulation).
    // The precomputed run is already-encoded bytes, so it contributes its own length; a
    // dynamic peer segment adds a 4-byte NAME header plus its bytes. The only bound left is
    // the format's own `u16` NAME length field — beyond that a mount path is limited solely
    // by the frame fitting the link's `max_frame`/MTU, never by a buffer budget.
    if (extra_seg.size() > 0xFFFFu) return std::nullopt;  // exceeds the NAME length field
    const std::size_t inbound_name_len =
        is_reply ? 0u : mount_tlv.size() + (extra_seg.empty() ? 0u : 4u + extra_seg.size());

    const std::size_t new_dst_body = r.rem_dst_len;
    const std::size_t new_src_body = src_h->body_len + inbound_name_len;
    const std::size_t new_dst_total = (new_dst_body > 0xFFFFu ? 6u : 4u) + new_dst_body;
    const std::size_t new_src_total = (new_src_body > 0xFFFFu ? 6u : 4u) + new_src_body;
    const std::size_t new_fwd_body =
        op_total + new_dst_total + r.sel_total + new_src_total + r.tail_len;

    // head1: FWD header + op (copied) + new (shrunk) dst header. head2: new (grown)
    // src header + the prepended inbound NAME. Both fixed stack buffers — ZERO heap
    // on the forward hop (ADR-0038 inv. #2). An overflow (a malformed op TLV larger
    // than the buffer) yields an empty span ⇒ the caller drops, never a buffer overrun.
    r.head1.header(wire::type_t::FWD, new_fwd_body);
    cur.for_each_span(op_pos, op_total, [&](std::span<const std::byte> s) { r.head1.raw(s); });
    r.head1.header(wire::type_t::PATH, new_dst_body);

    r.head2.header(wire::type_t::PATH, new_src_body);
    if (!is_reply) {
        r.mount_tlv = mount_tlv;
        if (!extra_seg.empty()) {
            r.extra_hdr[0] = static_cast<std::byte>(std::to_underlying(wire::type_t::NAME));
            r.extra_hdr[1] = std::byte{0};
            r.extra_hdr[2] = static_cast<std::byte>(extra_seg.size() & 0xFF);
            r.extra_hdr[3] = static_cast<std::byte>((extra_seg.size() >> 8) & 0xFF);
            r.extra_seg = extra_seg;
        }
    }

    return r;
}

/**
 * @brief Encode @p segs as a run of NAME TLVs — the precomputed mount prefix (#508).
 *
 * Built ONCE per child, at registration, and handed to every hop as
 * @ref fwd_rebuild_t::mount_tlv. Canonical form (`opt = 0`, `u16` length) is chosen here
 * deliberately: we are EMITTING, so there is no encoding variance to be wrong about — unlike
 * MATCHING an inbound path, where a peer may legally send `opt.LL=1` and byte comparison
 * would break conformance (ADR-0062 §Considered options).
 * @return The encoded run, or nullopt if a segment exceeds the NAME length field.
 */
[[nodiscard]] inline std::optional<std::vector<std::byte>> encode_mount_tlv(
    std::span<const std::string_view> segs) {
    std::vector<std::byte> out;
    std::size_t total = 0;
    for (const std::string_view s : segs) {
        if (s.size() > 0xFFFFu) return std::nullopt;
        total += 4u + s.size();
    }
    out.reserve(total);
    // The oversize check above is what makes this a straight substitution: `emit_tlv`
    // auto-widens a body past 0xFFFF to a 4-byte length (setting `opt.ll`), whereas a mount
    // run is peeked by OFFSET against a fixed 4-byte NAME header. Refusing first means
    // `emit_name` never widens here, so the bytes are identical to the hand-rolled push it
    // replaces (ADR-0048 §3 — one representation of the header layout).
    for (const std::string_view s : segs) wire::emit_name(out, s);
    return out;
}

/**
 * @brief Single-NAME convenience overload — a flat, one-segment mount (strip-1).
 *
 * The pre-ADR-0061 shape, kept for callers whose link identity is a bare NAME.
 */
template <class Cursor>
[[nodiscard]] std::optional<fwd_rebuild_t> rebuild_fwd_forward(const Cursor& cur,
                                                               std::string_view inbound_name) {
    return rebuild_fwd_forward(cur, std::span<const std::byte>{}, inbound_name, 1);
}

}  // namespace tr::net
