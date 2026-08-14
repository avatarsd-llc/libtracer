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

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>

#include "libtracer/byteorder.hpp"
#include "libtracer/crc.hpp"
#include "libtracer/error.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/path_ref.hpp"
#include "libtracer/tlv.hpp"

/**
 * @file
 * @brief The shared L2/L3 (`tr::wire`) TLV header/trailer grammar (ADR-0048 §1).
 */

// A dedicated `grammar` sub-namespace (not `detail`) so wire-layer code keeps
// resolving `detail::` to the layer-free `tr::detail` byte helpers.
namespace tr::wire::grammar {

// A wire `length` is at most 0xFFFFFFFF (the `LL` width), so every `length ->
// std::size_t` narrowing in this file is value-preserving on any target with a
// 32-bit-or-wider `std::size_t`. The one narrowing that runs BEFORE the total-size
// bound (the PATH_REF shape check) rests on exactly this; the rest run after it.
static_assert(sizeof(std::size_t) >= 4, "a wire length must narrow to size_t without truncating");

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
     * @brief Never latched — a contiguous cursor cannot serve a byte from outside its
     *        window, so it has nothing to report (#986).
     *
     * The rope source's latch exists because its window is a SOFT bound: the link chain
     * physically continues past it, so an overshooting feed reads real bytes belonging
     * to another part of the rope and nothing faults. A `span_cursor`'s window is its
     * whole object — @ref for_each_span clamps to it, and past that there are no bytes
     * to serve, wrong or otherwise.
     *
     * `static constexpr`, not a member: this makes `if (cur.poisoned())` in the shared
     * grammar fold away entirely on the span source, and — the reason it is written this
     * way — keeps the cursor exactly one `std::span` wide. Carrying a `bool` here cost
     * **compact-forward −26% deliv/s and compact-terminus −14%** (measured, 4/4 and 3/4
     * interleaved pairs, disjoint ranges): at 24 bytes the cursor stops being two
     * registers, and `walk`/`region` pass one per descent.
     */
    [[nodiscard]] static constexpr bool poisoned() noexcept { return false; }
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
    /** @brief Load @p n little-endian bytes at @p off as a u64 (`%byteorder.hpp`). */
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
     * @note Precondition: `off + n <= size()`, enforced in every build (#986) —
     *       the feed is truncated to the window and @ref poisoned latches.
     */
    template <class Fn>
    void for_each_span(std::size_t off, std::size_t n, Fn&& fn) const {
        fn(buf.subspan(off, n));
    }
};

/**
 * @brief The same bound where a sum CAN wrap — 32-bit `Size` (rv32, the primary MCU target).
 *
 * Kept as its own function so the wide path above is a single compare with nothing to skip
 * over, and so `grammar_bounds_test`'s `Size = std::uint32_t` instantiation still exercises
 * this arithmetic verbatim from a 64-bit host.
 *
 * The bound is taken by subtracting FROM @p avail rather than by summing toward it, so no
 * intermediate can wrap and the whole check stays single-word. Each step is bounded by the
 * one before:
 *
 * 1. `avail >= header` is established here, not assumed, so `avail - header` — the room a
 *    body plus trailer may occupy — cannot wrap;
 * 2. `length <= room` is then checked, so `room - length` cannot wrap either;
 * 3. `ts_size + crc_size` is at most 12 and is checked against that remainder.
 *
 * Only once all three hold is `total` formed, and every term of it is by then known to be at
 * or under @p avail — which is why the result cannot wrap.
 *
 * Summing into a `std::uint64_t` and comparing there is equally correct and was the first
 * shape tried; it lost on measurement. On rv32 (`-O2`, esp-15.2.0) the u64 sum kept the length
 * decode's high word live, cost +27 instructions and forced a stack frame into a previously
 * leaf, frame-free header parse — ~40% on the lazy tier's O(header) sibling skip, which is the
 * traffic `DEFER` exists to keep cheap. This form costs +1 instruction on a trailer-less
 * header and +3 with a trailer.
 *
 * @param total Receives the total encoded size; untouched when the bound fails.
 * @return False ⇔ the declared TLV does not fit — the caller's FRAME_TRUNCATED.
 */
template <class Size>
[[nodiscard]] constexpr bool total_size_fits_narrow(Size avail, Size header, std::uint32_t length,
                                                    Size ts_size, Size crc_size,
                                                    Size& total) noexcept {
    if (avail < header) return false;
    const Size room = static_cast<Size>(avail - header);
    if (length > room) return false;
    const Size rest = static_cast<Size>(room - static_cast<Size>(length));
    if (static_cast<Size>(ts_size + crc_size) > rest) return false;
    total = static_cast<Size>(header + static_cast<Size>(length) + ts_size + crc_size);
    return true;
}

/**
 * @brief Does the declared TLV's total encoded size fit in @p avail? — the bound
 *        taken without ever forming a value that can wrap (#921).
 *
 * Split out of @ref parse_header, and templated on the *size type* rather than
 * hard-wired to `std::size_t`, because the defect it closes is invisible at the
 * host's width: a wire-declared `length` is up to `0xFFFFFFFF` (the `LL` width),
 * so `header + length + ts_size + crc_size` overflows a **32-bit** `std::size_t`
 * — the primary embedded target's (ESP32-C6 / rv32). Narrowing that sum before
 * the compare made a hostile `length = 0xFFFFFFFF` frame present a `total` of 17,
 * pass `avail < total`, and hand `walk` a payload span far past the buffer.
 * Templating the width is what makes that reachable — and testable — from a
 * 64-bit host: a `Size = std::uint32_t` instantiation runs the identical
 * arithmetic the rv32 build compiles (`grammar_bounds_test`).
 *
 * TWO SHAPES, PICKED AT COMPILE TIME, because the wrap this guards against is only
 * reachable at one width. `header` is at most 6, `length` at most `0xFFFFFFFF` and the
 * trailer at most 12, so the total is under `2^32 + 18`: on any `Size` STRICTLY WIDER
 * than the 32-bit wire length the sum provably cannot wrap and the bound is the single
 * compare it always was. Only a 32-bit `Size` needs the wrap-free subtractive chain, and
 * that lives in @ref total_size_fits_narrow.
 *
 * Running the narrow chain everywhere is what regressed `compact-forward` by **+44%**
 * (#1173 -> #1177): `parse_header` runs once per TLV, so a few cycles per call multiply
 * across a frame. The measurement that licensed the original form ("+1 instruction") was
 * taken on rv32 at `-O2`; it did not hold on x86-64 at `-O3`, and the fix is to stop
 * paying a 32-bit-only cost on a 64-bit target rather than to weaken the bound.
 *
 * @param total Receives the total encoded size; untouched when the bound fails.
 * @return False ⇔ the declared TLV does not fit — the caller's FRAME_TRUNCATED.
 */
template <class Size>
[[nodiscard]] constexpr bool total_size_fits(Size avail, Size header, std::uint32_t length,
                                             Size ts_size, Size crc_size, Size& total) noexcept {
    static_assert(!std::numeric_limits<Size>::is_signed,
                  "the bound relies on unsigned subtraction");
    static_assert(sizeof(Size) >= sizeof(std::uint32_t), "a wire length must fit the size type");
    // WIDER THAN THE WIRE: one compare, because no sum here can wrap. `header` is at most 6,
    // `length` at most 0xFFFFFFFF and the trailer at most 12, so the total is under 2^32 + 18
    // — representable in any `Size` strictly wider than the 32-bit wire length. The subtractive
    // chain below exists solely for the case that bound is NOT free, and running it where it is
    // provably unnecessary is what cost +44% on `compact-forward` (#1173/#1177): `parse_header`
    // runs per TLV, so a few cycles per call multiply across a frame.
    if constexpr (sizeof(Size) > sizeof(std::uint32_t)) {
        const Size t = static_cast<Size>(header + static_cast<Size>(length) + ts_size + crc_size);
        if (avail < t) return false;
        total = t;
        return true;
    } else {
        return total_size_fits_narrow(avail, header, length, ts_size, crc_size, total);
    }
}

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
 * reject, `LL` length width, the `PATH_REF` fixed-stride body shape (RFC-0024
 * §4.2/§4.3), trailer sizing, and the two-span CRC (payload ++
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

    // u32, not u64: the `LL` width caps a wire length at 4 bytes, so this narrow is exact
    // — and saying so is what lets the total-size bound below stay single-word (#921).
    const std::uint32_t length = static_cast<std::uint32_t>(cur.load_le(2, opt.ll ? 4u : 2u));
    // The one per-type structural rule in the grammar (RFC-0024 §4.2/§4.3): a PATH_REF body is
    // a fixed-stride 8-byte record array, so its shape is not derivable from the header alone
    // the way every other type's is. PL/LL forbidden, length a whole number of elements, count
    // at or under the §4.3 bound. Shape only — what an element MEANS is L4's (path_ref.hpp).
    // BOTH bound-path codes carry that body (§7.1 amendment 2 gave the reverse list its own
    // type), and `is_path_ref_type` is why this stayed one compare: 0x15 is adjacent to 0x14,
    // so the pair test is a mask, not a disjunction, on a path every TLV of every frame runs.
    if (is_path_ref_type(static_cast<type_t>(type_b)) &&
        !path_ref_body_valid(opt.pl, opt.ll, static_cast<std::size_t>(length))) {
        return std::unexpected(err_t::FRAME_INVALID);
    }
    const std::size_t ts_size = opt.ts ? (opt.tf ? 4u : 8u) : 0u;
    const std::size_t crc_size = opt.cr ? (opt.cw ? 2u : 4u) : 0u;
    // The total-size bound, taken without ever forming a value that can wrap (#921):
    // a 32-bit `std::size_t` cannot hold `header + 0xFFFFFFFF + trailer`, and the sum
    // this replaces was narrowed BEFORE the compare — small enough to pass this very
    // check, after which `walk` read a payload span far past the buffer.
    std::size_t total = 0;
    if (!total_size_fits(avail, header, length, ts_size, crc_size, total)) {
        return std::unexpected(err_t::FRAME_TRUNCATED);
    }

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
        // The decode boundary for #986's containment latch. `total_size_fits` above
        // proves both feeds lie inside the window, so reaching this is a bound this
        // grammar failed to take — not hostile input — and the honest answer is the
        // one a short frame gets. Tested here rather than at the feeds so the CRC
        // arms stay branch-free; a truncated feed cannot pass its CRC anyway, which
        // is why this is the guarantee's backstop and not its only rung.
        if (cur.poisoned()) [[unlikely]]
            return std::unexpected(err_t::FRAME_TRUNCATED);
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
 * @brief One open structured node's traversal state in `walk`: a cursor over
 *        its children region and the walk position within it.
 *
 * @tparam Cursor A byte-source cursor (@ref span_cursor, or the rope cursor).
 */
template <class Cursor>
struct walk_frame_t {
    Cursor body{};       /**< @brief Cursor over the open node's children region. */
    std::size_t pos = 0; /**< @brief Walk position within @ref body. */
};

/**
 * @brief The walk's open-node stack — the RFC-0006 receiver-resource depth bound.
 *
 * Nesting depth is bounded by the receiver's decode resources, never by a
 * constant. The stack starts in the caller-provided @p inline_slots span — a
 * TUNING knob, not a limit: overflowing it changes cost, not behavior — and,
 * once those are exhausted, relocates into geometrically grown blocks drawn
 * from @p spill (one open-node record per open level, RFC-0006).
 *
 * A null @p spill makes the inline span the receiver's whole decode budget:
 * exhaustion makes @ref push return false, which `walk` maps to
 * `TLV_NESTING_TOO_DEEP` ("exceeds this receiver's decode resources") — a clean
 * reject, no throw, safe under `-fno-exceptions`. A non-null @p spill is drawn
 * from NOTHROW (#588): exhaustion there returns `nullptr` and takes the SAME
 * reject path, so the depth bound is honest whether or not a spill exists.
 *
 * @note @p spill is a @ref tr::mem::block_source_t and deliberately not a
 *       `std::pmr::memory_resource`. It used to be the latter, and `grow` used
 *       its throwing `allocate` unguarded — so a peer sending a frame nested
 *       past the inline slots could reach `__cxa_throw`'s `abort()` stub on a
 *       `-fno-exceptions` node, from the RX decode path, behind no ACL (#588).
 *       The reject this replaces it with was already specified; only the
 *       allocation was dishonest.
 */
template <class Cursor>
class walk_stack_t {
   public:
    /** @brief A stack over @p inline_slots, spilling to @p spill when they run out. */
    walk_stack_t(std::span<walk_frame_t<Cursor>> inline_slots, mem::block_source_t* spill) noexcept
        : data_(inline_slots.data()), cap_(inline_slots.size()), spill_(spill) {}

    /** @brief Releases the spill block, if any (the inline slots are the caller's). */
    ~walk_stack_t() {
        if (spilled_) spill_->release(data_, cap_ * sizeof(walk_frame_t<Cursor>), kAlign);
    }

    /** @brief Non-copyable (one walk, one stack — the spill block has one owner). */
    walk_stack_t(const walk_stack_t&) = delete;
    /** @brief Non-assignable. */
    walk_stack_t& operator=(const walk_stack_t&) = delete;

    /**
     * @brief Open one node. False ⇔ the receiver's decode resources are exhausted —
     *        the inline slots are full and the spill is absent OR itself exhausted.
     *        Never throws (#588).
     */
    [[nodiscard]] bool push(const walk_frame_t<Cursor>& f) {
        if (size_ == cap_ && !grow()) return false;
        data_[size_++] = f;
        return true;
    }
    /** @brief True when no node is open. */
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    /** @brief The innermost open node. */
    [[nodiscard]] walk_frame_t<Cursor>& back() noexcept { return data_[size_ - 1]; }
    /** @brief Close the innermost open node. */
    void pop() noexcept { --size_; }

   private:
    static constexpr std::size_t kAlign = alignof(walk_frame_t<Cursor>);

    // Relocate into a spill block ~2x the current capacity. walk_frame_t is
    // trivially copyable (both cursors are span/offset records), so relocation
    // is a memcpy and the vacated inline slots / old block need no destruction.
    [[nodiscard]] bool grow() {
        static_assert(std::is_trivially_copyable_v<walk_frame_t<Cursor>>);
        if (spill_ == nullptr) return false;
        const std::size_t new_cap = cap_ < 4 ? 8 : cap_ * 2;
        auto* fresh = static_cast<walk_frame_t<Cursor>*>(
            spill_->try_alloc(new_cap * sizeof(walk_frame_t<Cursor>), kAlign));
        if (fresh == nullptr) return false;  // exhausted ⇒ the same clean reject as no spill
        if (size_ > 0) std::memcpy(fresh, data_, size_ * sizeof(walk_frame_t<Cursor>));
        if (spilled_) spill_->release(data_, cap_ * sizeof(walk_frame_t<Cursor>), kAlign);
        data_ = fresh;
        cap_ = new_cap;
        spilled_ = true;
        return true;
    }

    walk_frame_t<Cursor>* data_;
    std::size_t cap_;
    std::size_t size_ = 0;
    mem::block_source_t* spill_;
    bool spilled_ = false;
};

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
 * its open-node state in @p stack, whose inline-slots + spill shape IS the
 * receiver's nesting-depth bound (RFC-0006 — no depth constant exists). A frame
 * that exhausts the stack is rejected with `TLV_NESTING_TOO_DEEP`, amended to
 * mean "exceeds this receiver's decode resources".
 *
 * The sink models each node through three hooks (balanced LIFO on success; a
 * rejected frame abandons the sink mid-walk):
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
 * @param stack     The open-node stack (must be fresh/empty) — the caller's
 *                  decode-resource bound (@ref walk_stack_t).
 * @param crc_policy CRC-trailer policy forwarded to @ref parse_header.
 * @return Nothing on success, or the first `err_t` the grammar rejects with
 *         (including `FRAME_INVALID` for trailing bytes after the root, and
 *         `TLV_NESTING_TOO_DEEP` on stack exhaustion).
 */
template <class Cursor, class Sink>
[[nodiscard]] std::expected<void, err_t> walk(const Cursor& root, Sink& sink,
                                              walk_stack_t<Cursor>& stack,
                                              crc_check_t crc_policy = crc_check_t::VERIFY) {
    const auto rh = parse_header(root, crc_policy);
    if (!rh) return std::unexpected(rh.error());
    if (rh->total != root.size()) return std::unexpected(err_t::FRAME_INVALID);  // trailing bytes
    if (!rh->opt.pl) {
        sink.on_leaf(*rh, root);  // opaque root: done
        return {};
    }
    sink.on_open(*rh, root);
    if (!stack.push({root.region(rh->header, rh->length), 0})) {
        return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);  // exceeds decode resources
    }

    while (!stack.empty()) {
        walk_frame_t<Cursor>& top = stack.back();
        if (top.pos == top.body.size()) {
            sink.on_close();  // node complete — seal / graft
            stack.pop();
            continue;
        }
        const auto ch =
            parse_header(top.body.region(top.pos, top.body.size() - top.pos), crc_policy);
        if (!ch) return std::unexpected(ch.error());
        const Cursor node = top.body.region(top.pos, ch->total);  // the child's own bytes
        top.pos += ch->total;
        if (ch->opt.pl) {
            sink.on_open(*ch, node);
            if (!stack.push({node.region(ch->header, ch->length), 0})) {
                return std::unexpected(err_t::TLV_NESTING_TOO_DEEP);  // exceeds decode resources
            }
        } else {
            sink.on_leaf(*ch, node);
        }
    }
    return {};
}

}  // namespace tr::wire::grammar
