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

#include "libtracer/config.hpp"
#include "libtracer/grammar.hpp"
#include "libtracer/op_resolve.hpp"
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
 * re-slices from its own cursor. Filled by @ref peek_fwd_dst;
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
    /**
     * @brief The FIRST `dst` segment's `[body_off, body_len)` — the gate's own read, kept.
     *
     * @ref peek_fwd_dst must parse this header anyway: "the leading child is a NAME" is the
     * gate that decides a `dst` is an address at all. It used to throw the parsed offsets
     * away, and the descent immediately re-read the identical four bytes — one duplicated
     * `parse_header` on EVERY forward hop, which is a per-frame cost the pre-lift peek did
     * not pay (its one walk both gated and collected). Carrying the two integers forward
     * removes the duplicate without moving the gate: the same header, read once, decides the
     * same thing. Meaningless when @ref valid is false.
     */
    std::size_t seg0_off = 0;
    std::size_t seg0_len = 0; /**< @brief Length of the first `dst` segment's body. */
    /** @brief Where the surviving `dst` starts after this hop consumes its leading segments —
     *         i.e. the end of segment `strip_k - 1`, or @ref dst_body_off when nothing is
     *         stripped. Filled by the caller after the mount descent; leaving it 0 with
     *         `valid` set would silently forward an unshrunk dst, so the rebuild treats a
     *         `strip_at` below @ref dst_body_off as "not supplied" and walks the segments. */
    std::size_t strip_at = 0;
    /**
     * @brief The `dst` is a `PATH_REF` (`0x14`) — a BOUND address (RFC-0024 §4).
     *
     * Filled by @ref peek_fwd_dst_ref and never by @ref peek_fwd_dst, which gates on a
     * canonical `PATH` of NAMEs. It changes exactly two things in the rebuild: the shrunk
     * `dst` header is emitted as a `PATH_REF` (`opt.PL = 0` — the body is a fixed-stride
     * record array, not child TLVs), and the shrink is an element rather than a run of
     * segments. Everything else about a forward hop — the grown `src`, the selector, the
     * payload, the egress gather — is identical, because a bound path changes how the
     * address is SPELLED and nothing about what a hop does with the rest of the frame.
     */
    bool dst_ref = false;
    /**
     * @brief Re-head the shrunk BOUND `dst` as a canonical empty `PATH` instead of a
     *        `PATH_REF` — the reverse-list delivery's LAST hop (RFC-0024 §7.1 amendment 1).
     *
     * Set only by the router's session-delivery arm, where the consumed element was the
     * final one and the egress is the accepted session itself: the peer behind it is an
     * ORIGIN, which never speaks the bound form, so the frame it receives must be the
     * canonical delivery shape byte-for-byte (`dst` = an empty `PATH`, exactly what the
     * canonical mount descent leaves after stripping mount + peer). Meaningless unless
     * @ref dst_ref is also set.
     */
    bool dst_to_path = false;
    /**
     * @brief The outer FWD header's decoded `opt` bits — the peek's own read, kept (#1109).
     *
     * The rebuild needs them for exactly one thing: preserving the frame's trailer-timestamp
     * across the hop (`opt.TS`/`opt.TF` name the trailer window at `body_end` that the fresh
     * head must re-claim and the gather must re-emit — without them the origin's stamp is
     * silently dropped at the first forwarder). Meaningless when @ref valid is false.
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
};

/**
 * @brief Open the `dst` window of a FWD frame and say WHICH form it is — the routing gate.
 *
 * Fills @p pre with everything the descent, the bound hop and the head rebuild need about the
 * frame's structure: where the op VALUE and the `dst` body are, and where the body ends. It
 * reads NO segments and materializes nothing, so its cost and its stack are the same whatever
 * the `dst`'s depth or element count.
 *
 * The two arms diverge only at the `dst` header's type code:
 *   - `PATH` — the canonical address. The leading child must be a NAME (a `dst` whose first
 *     child is not a NAME is not an address), and @ref fwd_pre_t::strip_at starts at the body
 *     because only `strip_k` can say how much of it this hop consumes.
 *   - `PATH_REF` — the bound address. The four STRUCTURAL rules (`opt.PL = 0`, `opt.LL = 0`,
 *     `length % 8 == 0`, `length <= 2040`) are checked through `tr::wire::path_ref_body_valid`
 *     — the one locus that owns them — and @ref fwd_pre_t::strip_at is known HERE, past
 *     element 0: each hop consumes exactly one element (§4.1), with no descent to wait for.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 * @param  pre    Filled on every non-`NONE` answer; reset with `valid = false` otherwise, so a
 *                caller cannot pass stale offsets to the rebuild.
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
    const std::size_t dst_pos = fwd_h->body_off + op_h->total;
    if (dst_pos >= body_end) return fwd_dst_kind_t::NONE;
    const auto dst_h = read_fwd_header(cur, dst_pos);
    if (!dst_h) return fwd_dst_kind_t::NONE;
    const bool is_ref = dst_h->type == wire::type_t::PATH_REF;
    // The gate's own read of segment 0, handed over below rather than discarded — see the
    // `seg0_off` member doc. Nothing is filled until BOTH arms have accepted, so a rejected
    // frame leaves `pre` cleared exactly as it did when these were two functions.
    std::size_t seg0_off = 0;
    std::size_t seg0_len = 0;
    if (is_ref) {
        if (!wire::path_ref_body_valid(dst_h->opt.pl, dst_h->opt.ll, dst_h->body_len))
            return fwd_dst_kind_t::NONE;
    } else {
        if (dst_h->type != wire::type_t::PATH || dst_h->body_len == 0) return fwd_dst_kind_t::NONE;
        const auto seg_h = read_fwd_header(cur, dst_h->body_off);
        if (!seg_h || seg_h->type != wire::type_t::NAME) return fwd_dst_kind_t::NONE;
        seg0_off = seg_h->body_off;
        seg0_len = seg_h->body_len;
    }
    pre.valid = true;
    pre.fwd_opt = fwd_h->opt;
    pre.body_end = body_end;
    pre.op_pos = fwd_h->body_off;
    pre.op_total = op_h->total;
    pre.op_body_off = op_h->body_off;
    pre.op_body_len = op_h->body_len;
    pre.dst_body_off = dst_h->body_off;
    pre.dst_end = dst_h->body_off + dst_h->body_len;
    pre.after_dst = dst_pos + dst_h->total;
    if (!is_ref) {
        pre.seg0_off = seg0_off;
        pre.seg0_len = seg0_len;
        pre.strip_at = dst_h->body_off;  // caller overwrites once strip_k is known
        return fwd_dst_kind_t::PATH;
    }
    pre.dst_ref = true;
    // Element 0 is this hop's own, and consuming it is not conditional on anything the
    // descent decides — there is no descent. So the shrink is known here, unlike the
    // canonical arm's, which has to wait for `strip_k`.
    pre.strip_at = dst_h->body_off + wire::kPathRefElementBytes;
    if (pre.strip_at > pre.dst_end) pre.strip_at = pre.dst_end;  // the H = 0 body
    ref_count = wire::path_ref_element_count(dst_h->body_len);
    return fwd_dst_kind_t::PATH_REF;
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
 * Fills @p pre as @ref peek_fwd_dst_any does on its `PATH_REF` answer, plus
 * @ref fwd_pre_t::dst_ref, and sets
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
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 */
template <class Cursor>
class dst_seg_walk_t {
   public:
    /** @brief Walk the `dst` window @p pre describes, over @p cur. */
    dst_seg_walk_t(const Cursor& cur, const fwd_pre_t& pre) noexcept
        : cur_(&cur), body_off_(pre.dst_body_off), end_(pre.dst_end), pos_(pre.dst_body_off) {}

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
            const auto h = read_fwd_header(*cur_, pos_);
            if (!h || h->type != wire::type_t::NAME) return;
            last_ = {h->body_off, h->body_len};
            cache_[cached_++] = last_;
            pos_ += h->total;
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
            // Behind the walk but past the cache: resume from the last cached segment. A NAME
            // body is its TLV's tail, so the next header starts at `off + len` — no stored
            // "next position" and no second field per entry.
            have_ = cached_;
            pos_ =
                cached_ == 0 ? body_off_ : cache_[cached_ - 1].first + cache_[cached_ - 1].second;
        }
        while (have_ <= i) {
            if (pos_ >= end_) return std::nullopt;
            const auto h = read_fwd_header(*cur_, pos_);
            if (!h || h->type != wire::type_t::NAME) return std::nullopt;
            last_ = {h->body_off, h->body_len};
            if (have_ < kDstSegCacheSlots) cache_[cached_++] = last_;
            pos_ += h->total;
            ++have_;
        }
        return last_;
    }

   public:
    /**
     * @brief Where segment @p i ENDS — the `strip_at` the head rebuild wants.
     *
     * A NAME body is its TLV's tail, so the consumed run ends at `body_off + body_len` of the
     * last stripped segment. Returns `std::nullopt` if that segment does not exist.
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
class stack_writer {
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
        wire::opt_t opt{.pl = true, .ts = trailer.ts, .tf = trailer.ts && trailer.tf};
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
    /**
     * @brief Append a BARE TLV header (`opt = 0`) for @p body_len — a `PATH_REF`'s own shape.
     *
     * Separate from @ref header, which sets `opt.PL` because every header it writes frames
     * child TLVs. A `PATH_REF` body is a fixed-stride record array, so `PL = 1` would make a
     * generic walker read the first four body bytes as a TLV header and mis-frame the whole
     * body — the rule is a MUST (RFC-0024 §4.2), not a preference. `LL` is never set either:
     * the element bound caps the body at 2040 bytes, so a body needing a u32 length cannot be
     * reached, and a @p body_len that claims otherwise overflows rather than widening.
     */
    void header_bare(wire::type_t type, std::size_t body_len) {
        if (len_ + 4 > N || body_len > 0xFFFFu) {
            overflow_ = true;
            return;
        }
        buf_[len_++] = static_cast<std::byte>(std::to_underlying(type));
        buf_[len_++] = std::byte{0};
        buf_[len_++] = static_cast<std::byte>(body_len & 0xFF);
        buf_[len_++] = static_cast<std::byte>((body_len >> 8) & 0xFF);
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
    stack_writer<kFwdHead1Cap> head1;  /**< @brief FWD header + op (copied) + shrunk dst header. */
    stack_writer<kFwdSrcHdrCap> head2; /**< @brief The grown src PATH header. */
    /** @brief The inbound mount as ALREADY-ENCODED NAME TLVs, emitted as ONE span and never
     *         copied. Precomputed once per child (#508), so a hop does no per-segment work. */
    std::span<const std::byte> mount_tlv;
    /** @brief A 4-byte NAME header for @ref extra_seg. */
    std::array<std::byte, 4> extra_hdr;
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
     * rather than forwarded stale (see @ref stack_writer::header).
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
    stack_writer<4 + wire::kPathRefElementBytes> mint;
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
 * Writes @p r's tail and mint fields; @return the bytes this hop's element adds to the body,
 * or 0 when it contributes nothing — which INCLUDES the strip cases (no element to give, or a
 * list already at the cap). A reply with no mint answer at all leaves @p r untouched.
 */
template <class Cursor, class MintFn>
[[gnu::noinline]] std::size_t rebuild_reply_mint(const Cursor& cur, std::size_t pos,
                                                 std::size_t body_end, MintFn& mint_fn,
                                                 fwd_rebuild_t& r) {
    const std::optional<trailing_mint_t> found =
        peek_trailing_mint(cur, pos, body_end, wire::type_t::PATH_REF);
    if (!found) return 0;
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
    if (!mint) return 0;
    r.ref_body_off = found->pos + 4;  // LL = 0 is a MUST, so the header is 4 bytes
    r.ref_body_len = found->body_len;
    r.mint.header_bare(wire::type_t::PATH_REF, r.ref_body_len + wire::kPathRefElementBytes);
    std::array<std::byte, wire::kPathRefElementBytes> e{};
    wire::path_ref_store_element(e, *mint);
    r.mint.raw(e);
    return wire::kPathRefElementBytes;
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
 * Writes @p r's tail and mint fields; @return the bytes this hop's element adds to the body
 * (an extension adds the element; a creation is billed through the same @ref
 * fwd_rebuild_t::ref_body_len = 0 accounting), or 0 for strip/no-op.
 */
template <class Cursor, class MintFn>
[[gnu::noinline]] std::size_t rebuild_request_reverse_mint(const Cursor& cur, std::size_t pos,
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
        if (!mint) return 0;
        r.mint.header_bare(wire::type_t::PATH_REF_REVERSE, wire::kPathRefElementBytes);
        std::array<std::byte, wire::kPathRefElementBytes> e{};
        wire::path_ref_store_element(e, *mint);
        r.mint.raw(e);
        return wire::kPathRefElementBytes;
    }
    // A list exists: the tail stops short of it either way — extended one element longer, or
    // STRIPPED whole (RFC-0024 §7.1 amendment 1; erratum 1's rule direction-reversed).
    r.tail_len = found->pos > pos ? found->pos - pos : 0;
    if (!mint) return 0;
    r.ref_body_off = found->pos + 4;  // LL = 0 is a MUST, so the header is 4 bytes
    r.ref_body_len = found->body_len;
    r.mint.header_bare(wire::type_t::PATH_REF_REVERSE, r.ref_body_len + wire::kPathRefElementBytes);
    std::array<std::byte, wire::kPathRefElementBytes> e{};
    wire::path_ref_store_element(e, *mint);
    r.mint.raw(e);
    return wire::kPathRefElementBytes;
}

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
 * @param  pre           The offsets the routing peek already read, or nullptr to re-parse.
 * @param  mint_fn       This hop's mint contribution, supplied LAZILY: invoked at most once,
 *                       and only on a forwarded REPLY that actually carries an extendable
 *                       mint answer. Laziness is the whole point — deciding eagerly meant
 *                       reading the op byte a second time on EVERY forwarded frame,
 *                       including the request hops that can never mint, and that duplicate
 *                       read is a rope-cursor byte walk on a fragmented frame. Defaults to
 *                       @ref no_mint_t, the hop that contributes nothing.
 * @retval std::nullopt The frame is not a well-formed forwardable FWD (wrong
 *         type/shape, or fewer than @p strip_k dst segments) — the caller falls to its
 *         terminus path.
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
    const Cursor& cur, std::span<const std::byte> mount_tlv, std::string_view extra_seg,
    std::size_t strip_k, const fwd_pre_t* pre = nullptr, MintFn mint_fn = MintFn{},
    ReverseMintFn reverse_mint_fn = ReverseMintFn{}) {
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
    wire::opt_t outer_opt{};

    if (pre != nullptr && pre->valid) {
        if (pre->op_body_len == 0) return std::nullopt;
        body_end = pre->body_end;
        op_pos = pre->op_pos;
        op_total = pre->op_total;
        op_body_off = pre->op_body_off;
        dst_body_off = pre->dst_body_off;
        dst_end = pre->dst_end;
        pos = pre->after_dst;
        outer_opt = pre->fwd_opt;
    } else {
        const auto fwd_h = read_fwd_header(cur, 0);
        if (!fwd_h || fwd_h->type != wire::type_t::FWD) return std::nullopt;
        body_end = fwd_h->body_off + fwd_h->body_len;
        outer_opt = fwd_h->opt;

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
    // Masked (RFC-0024 §9.3) — the flag bits say nothing about which op this is. The raw
    // byte is read ONCE and split: opcode for the reply test, bit 7 for the reverse mint's
    // gate (§7.1 amendment 1 — the reverse child rides only a mint-flagged request).
    const std::uint8_t op_byte = cur.byte_at(op_body_off);
    const bool is_reply =
        static_cast<graph::fwd_op_t>(op_byte & graph::kFwdOpcodeMask) == graph::fwd_op_t::REPLY;
    const bool mint_flagged = (op_byte & graph::kFwdOpFlagMintRequest) != 0;

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
    const bool src_ref = src_h->type == wire::type_t::PATH_REF;
    if (src_h->type != wire::type_t::PATH && !(src_ref && is_reply)) return std::nullopt;
    pos += src_h->total;

    r.tail_off = pos;
    r.tail_len = body_end > pos ? body_end - pos : 0;
    r.src_body_off = src_h->body_off;
    r.src_body_len = src_h->body_len;

    // This hop's mint contribution (RFC-0024 §7.1 + amendment 1), in either direction. A
    // forwarded REPLY carrying a mint answer gets this hop's FORWARD element prepended; a
    // mint-flagged forwarded REQUEST gets this hop's REVERSE element — its arrival identity's
    // vertex ref — prepended to (or creating, or stripping) the trailing reverse `PATH_REF`
    // child. An UNFLAGGED request is never touched — the mint request still costs zero added
    // origin bytes, which is the whole point of putting the flag in the op byte (§7.5).
    // Both mint halves are CALLS, never inlined here (see `rebuild_reply_mint`).
    const std::size_t mint_growth =
        is_reply       ? rebuild_reply_mint(cur, pos, body_end, mint_fn, r)
        : mint_flagged ? rebuild_request_reverse_mint(cur, pos, body_end, reverse_mint_fn, r)
                       : 0u;

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
    // `tail_len` no longer covers the trailing `PATH_REF` when this hop minted into it, so the
    // body accounts for that child explicitly: its own 4-byte head, the elements already
    // there, and the 8 this hop adds.
    const std::size_t ref_total =
        mint_growth == 0 ? 0u : 4u + r.ref_body_len + wire::kPathRefElementBytes;
    const std::size_t new_fwd_body =
        op_total + new_dst_total + r.sel_total + new_src_total + r.tail_len + ref_total;

    // The inbound frame's trailer timestamp, preserved VERBATIM across the hop (#1109):
    // the fresh head keeps the TS/TF bits and the gather re-emits the stamp's source
    // window as the outgoing frame's last bytes — else an origin's stamp is silently
    // dropped at the first forwarder, which is exactly the gap #1109 names. Either form
    // relays (a hop does not interpret the value; the anchorless-TF=1 MUST-reject binds
    // where the stamp is CONSUMED). An inbound CRC is dropped, not preserved: the body
    // this hop emits differs from the one the CRC covered (see stack_writer::header).
    //
    // The window is ONE word — offset plus the producer's form bit (`ts_window`, 4 bytes in
    // an alignment hole, which is what keeps this struct at 256; #1235). A body ending past
    // `kTsNarrow` cannot express its offset there, so the stamp and its header bits are
    // dropped TOGETHER: a head that declares a trailer the gather cannot emit would be a
    // frame its own receiver rejects, which is strictly worse than relaying it unstamped.
    const bool keep_ts = outer_opt.ts && body_end < fwd_rebuild_t::kTsNarrow;
    if (keep_ts) {
        r.ts_window =
            static_cast<std::uint32_t>(body_end) | (outer_opt.tf ? fwd_rebuild_t::kTsNarrow : 0u);
    }

    // head1: FWD header + op (copied) + new (shrunk) dst header. head2: new (grown)
    // src header + the prepended inbound NAME. Both fixed stack buffers — ZERO heap
    // on the forward hop (ADR-0038 inv. #2). An overflow (a malformed op TLV larger
    // than the buffer) yields an empty span ⇒ the caller drops, never a buffer overrun.
    r.head1.header(wire::type_t::FWD, new_fwd_body,
                   keep_ts ? outer_opt : outer_opt.without_trailer());
    cur.for_each_span(op_pos, op_total, [&](std::span<const std::byte> s) { r.head1.raw(s); });
    // A bound `dst` re-heads as a `PATH_REF` with `opt = 0`: the shrink is an element, and the
    // body it now describes is still a fixed-stride record array, so `PL` stays clear
    // (RFC-0024 §4.2 — a set `PL` here would mis-frame the whole body at the next hop).
    if (pre != nullptr && pre->valid && pre->dst_ref && !pre->dst_to_path) {
        r.head1.header_bare(wire::type_t::PATH_REF, new_dst_body);
    } else {
        // Canonical PATH — the ordinary shrunk dst, AND the reverse-list delivery's last
        // hop (`dst_to_path`): the consumed element was the final one and the peer behind
        // the egress is an origin, so it receives the canonical delivery shape (§7.1
        // amendment 1 — the origin never speaks the bound form).
        r.head1.header(wire::type_t::PATH, new_dst_body);
    }

    if (src_ref) {
        r.head2.header_bare(wire::type_t::PATH_REF, new_src_body);
    } else {
        r.head2.header(wire::type_t::PATH, new_src_body);
    }
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
