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
 * Byte-at-a-time through the cursor, never through a span: on the rope tier a record may
 * straddle a link boundary and there is then no span to hand a span-taking parser. Three `byte_at`
 * calls at worst, no scratch and no stitch slot.
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
    NONE,    /**< @brief Not a structured FWD, or a `dst` in neither routable form. */
    PATH,    /**< @brief A canonical `PATH` of NAMEs — the mount descent's address. */
    RETIRED, /**< @brief A `PATH_REF` (`0x14`) `dst`: retired as an address, refused (§5.3). */
    /**
     * @brief A canonical `PATH` whose FIRST record is an escape — an element-headed address
     *        (RFC-0029's PAIR).
     *
     * Told apart from the plain `PATH` answer because the two take different routes and must not be
     * confused: a canonical `PATH` opens with a NAME the mount descent folds a digest over, and
     * this opens with a record that is not a name at all. Descending it would read a peer-supplied
     * slot index as UTF-8.
     */
    PATH_ESCAPE,
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
 *     leading record must be a LITERAL segment (a `dst` whose first record is an escape is
 *     not an address this node can descend), and @ref fwd_pre_t::strip_at starts at
 *     the body because only `strip_k` can say how much of it this hop consumes.
 *   - `RETIRED` — a `PATH_REF` (`0x14`) `dst`, told by its type code alone so the router can
 *     refuse it addressed (RFC-0029 §5.3). Nothing about its body is read.
 *
 * @tparam Cursor A grammar byte-source cursor (span or rope).
 * @param  cur    The cursor positioned at the frame's first byte.
 * @param  pre    Filled on every non-`NONE` answer, with `valid` set. On `NONE` it is reset
 *                with `valid = false`, and only its op fields may be filled (when the frame is
 *                a structured FWD with an op VALUE), which is what the terminus split reads.
 *
 * @note `flatten` — see @ref rebuild_fwd_forward for the measurement. The four
 *       @ref read_fwd_header calls below are this function's whole body, and each returns a
 *       ~56-byte `std::optional<fwd_hdr_t>` that an out-of-line call must return through
 *       memory.
 */
template <class Cursor>
[[gnu::flatten]] [[nodiscard]] fwd_dst_kind_t peek_fwd_dst_any(const Cursor& cur, fwd_pre_t& pre) {
    pre = fwd_pre_t{};
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
        pre.dst_body_off = dst_h->body_off;
        pre.dst_end = dst_end;
        pre.after_dst = dst_pos + dst_h->total;
        pre.strip_at = dst_h->body_off;
    };
    // RFC-0029 §5.3: the bare `PATH_REF` (`0x14`) array is retired as an address. It is told
    // by its type code alone and refused, addressed, by the router — never shape-checked,
    // never routed.
    if (dst_h->type == wire::type_t::PATH_REF) {
        accept();
        return fwd_dst_kind_t::RETIRED;
    }
    if (dst_h->type != wire::type_t::PATH || dst_h->opt.pl) return refuse();
    if (dst_h->body_len == 0) {
        accept();
        return fwd_dst_kind_t::EMPTY;
    }
    // Segment 0 is a packed record, and WHICH kind it is decides which arm routes the frame.
    // A literal record is the canonical address the mount descent walks; an ESCAPE heads an
    // element-spelled address, which has no name to descend and is answered by the PAIR arm
    // (or, for any other escape, by the terminus arm's refusal). Its `seg0_off`/`seg0_len` stay
    // zero for an escape: a descent that read them would read the escape's payload as a name.
    const auto seg0 = read_packed_seg(cur, dst_h->body_off, dst_end);
    if (!seg0) return refuse();
    accept();
    if (seg0->escape) return fwd_dst_kind_t::PATH_ESCAPE;
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
    if (peek_fwd_dst_any(cur, pre) == fwd_dst_kind_t::PATH) return true;
    pre = fwd_pre_t{};  // a bound `dst` fills it; this gate's contract is "cleared on false"
    return false;
}

/**
 * @brief Where a relayed subscribe request's reverse child sits (RFC-0029 §7.1).
 *
 * @see peek_trailing_reverse
 */
struct trailing_reverse_t {
    std::size_t pos = 0;      /**< @brief Offset of the `PATH_REF_REVERSE` child's own header. */
    std::size_t body_off = 0; /**< @brief Offset of its body: the elements already on the wire. */
    std::size_t body_len = 0; /**< @brief Length of that body. */
};

/**
 * @brief The element supplier of a hop that has no PAIR to give — @ref rebuild_fwd_forward's
 *        default, and the shape every caller outside the router has. The hop then contributes
 *        its NAME run (RFC-0029 §6.2: never nothing).
 */
struct no_mint_t {
    /** @brief Nothing to give. */
    [[nodiscard]] std::optional<wire::path_ref_element_t> operator()() const noexcept {
        return std::nullopt;
    }
};

/**
 * @brief Find a subscribe request's `PATH_REF_REVERSE` (`0x15`) child among the children after
 *        `src`, told from the payload by its type and never by its position (RFC-0029 §7.1).
 *
 * The body is a `PATH` body of path elements (§16 Q1), so it has no shape of its own to check
 * here: a hop prepends to it and never reads it.
 *
 * @param cur   Cursor over the frame.
 * @param from  First byte after `src` — where the frame's trailing children begin.
 * @param end   End of the FWD body.
 * @retval std::nullopt No reverse child, or a malformed tail: the hop creates the child.
 */
template <class Cursor>
[[nodiscard]] std::optional<trailing_reverse_t> peek_trailing_reverse(const Cursor& cur,
                                                                      std::size_t from,
                                                                      std::size_t end) {
    std::optional<trailing_reverse_t> found;
    for (std::size_t pos = from; pos < end;) {
        const auto h = read_fwd_header(cur, pos);
        if (!h || h->total == 0 || pos + h->total > end) return std::nullopt;
        if (h->type == wire::type_t::PATH_REF_REVERSE)
            found =
                trailing_reverse_t{.pos = pos, .body_off = h->body_off, .body_len = h->body_len};
        pos += h->total;
    }
    return found;
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
            if (r->escape) {  // step over an escape record: it is not a name
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
    // MASKED at a forwarder: bits 7-6 are reserved, and a forwarder routes on the opcode
    // alone. Only the terminus refuses a set reserved bit (TYPE_MISMATCH, #2245), so the
    // refusal comes back once, from the hop that would have acted on the byte.
    return static_cast<graph::fwd_op_t>(cur.byte_at(op_h->body_off) & graph::kFwdOpcodeMask);
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
     * `LL` widens for a `PATH`, whose body may legally pass 0xFFFF. It never widens for any
     * other route type: a @p body_len that claims otherwise overflows rather than widening.
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
 * **A REPLY's element (RFC-0029 §6.2) does not move it either**: a reply grows `src` by this
 * hop's PAIR (one region) or by the NAME run (regions 5-7), never both, so a reply emits at
 * most the same 10. A relayed SUBSCRIBE request's reverse child (§7.1) adds a head, a second
 * NAME run and the old body — up to 15 — and the router gathers that cold frame into its
 * growable table instead (`route_fwd_forward`), so this constant, and the code placement of
 * the hot arm that the constant sizes, stay as measured. It was once raised to 11 and cost
 * `bench_forward_rope` a disjoint **+13% at fan 2** in branch mispredicts on a shape that
 * emits none of the regions it was raised for. (The +1 in 10 is a region a stamped frame
 * really emits — the #1109 trailer-TS window, region 10 above.)
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
    /** @brief A REPLY's `src` grows by @ref own, not by the NAME run. These three flags fill
     *         the alignment hole before @ref ts_window, keeping the struct at 256 (#1235). */
    bool src_pair = false;
    bool rev = false;      /**< @brief A reverse child follows the tail. */
    bool rev_pair = false; /**< @brief The reverse element is @ref own's PAIR record. */
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
     * @brief This hop's own element when it is a PAIR (RFC-0029 §6.2, §7.1): the 11-byte
     *        escape record — behind the reverse child's fresh header on a subscribe request.
     *
     * On a forwarded REPLY it holds the bare record, and @ref src_pair says `src` grows by it
     * instead of by the NAME run. On a relayed subscribe request it holds the
     * `PATH_REF_REVERSE` header, then the record when @ref rev_pair is set; without the
     * record the element is the NAME run (@ref push_run), emitted a second time.
     */
    stack_writer_t<4 + wire::kPathPairRecordBytes> own;
    std::size_t rev_body_off = 0; /**< @brief The reverse child's body already on the wire. */
    std::size_t rev_body_len = 0; /**< @brief Its length (the origin's `src` on a create). */
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

    /** @brief Bytes of this hop's NAME run: the mount run plus the bus peer's record. */
    [[nodiscard]] std::size_t run_bytes() const {
        return mount_tlv.size() + (extra_seg.empty() ? 0u : 1u + extra_seg.size());
    }

    /** @brief Emit this hop's NAME run (the mount run, then the peer record) via @p push. */
    template <class Push>
    void push_run(Push& push) const {
        if (!mount_tlv.empty()) push(mount_tlv);
        if (!extra_seg.empty()) {
            push(std::span<const std::byte>(extra_hdr));
            push(std::span<const std::byte>(reinterpret_cast<const std::byte*>(extra_seg.data()),
                                            extra_seg.size()));
        }
    }

    /**
     * @brief Emit the reverse child's regions (RFC-0029 §7.1): its fresh head and this hop's
     *        element, then the body already on the wire.
     *
     * Out of line because only a relayed subscribe request reaches it: inlined, its second NAME
     * run and body walk were +493 B of `route_fwd_forward` that no data frame executes.
     */
    template <class Cursor, class Push>
    [[gnu::noinline]] void gather_reverse(const Cursor& cur, Push& push) const {
        push(own.span());
        if (!rev_pair) push_run(push);
        if (rev_body_len > 0) cur.for_each_span(rev_body_off, rev_body_len, push);
    }

    /** @brief True ⇔ every head fits its stack buffer (else the caller drops). */
    [[nodiscard]] bool ok() const { return head1.ok() && head2.ok() && own.ok(); }

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
        // This hop's own part of the route, prepended: its PAIR on a reply that has one, else
        // its NAME run — ONE span for the precomputed mount run, plus at most a
        // header-and-bytes pair for a dynamically-named peer. Nothing is copied.
        if (src_pair) {
            push(own.span());
        } else {
            push_run(push);
        }
        if (src_body_len > 0) cur.for_each_span(src_body_off, src_body_len, push);
        if (tail_len > 0) cur.for_each_span(tail_off, tail_len, push);
        // A subscribe request's reverse child, re-headed one element longer: `tail` stops
        // short of the old child, so it is never forwarded twice (RFC-0029 §7.1).
        if (rev) gather_reverse(cur, push);
        // The preserved trailer timestamp goes LAST — after the whole body, reverse child
        // included —
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
 * @brief This hop's element on a forwarded REPLY (RFC-0029 §6.2): its PAIR for the connection
 *        vertex the reply arrived over, when it can issue one. Kept OUT OF LINE.
 *
 * `noinline` against @ref rebuild_fwd_forward's `flatten`, and it is a measurement, not a
 * preference: `flatten` pulls everything a function calls into it, and the supplier's lookup
 * dragged into the rebuild's body cost `bench_forward_rope` **+13% at fan 2** in branch
 * mispredicts on a request hop that never takes this branch. Out of line, the request hop
 * sees one not-taken branch.
 *
 * Without a PAIR the reply's `src` grows by the NAME run instead, so the origin never receives
 * a route that skips a hop.
 */
template <class MintFn>
[[gnu::noinline]] void rebuild_reply_own(MintFn& mint_fn, fwd_rebuild_t& r) {
    const std::optional<wire::path_ref_element_t> own = mint_fn();
    if (!own) return;
    std::array<std::byte, wire::kPathPairRecordBytes> rec{};
    wire::path_pair_store(rec, *own);
    r.own.raw(rec);
    r.src_pair = true;
}

/**
 * @brief This hop's reverse-direction element on a relayed subscribe request (RFC-0029 §7.1),
 *        prepended to the request's `PATH_REF_REVERSE` child — created when the request
 *        carries none. Out of line for the same measured reason as @ref rebuild_reply_own.
 *
 * The element is the PAIR for the identity the request arrived on, else the NAME run, so the
 * list never skips a hop and is never stripped. The child a hop CREATES starts with the
 * origin's own `src` as it arrived — the origin's part of the way back, which no hop could
 * otherwise spell — so a delivery down the chain still names the consumer at its last hop.
 * A list that would outgrow a 16-bit body is left off (the request goes on, canonical only).
 *
 * @param src_off,src_len The request's `src` body as it arrived (the create seed).
 */
template <class Cursor, class MintFn>
[[gnu::noinline]] void rebuild_request_reverse(const Cursor& cur, std::size_t pos,
                                               std::size_t body_end, std::size_t src_off,
                                               std::size_t src_len, MintFn& mint_fn,
                                               fwd_rebuild_t& r) {
    const std::optional<trailing_reverse_t> found = peek_trailing_reverse(cur, pos, body_end);
    r.rev_body_off = found ? found->body_off : src_off;
    r.rev_body_len = found ? found->body_len : src_len;
    // The tail stops short of an existing child either way: re-headed below, or dropped.
    if (found) r.tail_len = found->pos - pos;
    const std::optional<wire::path_ref_element_t> own = mint_fn();
    const std::size_t body = r.rev_body_len + (own ? wire::kPathPairRecordBytes : r.run_bytes());
    if (body > 0xFFFFu) return;
    r.own.header_route(wire::type_t::PATH_REF_REVERSE, body);
    if (own) {
        std::array<std::byte, wire::kPathPairRecordBytes> rec{};
        wire::path_pair_store(rec, *own);
        r.own.raw(rec);
    }
    r.rev = true;
    r.rev_pair = own.has_value();
}

/**
 * @brief True ⇔ the request at @p pos (the first child after `src`) is a subscribe: a `WRITE`
 *        whose payload is a `SUBSCRIBER` (RFC-0029 §7.1). One header read, taken only on a
 *        `WRITE` that carries a selector — a delivery carries none.
 */
template <class Cursor>
[[nodiscard]] bool is_subscribe_request(const Cursor& cur, std::size_t pos, std::size_t body_end) {
    if (pos >= body_end) return false;
    const auto h = read_fwd_header(cur, pos);
    return h && h->type == wire::type_t::SUBSCRIBER;
}

/**
 * @brief The forward hop's head rebuild, read entirely by OFFSET — no decoded
 *        tree (ADR-0038 inv. #1).
 *
 * Layout: `FWD{ op VALUE, dst PATH, FIELD? sel, src PATH, tail }` — consumes the leading
 * `dst` records the routing decision named (@ref fwd_pre_t::strip_at), grows src by
 * this hop's own part of the route — @p mount_tlv on a request, its PAIR or that run on a
 * reply (RFC-0029 §6.2, amending RFC-0004 §B) — and synthesizes the two fresh stack heads. The
 * caller scatter-gathers the result via
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
 *                       consumes (the mount descent's answer, or the head PAIR).
 * @param  mount_tlv     This node's mount path for the link the frame arrived on, ALREADY
 *                       ENCODED as a run of packed records (precomputed per child, #508).
 * @param  extra_seg     One further mount segment whose name is only known now — a bus PEER.
 *                       Empty when the mount is fully precomputed.
 * @param  mint_fn       This hop's PAIR on a forwarded REPLY, supplied LAZILY: invoked at most
 *                       once, and only on a reply. Laziness is the point — deciding eagerly
 *                       meant reading the op byte a second time on EVERY forwarded frame,
 *                       and that duplicate read is a rope-cursor byte walk on a fragmented
 *                       frame. Defaults to @ref no_mint_t (the NAME run is contributed).
 * @param  reverse_mint_fn The REVERSE-direction twin, invoked at most once and only on a
 *                       relayed subscribe request (RFC-0029 §7.1).
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
    ReverseMintFn reverse_mint_fn = ReverseMintFn{}) {
    // The peek's rejections are the peek's; these are the ones only a rebuild can make. An
    // EMPTY op is one of them on purpose (see `fwd_pre_t::op_body_len`: the peek accepts it,
    // so the check lives here or a malformed frame would change fate). A strip past the `dst`
    // is a `dst` shorter than the mount the descent matched. The packed record's length field
    // is a `u8` (RFC-0018 §5) — the wire's own bound on a segment and now the only one on a
    // peer name: past it the name has no spelling at all.
    if (!pre.valid || pre.op_body_len == 0 || pre.strip_at > pre.dst_end ||
        extra_seg.size() > wire::kPackedSegMaxBytes)
        return std::nullopt;
    // Masked (RFC-0024 §9.3): a forwarder stays opcode-agnostic, and bits 7-6 are refused
    // where the operation is applied, not here.
    const auto op =
        static_cast<graph::fwd_op_t>(cur.byte_at(pre.op_body_off) & graph::kFwdOpcodeMask);
    const bool is_reply = op == graph::fwd_op_t::REPLY;

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
    // `src` is a `PATH` in both directions: a reply's carries the forward route (RFC-0029 §6.2),
    // a request's the way back. Anything else cannot accumulate, so it is not forwarded.
    if (src_h->type != wire::type_t::PATH) return std::nullopt;
    pos += src_h->total;

    r.tail_off = pos;
    r.tail_len = pre.body_end > pos ? pre.body_end - pos : 0;
    r.src_body_off = src_h->body_off;
    r.src_body_len = src_h->body_len;

    // This hop's own part of the route, in either direction (RFC-0029 §6.2, §7.1): a relayed
    // REPLY's `src` grows by this hop's PAIR, else by its NAME run below; a relayed subscribe
    // request's reverse child gets this hop's reverse element. Both are calls, never inlined
    // here (see `rebuild_reply_own`), and no other frame reaches either.
    r.mount_tlv = mount_tlv;
    r.extra_seg = extra_seg;
    r.extra_hdr[0] = static_cast<std::byte>(extra_seg.size());
    if (is_reply) {
        rebuild_reply_own(mint_fn, r);
    } else if (op == graph::fwd_op_t::WRITE && r.sel_total > 0 &&
               is_subscribe_request(cur, pos, pre.body_end)) {
        rebuild_request_reverse(cur, pos, pre.body_end, r.src_body_off, r.src_body_len,
                                reverse_mint_fn, r);
    }

    // The leading `dst` records this hop consumes, as the routing decision recorded them. The
    // strip stays the zero-copy shrink of §5.1: the residual `dst` is emitted as ONE untouched
    // span under a fresh header, and no length table is rewritten.
    r.rem_dst_off = pre.strip_at;
    r.rem_dst_len = pre.dst_end - pre.strip_at;

    // What `src` grows by: this hop's PAIR on a reply that has one, else its NAME run — the
    // precomputed mount run, plus ONE length byte and the bytes of a dynamic bus-peer segment.
    // The only bound on a mount path is the packed record's `u8` length field; beyond that it
    // is limited solely by the frame fitting the link's `max_frame`/MTU, never by a buffer.
    const std::size_t grown = r.src_pair ? wire::kPathPairRecordBytes : r.run_bytes();
    const std::size_t new_dst_body = r.rem_dst_len;
    const std::size_t new_src_body = src_h->body_len + grown;
    // `tail_len` no longer covers a reverse child this hop re-heads, so the body accounts for
    // that child explicitly: its fresh head (with this hop's PAIR, when it has one), the NAME
    // run otherwise, and the elements already there.
    const std::size_t rev_bytes =
        r.rev ? r.own.span().size() + (r.rev_pair ? 0u : r.run_bytes()) + r.rev_body_len : 0u;
    const std::size_t new_fwd_body =
        pre.op_total + stack_writer_t<kFwdHead1Cap>::header_bytes(new_dst_body) + new_dst_body +
        r.sel_total + stack_writer_t<kFwdSrcHdrCap>::header_bytes(new_src_body) + new_src_body +
        r.tail_len + rev_bytes;

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
    // The shrunk `dst` is a `PATH` whatever was consumed — NAMEs or a PAIR (RFC-0029 §4.2).
    r.head1.header_route(wire::type_t::PATH, new_dst_body);
    r.head2.header_route(wire::type_t::PATH, new_src_body);
    return r;
}

}  // namespace tr::net
