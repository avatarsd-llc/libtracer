/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The FWD-plane frame view (ADR-0038 inv. #1/#2, ADR-0053 ④b): the offset-dispatch
 * cluster the forward hop reads a frame by — one top-level header read as ABSOLUTE
 * offsets, the forward-vs-terminus peeks (first `dst` segment, op discriminant),
 * the control-frame head peek, the fixed-capacity stack byte-writer, and the
 * shrunk-dst / grown-src head rebuild. Everything is templated over the grammar
 * `Cursor` concept (`%grammar.hpp`), so the identical logic serves a contiguous
 * `span_cursor_t` and a link-walking `rope_cursor_t` — offsets, never spans, so every
 * result is source-agnostic and the caller re-slices from its own cursor.
 *
 * Extracted from fwd_router.cpp so the dispatch rules are unit-testable directly
 * (hand-built frames, no live transports) — the length_prefix_framer_t precedent.
 * The router delegates mechanically; frames are byte-identical.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "libtracer/config.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/op_resolve.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path_ref.hpp"
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
 * @brief One packed `dst`/`src` segment record read through the cursor seam (RFC-0018 §5).
 *
 * The whole record grammar for the FORWARD plane, in one place: `[u8 len][len bytes]`, plus
 * the `len == 0` ESCAPE `00 <u8 kind> <u8 len> <payload>` (§5.4 Amendment 1). This is
 * **frame-path** context, so an escape is a record to STEP OVER — a forwarder that does not
 * implement its `kind` relays the frame rather than dropping one it is only carrying. The
 * canonical / key context, where the escape is refused, is `packed_path_valid_key` and
 * `key_view_t::record_end`; the two are two functions on purpose.
 *
 * Byte-at-a-time through the cursor, never through a span, for the reason
 * @ref read_path_ref_element is: on the rope tier a record may straddle a link boundary and
 * there is then no span to hand a span-taking parser. Three `byte_at` calls at worst, no
 * scratch and no stitch slot.
 */
struct packed_seg_t {
    std::size_t body_off = 0; /**< @brief Offset of the segment's payload bytes. */
    std::size_t body_len = 0; /**< @brief Payload length; 0 only for an escape. */
    std::size_t total = 0;    /**< @brief Whole record size — `1 + len`, or `3 + len`. */
    bool escape = false;      /**< @brief True ⇔ the `len == 0` escape record (§5.4). */
};

/**
 * @brief Read the packed segment record at absolute offset @p at, bounded by @p end.
 * @retval std::nullopt Ragged: no length byte before @p end, or a record running past it.
 */
template <class Cursor>
[[nodiscard]] std::optional<packed_seg_t> read_packed_seg(const Cursor& cur, std::size_t at,
                                                          std::size_t end) {
    if (at >= end) return std::nullopt;
    const std::size_t len = static_cast<std::uint8_t>(cur.byte_at(at));
    if (len != wire::kPackedEscapeLen) {
        if (at + 1 + len > end) return std::nullopt;
        return packed_seg_t{.body_off = at + 1, .body_len = len, .total = 1 + len};
    }
    if (at + wire::kPackedEscapeOverhead > end) return std::nullopt;
    const std::size_t esc = static_cast<std::uint8_t>(cur.byte_at(at + 2));
    if (at + wire::kPackedEscapeOverhead + esc > end) return std::nullopt;
    return packed_seg_t{.body_off = at + wire::kPackedEscapeOverhead,
                        .body_len = esc,
                        .total = wire::kPackedEscapeOverhead + esc,
                        .escape = true};
}

/**
 * @brief Read ONE TLV header at absolute offset @p pos of @p cur (no descent).
 *
 * Templated over the grammar `Cursor` concept (ADR-0053 ④b): the forward plane
 * reads its dispatch offsets through the SAME byte-source seam the one grammar
 * validates through — `span_cursor_t` for the contiguous path, the rope cursor for
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

/**
 * @brief What a `dst` peek already learned about a FWD frame, so the head rebuild need not
 *        re-derive it (ADR-0038 inv. #1 — the forward hop parses each header ONCE).
 *
 * A forward hop used to walk the same TLV headers twice: the `dst` peek read the FWD
 * header, the op VALUE, the dst PATH and every leading dst segment to decide where the frame
 * goes — then @ref rebuild_fwd_forward threw all of it away and re-read the identical bytes to
 * build the outgoing heads. Profiling a 1-link hop put ~88% of it in header parsing, most of it
 * that duplicate. Carrying the offsets forward is what removes it.
 *
 * Offsets, not spans, for the same reason the peeks are: the source may be a rope, so a caller
 * re-slices from its own cursor. Filled by @ref peek_fwd_dst_any, which is the ONLY parse of
 * the frame's leading headers a hop makes (#1794): the descent, the head rebuild, the bound
 * arms, the terminus split and both refusal arms read these offsets and never re-read the
 * headers behind them.
 *
 * Two validities, kept apart on purpose. @ref valid says the HEADERS were read — through the
 * `dst`, in a routable form or the empty one. @ref strip_at says how much of that `dst` this
 * hop consumes, which the peek knows only for a bound `dst` and the mount descent decides for
 * a canonical one. The descent used to clear @ref valid when it had no strip to hand over,
 * which threw the parsed headers away with it and made the rebuild parse them again.
 */
struct fwd_pre_t {
    /** @brief The leading headers were read: a structured FWD, an op VALUE and a `dst` in one
     *         of the forms @ref fwd_dst_kind_t names other than `NONE`. False ⇒ only the op
     *         fields below may be filled (see @ref op_body_len). */
    bool valid = false;
    std::size_t body_end = 0; /**< @brief End of the FWD body. */
    std::size_t op_pos = 0;   /**< @brief Offset of the op VALUE TLV. */
    std::size_t op_total = 0; /**< @brief Its total size. */

    std::size_t op_body_off = 0; /**< @brief Its body — read to test for REPLY. */
    /** @brief Its body length. Carried rather than re-checked so the rebuild keeps its own
     *         `body_len == 0` rejection: the peek does NOT reject an empty op (such a frame
     *         falls through to the terminus decode today), and making the peek stricter would
     *         silently turn a dropped frame into a terminus one.
     *
     *         The op fields are filled as soon as the op is read, before the `dst` is judged,
     *         so the terminus split reads the opcode off them even for a `dst` the peek
     *         refuses. Non-zero here is exactly the frame `peek_fwd_op` answers for. */
    std::size_t op_body_len = 0;
    std::size_t dst_body_off = 0; /**< @brief First byte of the dst PATH body. */
    std::size_t dst_end = 0;      /**< @brief End of the dst PATH body. */
    std::size_t after_dst = 0;    /**< @brief First byte after the dst PATH TLV. */
    /**
     * @brief The FIRST `dst` segment's `[body_off, body_len)` — the gate's own read, kept.
     *
     * @ref peek_fwd_dst must parse this header anyway: "the leading child is a NAME" is the
     * gate that decides a `dst` is an address at all. It used to throw the parsed offsets
     * away, and the descent immediately re-read the identical four bytes — one duplicated
     * `parse_header` on EVERY forward hop, which is a per-frame cost the pre-lift peek did
     * not pay (its one walk both gated and collected). Carrying the two integers forward
     * removes the duplicate without moving the gate: the same header, read once, decides the
     * same thing. Meaningless unless the peek answered `PATH`.
     */
    std::size_t seg0_off = 0;
    std::size_t seg0_len = 0; /**< @brief Length of the first `dst` segment's body. */
    /** @brief Where the surviving `dst` starts after this hop consumes its leading records.
     *
     *         The peek sets it past element 0 for a bound `dst` (each hop consumes exactly one
     *         element, RFC-0024 §4.1) and to @ref dst_body_off — nothing consumed — for a
     *         canonical one, whose strip the mount descent decides and writes here. The
     *         rebuild consumes `[dst_body_off, strip_at)` as given and refuses a value past
     *         @ref dst_end, so a strip the caller could not establish is a drop, never a
     *         re-walk. */
    std::size_t strip_at = 0;
    /**
     * @brief The `dst` header's type — and the type the rebuilt frame's shrunk `dst` is
     *        headed with: `PATH`, or `PATH_REF` for a BOUND address (RFC-0024 §4).
     *
     * The peek records the inbound type, so the outgoing one is the same: a canonical `dst`
     * stays canonical. Since RFC-0029 S1 the router forwards no `PATH_REF` `dst` at all — it
     * refuses that spelling as an address (§5.3), and a PAIR-spelled `dst` is a `PATH` whose
     * consumed head leaves a `PATH` — so every frame it rebuilds is headed `PATH`. The
     * `PATH_REF` arm of the peek survives for the codec-only vectors until S2 retires it.
     *
     * Value-initialised to `type_t{}` (no type), so a default `fwd_pre_t` stays all-zero and
     * resetting one per frame is a plain clear; the peek writes it whenever it reads a `dst`.
     */
    wire::type_t dst_type{};
    /**
     * @brief The outer FWD header's decoded `opt` bits — the peek's own read, kept (#1109).
     *
     * The rebuild needs them for exactly one thing: preserving the frame's trailer-timestamp
     * across the hop (`opt.TS`/`opt.TF` name the trailer window at `body_end` that the fresh
     * head must re-claim and the gather must re-emit — without them the origin's stamp is
     * silently dropped at the first forwarder). Filled with the op fields.
     */
    wire::opt_t fwd_opt{};
};

/**
 * @brief Which of the two routable `dst` forms a FWD frame carries.
 *
 * The two forms are mutually exclusive by the `dst`'s own type code, and telling them apart
 * is ONE read of the frame's three leading headers — so it is one function that answers, not
 * two gates run in sequence. Running them in sequence is what put a whole second header walk
 * on every bound frame (a shipped shape once RFC-0024 lands) while buying the canonical form
 * nothing at all.
 */
enum class fwd_dst_kind_t : std::uint8_t {
    NONE,     /**< @brief Not a structured FWD, or a `dst` in neither routable form. */
    PATH,     /**< @brief A canonical `PATH` of NAMEs — the mount descent's address. */
    PATH_REF, /**< @brief A BOUND address (RFC-0024 §4) — a fixed-stride element array. */
    /**
     * @brief A canonical `PATH` whose FIRST record is an escape — RFC-0027's labelled address.
     *
     * Told apart from the plain `PATH` answer because the two take different routes and must not be
     * confused: a canonical `PATH` opens with a NAME the mount descent folds a digest over, and
     * this opens with a record that is not a name at all. Descending it would read a peer-supplied
     * slot index as UTF-8.
     *
     * A node that does not implement minting treats it exactly as it treated a non-NAME
     * leading child before RFC-0027 existed — it names no mount here, so the frame falls to
     * the terminus arm and is refused there. That is why this is a distinct answer rather than
     * a flag on the plain `PATH` answer: the ONE branch a non-minting node takes on it is the one
     * it already took, and it takes it without a table, without a lookup, and without reading the
     * record.
     */
    PATH_LABEL,
    /**
     * @brief A canonical `PATH` with NO records — a fully consumed address.
     *
     * The shape a terminating REPLY carries (its `dst` is the request's accumulated `src`, and
     * every hop on the way back consumed its own part of it), and the shape of a request
     * addressed to this node's own root. There is nothing to descend, so it takes neither the
     * mount descent nor a bound arm; it terminates here. It is an answer of its own, with the
     * headers kept in the filled `fwd_pre_t`, so the terminus split and the refusal
     * correlation read their offsets instead of parsing the same headers again (#1794).
     */
    EMPTY,
};

/**
 * @brief Open the `dst` window of a FWD frame and say WHICH form it is — the routing gate.
 *
 * Fills @p pre with everything the descent, the bound hop and the head rebuild need about the
 * frame's structure: where the op VALUE and the `dst` body are, and where the body ends. It
 * reads NO segments and materializes nothing, so its cost and its stack are the same whatever
 * the `dst`'s depth or element count.
 *
 * The arms diverge only at the `dst` header's type code (plus the `EMPTY` answer, a canonical
 * `PATH` with no records):
 *   - `PATH` — the canonical address, a packed record run with `opt.PL = 0` (RFC-0018). The
 *     leading record must be a LITERAL segment (a `dst` whose first record is the label
 *     escape is not an address this node can descend), and @ref fwd_pre_t::strip_at starts at
 *     the body because only `strip_k` can say how much of it this hop consumes.
 *   - `PATH_REF` — the bound address. The four STRUCTURAL rules (`opt.PL = 0`, `opt.LL = 0`,
 *     `length % 8 == 0`, `length <= 2040`) are checked through `tr::wire::path_ref_body_valid`
 *     — the one locus that owns them — and @ref fwd_pre_t::strip_at is known HERE, past
 *     element 0: each hop consumes exactly one element (§4.1), with no descent to wait for.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 * @param  pre    Filled on every non-`NONE` answer, with `valid` set. On `NONE` it is reset
 *                with `valid = false`, and only its op fields may be filled (when the frame is
 *                a structured FWD with an op VALUE), which is what the terminus split reads.
 * @param  ref_count Written with the `PATH_REF` element count on the `PATH_REF` answer, 0
 *                otherwise. **1 is the terminus** (the residual is this node's own reference
 *                to the target vertex); **> 1 is a forwarder hop**; **0 is a route with no
 *                hops**, which the codec deliberately admits and the router refuses.
 *
 * @note `flatten` — see @ref rebuild_fwd_forward for the measurement. The four
 *       @ref read_fwd_header calls below are this function's whole body, and each returns a
 *       ~56-byte `std::optional<fwd_hdr_t>` that an out-of-line call must return through
 *       memory.
 */
template <class Cursor>
[[gnu::flatten]] [[nodiscard]] fwd_dst_kind_t peek_fwd_dst_any(const Cursor& cur, fwd_pre_t& pre,
                                                               std::size_t& ref_count) {
    pre = fwd_pre_t{};
    ref_count = 0;
    const auto fwd_h = read_fwd_header(cur, 0);
    if (!fwd_h || fwd_h->type != wire::type_t::FWD || !fwd_h->opt.pl) return fwd_dst_kind_t::NONE;
    const std::size_t body_end = fwd_h->body_off + fwd_h->body_len;
    const auto op_h = read_fwd_header(cur, fwd_h->body_off);
    if (!op_h || op_h->type != wire::type_t::VALUE) return fwd_dst_kind_t::NONE;
    // The op is known from here, whatever the `dst` turns out to be, so every answer below
    // fills it — `NONE` included: the terminus split reads the opcode off these offsets for
    // every FWD that reaches it, including one whose `dst` this peek refuses (`op_body_len`).
    //
    // Stored only once the last header is read, never between reads, and the order is
    // measured: a `std::size_t` store through @p pre is one the compiler must assume aliases
    // the cursor's own length word, so each header read after it reloads the cursor (+1.5 ns
    // on the resolve leg). Filling a local and copying it out instead cost +5 ns.
    const auto fill_op = [&] {
        pre.fwd_opt = fwd_h->opt;
        pre.body_end = body_end;
        pre.op_pos = fwd_h->body_off;
        pre.op_total = op_h->total;
        pre.op_body_off = op_h->body_off;
        pre.op_body_len = op_h->body_len;
    };
    const auto refuse = [&] {
        fill_op();
        return fwd_dst_kind_t::NONE;
    };
    const std::size_t dst_pos = fwd_h->body_off + op_h->total;
    if (dst_pos >= body_end) return refuse();
    const auto dst_h = read_fwd_header(cur, dst_pos);
    if (!dst_h) return refuse();
    const std::size_t dst_end = dst_h->body_off + dst_h->body_len;
    // Every accepted form: the headers are read (`valid`), and the `dst` window is open with
    // nothing consumed yet — a canonical `dst`'s descent overwrites `strip_at`.
    const auto accept = [&] {
        fill_op();
        pre.valid = true;
        pre.dst_type = dst_h->type;
        pre.dst_body_off = dst_h->body_off;
        pre.dst_end = dst_end;
        pre.after_dst = dst_pos + dst_h->total;
        pre.strip_at = dst_h->body_off;
    };
    if (dst_h->type == wire::type_t::PATH_REF) {
        if (!wire::path_ref_body_valid(dst_h->opt.pl, dst_h->opt.ll, dst_h->body_len))
            return refuse();
        accept();
        // Element 0 is this hop's own, and consuming it is not conditional on anything the
        // descent decides — there is no descent. So the shrink is known here, unlike the
        // canonical arm's, which has to wait for `strip_k`. Clamped for the H = 0 body.
        pre.strip_at = std::min(dst_h->body_off + wire::kPathRefElementBytes, dst_end);
        ref_count = wire::path_ref_element_count(dst_h->body_len);
        return fwd_dst_kind_t::PATH_REF;
    }
    if (dst_h->type != wire::type_t::PATH || dst_h->opt.pl) return refuse();
    if (dst_h->body_len == 0) {
        accept();
        return fwd_dst_kind_t::EMPTY;
    }
    // Segment 0 is a packed record, and WHICH kind it is decides which arm routes the frame.
    // A literal record is the canonical address the mount descent walks; an ESCAPE is
    // RFC-0027's labelled address, which has no name to descend and is answered by the label
    // branch instead (or, on a node that does not mint, by the terminus arm — the same
    // fall-through a non-NAME leading child has always taken). Its `seg0_off`/`seg0_len` stay
    // zero for an escape: a descent that read them would read the escape's payload as a name.
    const auto seg0 = read_packed_seg(cur, dst_h->body_off, dst_end);
    if (!seg0) return refuse();
    accept();
    if (seg0->escape) return fwd_dst_kind_t::PATH_LABEL;
    pre.seg0_off = seg0->body_off;
    // `total - 1`, not `body_len` (the same value for a literal record: one length byte, then
    // the name). Copying the two adjacent fields made GCC 13 reload them from the stack as one
    // 16-byte load over two 8-byte stores, a store-forwarding stall measured at +1.5 % on the
    // whole fwd-demux hop.
    pre.seg0_len = seg0->total - 1;
    return fwd_dst_kind_t::PATH;
}

/**
 * @brief Open the `dst` window of a FWD frame — the mount descent's gate, read by OFFSET.
 *
 * The canonical arm of @ref peek_fwd_dst_any, for a caller that routes only the canonical
 * form (the unit tests and `bench_forward_demux`). Fills @p pre with everything the descent
 * and the head rebuild need about the frame's structure: where the op VALUE and the `dst`
 * PATH body are, and where the body ends. It reads NO segments and materializes nothing, so
 * its cost and its stack are the same whatever the `dst`'s depth — the point of #523. Segments
 * are then walked lazily
 * through @ref dst_seg_walk_t, one at a time, only as far as the registry actually asks.
 *
 * This replaces `peek_fwd_dst_segs`, which eagerly filled a `kMountPeekMax`-sized array of
 * offsets. That array was the width bound's last physical residue: it decided in advance
 * how many segments the descent could ever look at, and sizing it by the widest mount would
 * have put a W-sized array on every rope frame (measured on rv32: 592 B of stack at W=4,
 * 2912 B at W=33). Nothing here is sized by a width at all.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 * @param  pre    Filled on success; reset with `valid = false` on every failure, so a caller
 *                cannot pass stale offsets to the rebuild.
 * @retval false  Not a structured FWD with an op VALUE, a non-empty `dst` PATH, and a
 *                leading NAME segment — the caller falls through to the terminus/control
 *                arms exactly as it did on the old `n == 0`. A well-formed BOUND `dst` is
 *                among the false answers, and @p pre is cleared for it too.
 */
template <class Cursor>
[[nodiscard]] bool peek_fwd_dst(const Cursor& cur, fwd_pre_t& pre) {
    std::size_t ref_count = 0;
    if (peek_fwd_dst_any(cur, pre, ref_count) == fwd_dst_kind_t::PATH) return true;
    pre = fwd_pre_t{};  // a bound `dst` fills it; this gate's contract is "cleared on false"
    return false;
}

/**
 * @brief Open the `dst` window of a BOUND FWD frame — the bound hop's gate (RFC-0024 §5).
 *
 * The bound arm of @ref peek_fwd_dst_any, for a caller that has only the bound question to
 * ask (the conformance and unit tests). The router asks BOTH questions at once, because a
 * frame is one form or the other and finding out twice is a second header walk for nothing.
 *
 * Fills @p pre as @ref peek_fwd_dst_any does on its `PATH_REF` answer, and sets
 * @ref fwd_pre_t::strip_at past element 0 — the ONE element this hop consumes (§4.1: each hop
 * consumes element 0 and forwards the remainder, the same monotone shrink the canonical `dst`
 * performs, which is why a bound path is loop-free by construction and needs no visited set).
 *
 * The four STRUCTURAL rules (`opt.PL = 0`, `opt.LL = 0`, `length % 8 == 0`, `length <= 2040`)
 * are checked through `tr::wire::path_ref_body_valid` — the one locus that owns them — so a
 * frame that fails any of them is not a bound address and falls through to the caller's
 * terminus arm, where the resolver refuses it as it refuses every other malformed `dst`.
 *
 * @retval std::nullopt Not a structured FWD whose `dst` is a structurally valid `PATH_REF`.
 * @return The element count on the wire. **1 is the terminus** (the residual is this node's own
 *         reference to the target vertex); **> 1 is a forwarder hop**; **0 is a route with no
 *         hops**, which the codec deliberately admits and the router refuses.
 */
template <class Cursor>
[[nodiscard]] std::optional<std::size_t> peek_fwd_dst_ref(const Cursor& cur, fwd_pre_t& pre) {
    std::size_t ref_count = 0;
    if (peek_fwd_dst_any(cur, pre, ref_count) != fwd_dst_kind_t::PATH_REF) {
        pre = fwd_pre_t{};  // a canonical `dst` fills it; this gate is cleared on nullopt
        return std::nullopt;
    }
    return ref_count;
}

/**
 * @brief Read the 8-byte `PATH_REF` element at @p off through the cursor seam.
 *
 * Byte-wise rather than through `tr::wire::path_ref_element_at`, because on the rope tier
 * an element may straddle a link boundary and there is then no span to hand that function.
 * Eight `byte_at` calls need no scratch, no stitch slot and no flatten, which is the property
 * that lets a bound hop stay allocation-free on a fragmented frame; the codec's own reader
 * stays the one that serves a contiguous body.
 *
 * @note Precondition: `off + 8` is inside the frame — the caller has already had the body
 *       shape settled by @ref peek_fwd_dst_ref and knows the element count.
 */
template <class Cursor>
[[nodiscard]] wire::path_ref_element_t read_path_ref_element(const Cursor& cur, std::size_t off) {
    const auto u32_at = [&](std::size_t at) {
        return static_cast<std::uint32_t>(cur.byte_at(at)) |
               (static_cast<std::uint32_t>(cur.byte_at(at + 1)) << 8) |
               (static_cast<std::uint32_t>(cur.byte_at(at + 2)) << 16) |
               (static_cast<std::uint32_t>(cur.byte_at(at + 3)) << 24);
    };
    return wire::path_ref_element_t{.index = u32_at(off), .generation = u32_at(off + 4)};
}

/**
 * @brief The trailing bound-path child a forwarded frame carries — the mint list so far, in
 *        whichever direction the caller asked for.
 *
 * @see peek_trailing_mint
 */
struct trailing_mint_t {
    std::size_t pos = 0;      /**< @brief Offset of the `PATH_REF` child's own header. */
    std::size_t body_len = 0; /**< @brief Length of the element array already on the wire. */
    /**
     * @brief False ⇔ the list is at the normative element cap and one more would not be
     *        spellable, so this hop MUST strip it rather than relay it (§7.1 erratum 1).
     */
    bool can_contribute = false;
};

/**
 * @brief The mint supplier of a hop that contributes nothing — @ref rebuild_fwd_forward's
 *        default, and the shape every caller outside the router has.
 *
 * A callable rather than a pointer so the router's own supplier can be a closure that runs
 * ONLY when the frame turns out to carry an extendable mint answer. Returning `nullopt` here
 * does not relay the answer: the rebuild STRIPS it, which is the §7.1 erratum-1 rule.
 */
struct no_mint_t {
    /** @brief Nothing to give. */
    [[nodiscard]] std::optional<wire::path_ref_element_t> operator()() const noexcept {
        return std::nullopt;
    }
};

/**
 * @brief Where a forwarded frame's trailing mint list sits, if it carries one (RFC-0024 §7.1).
 *
 * A mint list rides its frame as the LAST child, so a hop that wants to contribute its own
 * element looks exactly there and nowhere else. The presence of that child IS the signal that
 * the origin asked for a mint — a hop holds no per-flow state and has nothing else to read it
 * from, which is what keeps the accumulation stateless.
 *
 * **The two directions are told apart by TYPE, never by position** (RFC-0024 §7.1
 * amendment 2): the forward mint ANSWER on a reply is a `PATH_REF` (`0x14`), the REVERSE list
 * on a mint-flagged request a `PATH_REF_REVERSE` (`0x15`). @p want is that discriminant, and
 * it is free: the loop below already compares each tail child's type byte, so asking for the
 * other constant is the same compare. A positional rule ("the only trailing child") would
 * have cost the same here and foreclosed a raw `PATH_REF` payload on a mint-flagged WRITE,
 * which is why the type carries the role.
 *
 * **`want` is a RUNTIME parameter, and that is measured, not stylistic.** As a template
 * parameter it reads better and folds to an immediate — and it costs **+14% on the fixed
 * forward hop and +23% on the 64-link demux scan** (`bench_forward_demux`, reproduced against
 * `main`), because two instantiations stop being one shared out-of-line function and get
 * inlined into the two `noinline` mint helpers instead, which repartitions the `flatten`ed
 * `rebuild_fwd_forward` the whole demux path runs. This is exactly the hazard the mint
 * helpers' own `noinline` notes describe. One shared copy, one register argument: the
 * pre-amendment code shape, and level with it.
 *
 * FINDING the answer and being able to ADD to it are two different answers, and the caller
 * needs both: a hop that finds a list it cannot extend MUST STRIP it (§7.1 erratum 1), never
 * relay it. A relayed list that skips a hop is not a shorter route, it is a WRONG one — see
 * the strip branch in @ref rebuild_fwd_forward for the mis-route it produces. Reporting a
 * full-cap list as "not found" would take exactly that forbidden branch, so the cap rides back
 * as @ref trailing_mint_t::can_contribute rather than as a `nullopt`.
 *
 * @param cur   Cursor over the frame.
 * @param from  First byte after `src` — where the frame's trailing children begin.
 * @param end   End of the FWD body.
 * @param want The mint list's type: `PATH_REF` on a reply, `PATH_REF_REVERSE` on a
 *              mint-flagged request (RFC-0024 §7.1 amendment 2). A RUNTIME parameter, and
 *              deliberately so — see the note above on why a template one is not free here.
 * @retval std::nullopt No trailing child of type @p want at all, or a malformed tail. The
 *         frame carries no mint exchange in this direction and is forwarded untouched.
 */
template <class Cursor>
[[nodiscard]] std::optional<trailing_mint_t> peek_trailing_mint(
    const Cursor& cur, std::size_t from, std::size_t end,
    wire::type_t want = wire::type_t::PATH_REF) {
    std::size_t pos = from;
    std::size_t ref_pos = 0;
    std::size_t ref_body_len = 0;
    bool found = false;
    while (pos < end) {
        const auto h = read_fwd_header(cur, pos);
        if (!h || h->total == 0 || pos + h->total > end) return std::nullopt;
        // ONE compare, exactly as before amendment 2 gave the reverse list its own code:
        // a register compare where it used to be an immediate one. No cursor read and no
        // body peek is added to any hop by the discriminant being a type, not a position.
        found = h->type == want;
        ref_pos = pos;
        ref_body_len = h->body_len;
        if (found && !wire::path_ref_body_valid(h->opt.pl, h->opt.ll, h->body_len))
            return std::nullopt;
        pos += h->total;
    }
    if (!found) return std::nullopt;
    return trailing_mint_t{
        .pos = ref_pos,
        .body_len = ref_body_len,
        .can_contribute = wire::path_ref_element_count(ref_body_len) < wire::kMaxPathRefElements,
    };
}

/**
 * @brief A FORWARD-ONLY walker over a `dst`'s leading NAME segments (#523).
 *
 * Hands out segment `i` as `[body_off, body_len)` on demand and remembers where it stopped,
 * so the descent's natural ascending access pattern (`0, 1, 2, …`) costs ONE walk of the
 * headers however many slots ask. A request for an index BEHIND the cursor restarts from the
 * `dst` body — which happens only when a narrower registry slot is tested after a wider one,
 * and costs a handful of 4-byte header reads.
 *
 * Its whole state is three integers. That is what lets the mount width be unbounded: there is
 * no array to size, so the router's stack frame does not grow with the deepest `dst` it may
 * ever see, and no deep-peek scratch has to be drawn from the injected `flat` backend either.
 *
 * Offsets, not spans, for the reason every peek here uses them: the source may be a rope, so
 * the caller re-slices from its own cursor.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 */
/**
 * @brief How many segment offsets a @ref dst_seg_walk_t keeps inline: ONE CACHE LINE's worth.
 *
 * NOT a width bound and not a new constant to raise — it is a CACHE. The walk is correct, and
 * gives the same answers, at any width with any value here (including the structural floor of
 * two); past the cached run it simply re-reads headers. Nothing about which mounts resolve
 * depends on it, which is exactly what `kMountPeekMax` could not say.
 *
 * Sized from `tr::kCacheLineBytes`, the config quantity a target already declares (ADR-0068
 * §3; `-DLIBTRACER_CACHE_LINE_BYTES`), because the whole point is that the cached run costs no
 * extra line fetch. A config that declares no cache line (`0`, the single-core profile) still
 * gets the floor of TWO — the two the descent structurally needs, since it reads segment `k`
 * to see whether the address continues and then asks where segment `k-1` ended.
 */
inline constexpr std::size_t kDstSegCacheSlots =
    graph::kCacheLineBytes / sizeof(std::pair<std::size_t, std::size_t>) < 2
        ? std::size_t{2}
        : graph::kCacheLineBytes / sizeof(std::pair<std::size_t, std::size_t>);

/**
 * @brief A FORWARD-ONLY walker over a `dst`'s leading NAME segments (#523).
 *
 * Hands out segment `i` as `[body_off, body_len)` on demand. The mount descent walks a `dst`
 * TWICE by nature — once to fold the digest chain the registry scan filters on, once to
 * CONFIRM the one candidate that survived it — so a purely forward walker re-parsed every
 * header of the run per frame, and that showed up as a measured latency regression on the
 * `W <= 3` shapes that already worked. The first @ref kDstSegCacheSlots offsets are therefore
 * remembered; past them a backwards ask resumes from the last cached one rather than from the
 * `dst` body, so even a very deep mount re-reads only the uncached tail.
 *
 * Its whole state is that fixed cache plus three integers — a CONSTANT, config-derived stack
 * cost. That is what lets the mount width be unbounded: nothing here is sized by W, so the
 * router's stack frame does not grow with the deepest `dst` it may ever see, and no deep-peek
 * scratch has to be drawn from the injected `flat` backend either. (The measured alternative —
 * a W-sized peek array — cost 592 B of rv32 stack at W=4 and 2912 B at W=33, per rope frame.)
 *
 * Offsets, not spans, for the reason every peek here uses them: the source may be a rope, so
 * the caller re-slices from its own cursor.
 *
 * **It BORROWS the cursor, which must outlive it** — and it is the only thing in this cluster
 * that does. Every peek here (@ref peek_fwd_dst, @ref peek_fwd_first_dst_seg,
 * @ref rebuild_fwd_forward) consumes its cursor within the call and hands back offsets, so a
 * caller may pass a temporary to any of them; this walker keeps reading through the cursor
 * after the constructor returns, so a temporary there is a dangling read on the first
 * @ref at. Stated here because it was not, and because a caller cannot infer it from a
 * signature that takes `const Cursor&` like every other function on the page — the rvalue
 * constructors below turn the mistake into a COMPILE error rather than a sanitizer finding.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 */
template <class Cursor>
class dst_seg_walk_t {
   public:
    /**
     * @brief Walk the `dst` window @p pre describes, over @p cur.
     *
     * @p cur is BORROWED — this object holds a pointer to it and reads through that pointer
     * on every @ref at, so @p cur must outlive the walk. @p pre is copied (three integers),
     * so it need not.
     */
    dst_seg_walk_t(const Cursor& cur, const fwd_pre_t& pre) noexcept
        : cur_(&cur), body_off_(pre.dst_body_off), end_(pre.dst_end), pos_(pre.dst_body_off) {}

    /**
     * @brief A TEMPORARY cursor is refused at compile time (both value categories).
     *
     * `dst_seg_walk_t<span_cursor_t> w(span_cursor_t{frame}, pre);` reads exactly like the
     * `peek_fwd_dst(span_cursor_t{frame}, pre)` one line above it and is the one spelling that
     * is wrong: the temporary dies at the end of the full expression and every later `at()`
     * reads a dead stack slot. That is not hypothetical — it is what a test wrote and what
     * ASan caught as `stack-use-after-scope` through `read_packed_seg`. Deleting these makes
     * the shape unrepresentable instead of merely documented, at zero runtime cost; a caller
     * with a temporary in hand names it first, which is what the three in-tree call sites
     * already do.
     */
    dst_seg_walk_t(Cursor&&, const fwd_pre_t&) = delete;
    /** @brief The `const` rvalue spelling of the same mistake — deleted for the same reason. */
    dst_seg_walk_t(const Cursor&&, const fwd_pre_t&) = delete;

    /**
     * @brief Segment @p i's `[body_off, body_len)`.
     * @retval std::nullopt The `dst` has no segment @p i — it ended, or the next child is
     *                      not a NAME (a selector, say), which is where an ADDRESS stops.
     */
    /**
     * @brief Fill the inline cache NOW, in ONE tight loop.
     *
     * The descent's first act is to materialize the cached run, and doing it through `at`
     * meant one out-of-line walk call per segment — a profile of the forward hop put a quarter
     * of it there, because the header parse makes that half of `at` too large to inline.
     * One call fills the whole run; every later ask is then the inlined cache half of `at`.
     *
     * Purely an optimisation. Every answer is identical without it, and a `dst` deeper than
     * the cached run is still walked on demand — which is exactly what makes this a CACHE and
     * not the fixed peek window it replaced.
     */
    void prefill() {
        while (cached_ < kDstSegCacheSlots && pos_ < end_) {
            const auto r = read_packed_seg(*cur_, pos_, end_);
            if (!r) return;
            if (r->escape) {  // step over a label record this node does not implement
                pos_ += r->total;
                continue;
            }
            last_ = {r->body_off, r->body_len};
            cache_[cached_++] = last_;
            pos_ += r->total;
            ++have_;
        }
    }

    /**
     * @brief Segment @p i's `[body_off, body_len)`.
     * @retval std::nullopt The `dst` has no segment @p i — it ended, or the next child is
     *                      not a NAME (a selector, say), which is where an ADDRESS stops.
     */
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> at(std::size_t i) {
        // Split deliberately: this half is a bounds compare and a load, so it inlines into
        // every call site, and the descent calls it several times per frame. Fused with the
        // walk below it did NOT inline — the header parse makes the body too large — and a
        // profile of the forward hop put 35% of it in this one out-of-line call.
        if (i < cached_) return cache_[i];
        return walk_to(i);
    }

   private:
    /** @brief The uncached half of `at`: walk forward (restarting from the cache if the
     *         ask is behind it) until segment @p i is in hand. */
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> walk_to(std::size_t i) {
        if (i < have_) {
            // Behind the walk but past the cache: resume from the last cached segment. A
            // packed record's payload is its tail, so the next record starts at `off + len`
            // — no stored "next position" and no second field per entry.
            have_ = cached_;
            pos_ =
                cached_ == 0 ? body_off_ : cache_[cached_ - 1].first + cache_[cached_ - 1].second;
        }
        while (have_ <= i) {
            if (pos_ >= end_) return std::nullopt;
            const auto r = read_packed_seg(*cur_, pos_, end_);
            if (!r) return std::nullopt;
            pos_ += r->total;
            if (r->escape) continue;  // stepped over, not counted as a segment
            last_ = {r->body_off, r->body_len};
            if (have_ < kDstSegCacheSlots) cache_[cached_++] = last_;
            ++have_;
        }
        return last_;
    }

   public:
    /**
     * @brief Where segment @p i ENDS — the `strip_at` the head rebuild wants.
     *
     * A packed record's payload is its tail, so the consumed run ends at
     * `body_off + body_len` of the last stripped segment. Returns `std::nullopt` if that
     * segment does not exist. An escape record STEPPED OVER inside the run is inside
     * `[body, end_of(i))` too, so the strip stays a single contiguous shrink.
     */
    [[nodiscard]] std::optional<std::size_t> end_of(std::size_t i) {
        const auto s = at(i);
        if (!s) return std::nullopt;
        return s->first + s->second;
    }

   private:
    const Cursor* cur_;
    std::size_t body_off_;
    std::size_t end_;
    std::size_t pos_;
    std::size_t have_ = 0;   /**< @brief Segments walked; `last_` is number `have_-1`. */
    std::size_t cached_ = 0; /**< @brief Entries of `cache_` filled (`<= have_`). */
    std::pair<std::size_t, std::size_t> last_{0, 0};
    /** @brief Uninitialised on purpose: `cached_` gates every read, and zeroing a
     *         cache line per frame is a cost the pre-lift descent did not pay. */
    std::array<std::pair<std::size_t, std::size_t>, kDstSegCacheSlots> cache_;
};

/**
 * @brief The forward dispatch decision, read by OFFSET with no allocation
 *        (ADR-0038 inv. #1, ADR-0039).
 *
 * A FWD whose first `dst` segment names a transport child is a forward hop that
 * never needs the decoded tree. Returns the `[body_off, body_len)` of the first
 * packed dst-segment record iff the frame is a structured FWD with an op VALUE + a
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
    if (!dst_h || dst_h->type != wire::type_t::PATH || dst_h->opt.pl || dst_h->body_len == 0)
        return std::nullopt;
    // dst record[0] = the first packed segment (RFC-0018); an escape names no transport child
    const auto seg0 = read_packed_seg(cur, dst_h->body_off, dst_h->body_off + dst_h->body_len);
    if (!seg0 || seg0->escape) return std::nullopt;
    return std::pair{seg0->body_off, seg0->body_len};
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
    // MASKED, never switched on raw (RFC-0024 §9.3): bits 7-6 are flags, so an op byte
    // carrying a bind request must peek as the plain opcode it also is. Switching on the raw
    // byte here would make a mint-flagged READ an unknown opcode at every forwarder on the
    // route — a clean error, but an error, and the whole point of spending an existing byte's
    // spare bits is that a peer which ignores the flag still routes the operation.
    return static_cast<graph::fwd_op_t>(cur.byte_at(op_h->body_off) & graph::kFwdOpcodeMask);
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
class stack_writer_t {
   public:
    /**
     * @brief Append a structured TLV header (`pl` set, `ll` auto-widened) for @p body_len.
     *
     * @p trailer contributes its TS/TF bits alone (#1109) — the builder can now EXPRESS a
     * trailer, in either form, so a forwarded frame's origin stamp survives the head rebuild
     * (the caller that sets them owns emitting the trailer bytes after the body). CR never
     * crosses: a rebuilt body invalidates any inbound CRC by construction, so preserving the
     * bit would mint a frame its own receiver rejects as `crc_fail`.
     */
    void header(wire::type_t type, std::size_t body_len, wire::opt_t trailer = {}) {
        put_header(type,
                   wire::opt_t{.pl = true,
                               .ts = trailer.ts,
                               .ll = body_len > 0xFFFFu,
                               .tf = trailer.ts && trailer.tf},
                   body_len);
    }
    /**
     * @brief Append a ROUTE header — a `PATH`, `PATH_REF` or `PATH_REF_REVERSE` — for
     *        @p body_len, with `opt.PL = 0`.
     *
     * Its own method rather than @ref header, because a route body is NOT a child run: a packed
     * `PATH` body is a record run (RFC-0018 §5) and a `PATH_REF` body a fixed-stride element
     * array (RFC-0024 §4.2), and `opt.PL = 1` on either would make a generic walker read the
     * first body bytes as a TLV header and mis-frame the whole address — a MUST, not a
     * preference.
     *
     * `LL` widens for a `PATH`, whose body may legally pass 0xFFFF. It never widens for the
     * bound types: their element bound caps the body at 2040 bytes, so a @p body_len that
     * claims otherwise overflows rather than widening. One method for every route type is what
     * lets the forward hop head its shrunk `dst` with the type the peek carried
     * (@ref fwd_pre_t::dst_type) instead of choosing between two writers.
     */
    void header_route(wire::type_t type, std::size_t body_len) {
        const bool wide = body_len > 0xFFFFu;
        if (wide && type != wire::type_t::PATH) {
            overflow_ = true;
            return;
        }
        put_header(type, wire::opt_t{.ll = wide}, body_len);
    }
    /**
     * @brief The wire bytes a header this writer emits for @p body_len occupies — 4, or 6 once
     *        the length needs a u32.
     *
     * The writer reports its own width, so a caller sizing a parent body counts a child header
     * by the rule that will write it rather than by a copy of that rule.
     */
    [[nodiscard]] static constexpr std::size_t header_bytes(std::size_t body_len) noexcept {
        return wire::header_bytes(wire::opt_t{.ll = body_len > 0xFFFFu});
    }

    /** @brief Append one packed PATH segment record over @p s (`[u8 len][bytes]`, RFC-0018).
     *         An empty @p s would spell the §5.4 escape, so it overflows rather than mints. */
    void path_seg(std::string_view s) {
        if (len_ + 1 + s.size() > N || s.empty() || s.size() > wire::kPackedSegMaxBytes) {
            overflow_ = true;
            return;
        }
        buf_[len_++] = static_cast<std::byte>(s.size());
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
    /** @brief The one header layout: type, `opt`, then the u16 or u32 LE length `opt.LL`
     *         selects. A write past @p N overflows rather than truncates. */
    void put_header(wire::type_t type, wire::opt_t opt, std::size_t body_len) {
        const std::size_t width = wire::header_bytes(opt) - 2;
        if (len_ + 2 + width > N) {
            overflow_ = true;
            return;
        }
        buf_[len_++] = static_cast<std::byte>(std::to_underlying(type));
        buf_[len_++] = static_cast<std::byte>(opt.encode());
        for (std::size_t i = 0; i < width; ++i)
            buf_[len_++] = static_cast<std::byte>((body_len >> (8 * i)) & 0xFF);
    }

    std::array<std::byte, N> buf_{}; /**< @brief The fixed stack buffer. */
    std::size_t len_ = 0;            /**< @brief Bytes written so far. */
    bool overflow_ = false;          /**< @brief A write exceeded @p N. */
};

/**
 * @brief The pre-v0.18.0 spelling of @ref stack_writer_t; removed in v0.19.0 (#1723).
 * @tparam N The inline byte capacity, as for @ref stack_writer_t.
 */
template <std::size_t N>
using stack_writer = stack_writer_t<N>;

/** @brief Capacity of the forward hop's first head: FWD hdr(≤6) + op TLV(small) + PATH hdr(≤6). */
inline constexpr std::size_t kFwdHead1Cap = 64;
/** @brief Capacity of the forward hop's second head: the grown src PATH header alone. */
// The grown src PATH header alone — 2 type/opt bytes plus a 2- or 4-byte length. This is a
// STRUCTURAL bound (the widest TLV header the format has), not a budget: the prepended mount
// records are emitted as bare one-byte length prefixes and their bytes are referenced from the
// caller's storage by @ref fwd_rebuild_t::gather, never copied in here. An earlier revision copied
// them into this buffer, which silently made the buffer size a cap on how long a connection NAME
// could be — a synthetic limit on user-chosen data (forbidden by RFC-0006/0007 + ADR-0051), and
// one whose breach was a dropped LEGAL frame rather than a clean rejection. A mount path is now
// bounded only by the packed record's own `u8` length field and by the outgoing frame fitting
// the link's `max_frame`/MTU.
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
 *                         10. trailer TS        (the preserved stamp window, #1109)
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
 * At 10 the headroom to the transport spill is **7 regions**. Keep the mount one span and this
 * constant does not move when the descent is uncapped.
 *
 * **The bound-path mint accumulation (RFC-0024 §7.1) does not move it either**, and this is
 * counted rather than assumed. A forwarded REPLY's mint adds two regions — this hop's 12-byte
 * head-plus-element, and the elements already on the wire — but a REPLY grows no `src`, so the
 * mount run and the bus peer's header-and-segment pair (regions 5-7) are empty on exactly the
 * frames that use them. The two sets are mutually exclusive by `is_reply`: a REQUEST emits at
 * most head1, remaining dst, selector, head2, mount, peer header, peer segment, `src` body,
 * tail and trailer TS = **10**; a REPLY at most head1, remaining dst, selector, head2, `src`
 * body, tail, mint head, mint elements and trailer TS = **9**. It was briefly raised to 11 by
 * adding the request and mint sets together, which is a bound no frame can reach — and the
 * constant is measured, not defensive: that change moved code placement enough to cost
 * `bench_forward_rope` a disjoint **+13% at fan 2** in branch mispredicts, on a shape that
 * emits none of the regions it was raised for. (The +1 here, by contrast, is a region a
 * stamped frame really emits — the #1109 trailer-TS window, region 10 above.)
 */
inline constexpr std::size_t kFwdMaxIov = 10;

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
    stack_writer_t<kFwdHead1Cap> head1; /**< @brief FWD header + op (copied) + shrunk dst header. */
    stack_writer_t<kFwdSrcHdrCap> head2; /**< @brief The grown src PATH header. */
    /** @brief The inbound mount as ALREADY-ENCODED packed records, emitted as ONE span and never
     *         copied. Precomputed once per child (#508), so a hop does no per-segment work. */
    std::span<const std::byte> mount_tlv;
    /** @brief The one-byte packed length prefix for @ref extra_seg (RFC-0018). */
    std::array<std::byte, 1> extra_hdr;
    /**
     * @brief The inbound frame's trailer-TIMESTAMP window, re-emitted VERBATIM as the
     *        outgoing frame's last bytes (#1109) — offset in bits 0-30, FORM in bit 31.
     *
     * Zero ⇒ the frame carried no stamp, unambiguously: a trailer sits past a TLV's own
     * 4-byte header, so no stamped frame has a window at offset 0. Bit 31 (@ref kTsNarrow)
     * is the `opt.TF` form the PRODUCER chose and this hop relays rather than picks —
     * clear = the WIDE absolute stamp (8 bytes), set = the NARROW relative one (4). Read it
     * through @ref ts_off and @ref ts_bytes, never raw. The CRC half of an inbound trailer
     * is NOT here and never will be: the rebuilt body invalidates it, so it is dropped
     * rather than forwarded stale (see @ref stack_writer_t::header).
     *
     * **One 4-byte word here, rather than an offset and a width at the end of this struct,
     * is MEASURED, not tidiness** (#1235). It occupies the alignment hole after
     * @ref extra_hdr, which keeps `sizeof(fwd_rebuild_t)` at **256**. The two `std::size_t`
     * fields this replaced pushed it to 272, and that alone — with the members NEVER READ,
     * the ablation that proved it — cost `fwd-demux-fixed 79B/fan1/1ep` **p50 +9.6% /
     * throughput −9.3%** on the pinned host. A 264-byte intermediate (two `std::uint32_t`s)
     * still cost +9.5%, so the step is at 256 and it is the whole object's size that
     * matters, not the field count. Packing the form INTO the word rather than deriving it
     * from the emitted head is measured too: the derived form left the rope hop's
     * `route_fwd_forward` 16 B larger and the demux row 5 ns short of the parent, where this
     * shape returns both to it.
     *
     * A frame whose body ends past @ref kTsNarrow cannot express its window here, so the
     * rebuild drops the stamp AND its header bits together rather than declaring a trailer
     * it will not emit — a 2 GiB single TLV, which no shipped transport will carry.
     */
    std::uint32_t ts_window = 0;
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
    /**
     * @brief This hop's contribution to a mint answer: a fresh `PATH_REF` header plus ONE
     *        8-byte element (RFC-0024 §7.1 step 2). Empty ⇒ this frame carries no mint.
     *
     * Written only on a forwarded REPLY whose last child is already a `PATH_REF`. The element
     * goes FIRST in the new body, ahead of @ref ref_body_off — the elements the hops further
     * out have already contributed — because the list is origin-first and this hop is nearer
     * the origin than every host that has touched the reply so far. That is the mirror of the
     * way `src` accumulates on the way in (RFC-0004 §B), and it is a rope operation on the
     * egress rather than a rewrite: the existing elements are referenced, never copied.
     */
    stack_writer_t<4 + wire::kPathRefElementBytes> mint;
    std::size_t ref_body_off = 0; /**< @brief The trailing `PATH_REF`'s existing element array. */
    std::size_t ref_body_len = 0; /**< @brief Its length; 0 with a written @ref mint is H = 0. */
    /** @brief @ref ts_window's bit 31: the stamp is the NARROW relative form (`opt.TF` set). */
    static constexpr std::uint32_t kTsNarrow = 0x8000'0000u;

    /**
     * @brief The preserved trailer timestamp's WIDTH in bytes — 0 (none), 4 (narrow) or 8
     *        (wide), as the producer spelled it (#1109).
     *
     * The forwarder never chooses a width: it reports the one @ref ts_window recorded from
     * the inbound head, and @ref rebuild_fwd_forward sets that word and the outgoing head's
     * TS/TF bits from the same `opt` in one place, so the emitted trailer and the header
     * declaring it cannot disagree.
     */
    [[nodiscard]] std::size_t ts_bytes() const {
        return ts_window == 0 ? 0u : ((ts_window & kTsNarrow) != 0 ? 4u : 8u);
    }

    /** @brief The window's byte offset into the SOURCE cursor (bit 31 masked off). */
    [[nodiscard]] std::size_t ts_off() const { return ts_window & ~kTsNarrow; }

    /** @brief True ⇔ every head fits its stack buffer (else the caller drops). */
    [[nodiscard]] bool ok() const { return head1.ok() && head2.ok() && mint.ok(); }

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
        // The mint accumulation, when this hop contributed one: its own element ahead of the
        // ones already on the reply. `tail` stops short of the trailing `PATH_REF` in that
        // case, so the child is re-headed here rather than forwarded twice.
        if (!mint.span().empty()) {
            push(mint.span());
            if (ref_body_len > 0) cur.for_each_span(ref_body_off, ref_body_len, push);
        }
        // The preserved trailer timestamp goes LAST — after the whole body, mint included —
        // because that is where the grammar's `total = header + length + ts_size` reads it
        // (#1109). Verbatim source bytes: this hop reads no clock and rewrites no stamp.
        if (const std::size_t ts = ts_bytes(); ts > 0) cur.for_each_span(ts_off(), ts, push);
    }
};

/**
 * @brief The forward hop's stack object stays at or under **256 bytes** — a MEASURED ratchet
 *        (#1235), not a style rule.
 *
 * This object is built on the stack of every forwarded frame, and its size is load-bearing
 * on the shipped fast path: growing it to 272 by adding two `std::size_t` fields cost
 * `fwd-demux-fixed 79B/fan1/1ep` **p50 +9.6% / throughput −9.3%** on the pinned host, and an
 * ablation that added the same 16 bytes and NEVER READ THEM cost exactly the same — so the
 * price is the size, not the work. A 264-byte intermediate was still +9.5%. The regression
 * shipped for 16 samples because it walked under every PR-gate threshold; this assert is
 * what makes the next such growth a compile error instead. A field that will not fit belongs
 * in an alignment hole, or packed into a word that is already there (see
 * @ref fwd_rebuild_t::ts_window).
 */
static_assert(sizeof(fwd_rebuild_t) <= 256,
              "fwd_rebuild_t is a per-hop stack object; growing it past 256 B measurably "
              "regresses the forward demux path (#1235)");

/**
 * @brief The mint accumulation half of a forwarded REPLY's rebuild (RFC-0024 §7.1 step 2),
 *        deliberately kept OUT OF LINE.
 *
 * `noinline` against @ref rebuild_fwd_forward's `flatten`, and it is a measurement, not a
 * preference. `flatten` pulls everything a function calls into it, so inlined this dragged
 * @ref peek_trailing_mint's header loop into the rebuild's body — on the branch a REQUEST hop
 * never takes. The extra front end that bought cost `bench_forward_rope` **+13% at fan 2** in
 * branch mispredicts (3x armA's count under `perf stat`, on ~equal instructions): a shipped
 * shape paying for a form its frames cannot be. Out of line, the request hop sees one
 * not-taken branch and the rope arm measures at or below `main` at every fan.
 *
 * Writes @p r's tail and mint fields. A contribution leaves @ref fwd_rebuild_t::mint written
 * (the re-headed child and this hop's element); contributing nothing — which INCLUDES the strip
 * cases (no element to give, or a list already at the cap) — leaves it empty, and the caller
 * sizes the body from those two fields alone. A reply with no mint answer at all leaves @p r
 * untouched.
 */
template <class Cursor, class MintFn>
[[gnu::noinline]] void rebuild_reply_mint(const Cursor& cur, std::size_t pos, std::size_t body_end,
                                          MintFn& mint_fn, fwd_rebuild_t& r) {
    const std::optional<trailing_mint_t> found =
        peek_trailing_mint(cur, pos, body_end, wire::type_t::PATH_REF);
    if (!found) return;
    // The tail stops short of the mint answer either way: this hop re-heads it one element
    // longer, or removes it. It is never relayed untouched.
    r.tail_len = found->pos > pos ? found->pos - pos : 0;
    // The ONE call, made only now that the frame is known to carry an extendable answer. A
    // list already at the cap is NOT extendable, so it strips with everything else this hop
    // cannot contribute to.
    const std::optional<wire::path_ref_element_t> mint =
        found->can_contribute ? mint_fn() : std::nullopt;
    // STRIP, and it is a SAFETY rule rather than tidiness (RFC-0024 §7.1, car-3 erratum).
    // Every cannot-contribute case lands here — no connection vertex, a saturated or retired
    // generation, and a full list — because the erratum names them together and they have one
    // safe outcome between them. A list that skips a hop is not a shorter route, it is a WRONG
    // one: the origin would consume its own element, send a list one element short, and the
    // hop that could not contribute would find exactly one element left, believe itself the
    // terminus, and dereference an element minted on a DIFFERENT host against its own vertex
    // map — where the same index and generation are an ordinary live vertex. That is a
    // mis-route, which the design refuses outright. The origin sees an ordinary reply, stays
    // canonical, and loses nothing but the optimisation.
    if (!mint) return;
    r.ref_body_off = found->pos + 4;  // LL = 0 is a MUST, so the header is 4 bytes
    r.ref_body_len = found->body_len;
    r.mint.header_route(wire::type_t::PATH_REF, r.ref_body_len + wire::kPathRefElementBytes);
    std::array<std::byte, wire::kPathRefElementBytes> e{};
    wire::path_ref_store_element(e, *mint);
    r.mint.raw(e);
}

/**
 * @brief The REVERSE-direction mint on a forwarded mint-flagged REQUEST (RFC-0024 §7.1
 *        amendment 1) — the request-side mirror of @ref rebuild_reply_mint, equally
 *        OUT OF LINE and for the same measured reason (its `noinline` note applies verbatim:
 *        the tail walk must not be flattened into the hop every unflagged frame runs).
 *
 * Three outcomes, all normative:
 *
 * - **Extend.** The request's last child is already a `PATH_REF_REVERSE` (`0x15`, the
 *   reverse list's OWN type since RFC-0024 §7.1 amendment 2 — never a position) and @p mint_fn
 *   yields this hop's element for the identity the frame ARRIVED on — its connection vertex
 *   for a point-to-point link, or the accepted session's identity vertex for a bus session.
 *   The element is PREPENDED (the list runs responder-first), exactly the @ref
 *   fwd_rebuild_t::mint machinery the reply side uses.
 * - **Create.** No reverse child yet — this is the FIRST forwarding hop (the origin never
 *   emits the child) — and @p mint_fn yields an element: a fresh one-element
 *   `PATH_REF_REVERSE` is appended as the new last child. "A hop with no reverse child yet
 *   MAY create it"; the reference core participates.
 * - **Strip.** A reverse child exists but this hop cannot contribute (no identity vertex, a
 *   saturated generation, a full list): the WHOLE child is removed. Erratum 1's rule
 *   direction-reversed and equally forced — a list that skips a hop is a wrong route, the
 *   §5.3 mis-route class, so it is all-or-nothing over the reverse list alone.
 *
 * The unflagged request never reaches here (the caller gates on op bit 7), so the ordinary
 * forward hop pays one not-taken branch, exactly as it does for the reply mint.
 *
 * Writes @p r's tail and mint fields, as @ref rebuild_reply_mint does: an extension or a
 * creation leaves @ref fwd_rebuild_t::mint written (a creation with
 * @ref fwd_rebuild_t::ref_body_len = 0), and strip or no-op leaves it empty.
 */
template <class Cursor, class MintFn>
[[gnu::noinline]] void rebuild_request_reverse_mint(const Cursor& cur, std::size_t pos,
                                                    std::size_t body_end, MintFn& mint_fn,
                                                    fwd_rebuild_t& r) {
    const std::optional<trailing_mint_t> found =
        peek_trailing_mint(cur, pos, body_end, wire::type_t::PATH_REF_REVERSE);
    // The ONE call — after the frame is known mint-flagged, at most once per hop.
    const std::optional<wire::path_ref_element_t> mint =
        (!found || found->can_contribute) ? mint_fn() : std::nullopt;
    if (!found) {
        // CREATE: no reverse child yet. Nothing to strip; a hop that cannot mint forwards
        // the flagged request untouched (the strip rule binds only when a list exists).
        if (!mint) return;
        r.mint.header_route(wire::type_t::PATH_REF_REVERSE, wire::kPathRefElementBytes);
        std::array<std::byte, wire::kPathRefElementBytes> e{};
        wire::path_ref_store_element(e, *mint);
        r.mint.raw(e);
        return;
    }
    // A list exists: the tail stops short of it either way — extended one element longer, or
    // STRIPPED whole (RFC-0024 §7.1 amendment 1; erratum 1's rule direction-reversed).
    r.tail_len = found->pos > pos ? found->pos - pos : 0;
    if (!mint) return;
    r.ref_body_off = found->pos + 4;  // LL = 0 is a MUST, so the header is 4 bytes
    r.ref_body_len = found->body_len;
    r.mint.header_route(wire::type_t::PATH_REF_REVERSE,
                        r.ref_body_len + wire::kPathRefElementBytes);
    std::array<std::byte, wire::kPathRefElementBytes> e{};
    wire::path_ref_store_element(e, *mint);
    r.mint.raw(e);
}

/**
 * @brief The forward hop's head rebuild, read entirely by OFFSET — no decoded
 *        tree (ADR-0038 inv. #1).
 *
 * Layout: `FWD{ op VALUE, dst PATH, FIELD? sel, src PATH, tail }` — consumes the leading
 * `dst` records the routing decision named (@ref fwd_pre_t::strip_at), grows src by
 * @p mount_tlv (unless the op is REPLY: a reply accumulates no return route, RFC-0004 §B),
 * and synthesizes the two fresh stack heads. The caller scatter-gathers the result via
 * @ref fwd_rebuild_t::gather — no payload copy, zero heap.
 *
 * **One parse per hop (#1794).** Every header in front of the selector was read by
 * @ref peek_fwd_dst_any, and this function reads none of them again: it starts at
 * @ref fwd_pre_t::after_dst and parses only the selector and `src` headers the peek never
 * reached. It used to carry a second, self-parsing arm for a caller with no peek in hand, and
 * a segment walk for a caller that had not recorded where its strip ended; both are gone, so
 * the header logic of a forward hop lives in one function and a fix to it lands everywhere.
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
 * @param  pre           What @ref peek_fwd_dst_any read off this frame, with
 *                       @ref fwd_pre_t::strip_at set to the end of the records this hop
 *                       consumes (the mount descent's answer, or element 0 for a bound `dst`)
 *                       and @ref fwd_pre_t::dst_type to the type the shrunk `dst` is headed
 *                       with.
 * @param  mount_tlv     This node's mount path for the link the frame arrived on, ALREADY
 *                       ENCODED as a run of packed records (precomputed per child, #508).
 * @param  extra_seg     One further mount segment whose name is only known now — a bus PEER.
 *                       Empty when the mount is fully precomputed.
 * @param  mint_fn       This hop's mint contribution, supplied LAZILY: invoked at most once,
 *                       and only on a forwarded REPLY that actually carries an extendable
 *                       mint answer. Laziness is the whole point — deciding eagerly meant
 *                       reading the op byte a second time on EVERY forwarded frame,
 *                       including the request hops that can never mint, and that duplicate
 *                       read is a rope-cursor byte walk on a fragmented frame. Defaults to
 *                       @ref no_mint_t, the hop that contributes nothing.
 * @param  reverse_mint_fn The REVERSE-direction twin, invoked at most once and only on a
 *                       mint-flagged request (RFC-0024 §7.1 amendment 1).
 * @param  reply_label   RFC-0027 6.1's minted spelling of THIS hop's own local part, as the
 *                       already-encoded 7-byte label element, or empty to mint nothing.
 *
 *                       This is the one region a REPLY's `src` may grow by, and it exists
 *                       because 6.1 requires the first reply to reach the original sender
 *                       already minted: a hop's local part is stripped from the reply's `dst`
 *                       and echoed nowhere else, so a label that replaced it would otherwise
 *                       have no way home. What is prepended is strictly SHORTER than the
 *                       string run it stands for (7 bytes against a mount run's 10 and up),
 *                       which is 6.1's "replaces, never appends" in the accounting 6.1 itself
 *                       uses — the comparison is against the string spelling of the same
 *                       accumulation, not against a reply that accumulates nothing.
 *
 *                       It narrows RFC-0004 B's "a reply accumulates no return route" to
 *                       NON-MINTING hops, which is a normative tension RFC-0027 6.1 already
 *                       decided at acceptance and which is recorded as an erratum in that
 *                       RFC's log rather than assumed here. Empty is the default and the
 *                       conformant behaviour, so a node that never mints is byte-unchanged.
 * @retval std::nullopt The frame is not a well-formed forwardable FWD (no peeked headers, an
 *         empty op, a strip past the `dst`, an unspellable peer segment, or a `src` that
 *         cannot be relayed) — the caller drops it.
 * @note   A returned rebuild may still have `!ok()` (an oversized op TLV
 *         overflowed a head) — the caller must check and drop, never overrun.
 *
 * @note **`flatten`, and it is measured.** @ref read_fwd_header returns
 *       `std::optional<fwd_hdr_t>` — six words — so an OUT-OF-LINE call returns it through
 *       memory and the caller re-loads every field. Inlined it is registers. Which way the
 *       compiler goes is a budget decision it makes per caller, and it flipped the wrong way
 *       for the three header readers on the forward hop once the descent's cold arm was moved
 *       out of line and `on_frame_impl` shrank: a `perf` profile of a `W = 3` hop put **58%**
 *       of it inside an out-of-line `read_fwd_header`, against 4% on the pre-lift build where
 *       the same calls were inlined. `flatten` on the three functions that read headers in a
 *       loop (here, @ref peek_fwd_dst, and the router's `resolve_mount_at`) is worth 6-8 ns
 *       per hop across every `W` measured. It is deliberately NOT `always_inline` on
 *       @ref read_fwd_header itself: that inlines it into the cold control-frame and terminus
 *       paths too and measured **+16 to +24%** — strictly worse than doing nothing.
 */
template <class Cursor, class MintFn = no_mint_t, class ReverseMintFn = no_mint_t>
[[gnu::flatten]] [[nodiscard]] std::optional<fwd_rebuild_t> rebuild_fwd_forward(
    const Cursor& cur, const fwd_pre_t& pre, std::span<const std::byte> mount_tlv,
    std::string_view extra_seg, MintFn mint_fn = MintFn{},
    ReverseMintFn reverse_mint_fn = ReverseMintFn{}, std::span<const std::byte> reply_label = {}) {
    // The peek's rejections are the peek's; these are the ones only a rebuild can make. An
    // EMPTY op is one of them on purpose (see `fwd_pre_t::op_body_len`: the peek accepts it,
    // so the check lives here or a malformed frame would change fate). A strip past the `dst`
    // is a `dst` shorter than the mount the descent matched. The packed record's length field
    // is a `u8` (RFC-0018 §5) — the wire's own bound on a segment and now the only one on a
    // peer name: past it the name has no spelling at all.
    if (!pre.valid || pre.op_body_len == 0 || pre.strip_at > pre.dst_end ||
        extra_seg.size() > wire::kPackedSegMaxBytes)
        return std::nullopt;
    // Masked (RFC-0024 §9.3) — the flag bits say nothing about which op this is. The raw
    // byte is read ONCE and split: opcode for the reply test, bit 7 for the reverse mint's
    // gate (§7.1 amendment 1 — the reverse child rides only a mint-flagged request).
    const std::uint8_t op_byte = cur.byte_at(pre.op_body_off);
    const bool is_reply =
        static_cast<graph::fwd_op_t>(op_byte & graph::kFwdOpcodeMask) == graph::fwd_op_t::REPLY;
    const bool mint_flagged = (op_byte & graph::kFwdOpFlagMintRequest) != 0;

    std::size_t pos = pre.after_dst;
    fwd_rebuild_t r;
    if (pos < pre.body_end) {
        const auto peek = read_fwd_header(cur, pos);
        if (peek && peek->type == wire::type_t::FIELD) {
            r.sel_pos = pos;
            r.sel_total = peek->total;
            pos += peek->total;
        }
    }

    const auto src_h = read_fwd_header(cur, pos);
    if (!src_h) return std::nullopt;
    // A REPLY's `src` echoes the request's `dst`, so a reply to a BOUND request carries a
    // `PATH_REF` there — and it is forwarded, not grown (a reply accumulates no return route,
    // RFC-0004 §B). Refusing it would have dropped every reply to a bound multi-hop request at
    // the first forwarder, which is the frame the whole mint exchange rides home on.
    //
    // On a REQUEST the same shape is refused, and that asymmetry is the point: this hop grows
    // `src` by its inbound mount, and a mount NAME prepended into a fixed-stride record array
    // is not a longer route, it is a corrupt one. A request whose `src` cannot accumulate has
    // no return route, so it is dropped here rather than forwarded unanswerable.
    if (src_h->type != wire::type_t::PATH && !(src_h->type == wire::type_t::PATH_REF && is_reply))
        return std::nullopt;
    pos += src_h->total;

    r.tail_off = pos;
    r.tail_len = pre.body_end > pos ? pre.body_end - pos : 0;
    r.src_body_off = src_h->body_off;
    r.src_body_len = src_h->body_len;

    // This hop's mint contribution (RFC-0024 §7.1 + amendment 1), in either direction. A
    // forwarded REPLY carrying a mint answer gets this hop's FORWARD element prepended; a
    // mint-flagged forwarded REQUEST gets this hop's REVERSE element — its arrival identity's
    // vertex ref — prepended to (or creating, or stripping) the trailing reverse `PATH_REF`
    // child. An UNFLAGGED request is never touched — the mint request still costs zero added
    // origin bytes, which is the whole point of putting the flag in the op byte (§7.5).
    // Both mint halves are CALLS, never inlined here (see `rebuild_reply_mint`).
    if (is_reply) {
        rebuild_reply_mint(cur, pos, pre.body_end, mint_fn, r);
    } else if (mint_flagged) {
        rebuild_request_reverse_mint(cur, pos, pre.body_end, reverse_mint_fn, r);
    }

    // The leading `dst` records this hop consumes, as the routing decision recorded them. The
    // strip stays the zero-copy shrink of §5.1: the residual `dst` is emitted as ONE untouched
    // span under a fresh header, and no length table is rewritten.
    r.rem_dst_off = pre.strip_at;
    r.rem_dst_len = pre.dst_end - pre.strip_at;

    // What `src` grows by. A REQUEST grows by the inbound mount path — the precomputed run is
    // already-encoded bytes, and a dynamic bus-peer segment adds ONE length byte plus its
    // bytes. A REPLY grows by RFC-0027 §6.1's minted spelling alone, which is empty for every
    // hop that does not mint — which is every hop today — so a REPLY's `src` is byte-identical
    // to what it has always been and no shipped vector moves. The two ride the SAME fields
    // because they are the same region of the frame: the bytes prepended to `src`. The only
    // bound on a mount path is the packed record's `u8` length field; beyond that it is limited
    // solely by the frame fitting the link's `max_frame`/MTU, never by a buffer budget.
    r.mount_tlv = is_reply ? reply_label : mount_tlv;
    r.extra_seg = is_reply ? std::string_view{} : extra_seg;
    r.extra_hdr[0] = static_cast<std::byte>(r.extra_seg.size());
    const std::size_t grown =
        r.mount_tlv.size() + (r.extra_seg.empty() ? 0u : 1u + r.extra_seg.size());

    const std::size_t new_dst_body = r.rem_dst_len;
    const std::size_t new_src_body = src_h->body_len + grown;
    // `tail_len` no longer covers the trailing mint child when this hop minted into it, so the
    // body accounts for that child explicitly: its fresh head and element (`mint`, empty when
    // this hop contributed nothing) and the elements already there.
    const std::size_t new_fwd_body =
        pre.op_total + stack_writer_t<kFwdHead1Cap>::header_bytes(new_dst_body) + new_dst_body +
        r.sel_total + stack_writer_t<kFwdSrcHdrCap>::header_bytes(new_src_body) + new_src_body +
        r.tail_len + r.mint.span().size() + r.ref_body_len;

    // The inbound frame's trailer timestamp, preserved VERBATIM across the hop (#1109):
    // the fresh head keeps the TS/TF bits and the gather re-emits the stamp's source
    // window as the outgoing frame's last bytes — else an origin's stamp is silently
    // dropped at the first forwarder, which is exactly the gap #1109 names. Either form
    // relays (a hop does not interpret the value; the anchorless-TF=1 MUST-reject binds
    // where the stamp is CONSUMED). An inbound CRC is dropped, not preserved: the body
    // this hop emits differs from the one the CRC covered (see stack_writer_t::header).
    //
    // The window is ONE word — offset plus the producer's form bit (`ts_window`, 4 bytes in
    // an alignment hole, which is what keeps this struct at 256; #1235). A body ending past
    // `kTsNarrow` cannot express its offset there, so the stamp and its header bits are
    // dropped TOGETHER: a head that declares a trailer the gather cannot emit would be a
    // frame its own receiver rejects, which is strictly worse than relaying it unstamped.
    const bool keep_ts = pre.fwd_opt.ts && pre.body_end < fwd_rebuild_t::kTsNarrow;
    if (keep_ts) {
        r.ts_window = static_cast<std::uint32_t>(pre.body_end) |
                      (pre.fwd_opt.tf ? fwd_rebuild_t::kTsNarrow : 0u);
    }

    // head1: FWD header + op (copied) + new (shrunk) dst header. head2: new (grown)
    // src header. Both fixed stack buffers — ZERO heap on the forward hop (ADR-0038 inv. #2).
    // An overflow (a malformed op TLV larger than the buffer) yields an empty span ⇒ the
    // caller drops, never a buffer overrun.
    r.head1.header(wire::type_t::FWD, new_fwd_body,
                   keep_ts ? pre.fwd_opt : pre.fwd_opt.without_trailer());
    cur.for_each_span(pre.op_pos, pre.op_total,
                      [&](std::span<const std::byte> s) { r.head1.raw(s); });
    // The shrunk `dst` is headed with the type the peek carried: a bound `dst` stays a
    // `PATH_REF` with `opt = 0` (the shrink is an element and the body is still a fixed-stride
    // record array, so a set `PL` would mis-frame it at the next hop, RFC-0024 §4.2), a
    // canonical one stays a `PATH`, and the reverse-list delivery's last hop arrives here
    // already re-typed to `PATH` (see `fwd_pre_t::dst_type`).
    r.head1.header_route(pre.dst_type, new_dst_body);
    // `src` keeps its own type: a `PATH`, or the `PATH_REF` a reply to a bound request echoes.
    r.head2.header_route(src_h->type, new_src_body);
    return r;
}

}  // namespace tr::net
