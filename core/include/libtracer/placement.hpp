/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The placement module (ADR-0083 Decision 5, #1775): the one owner of how a segment's header
 * and payload are laid out in the blocks a backend draws. Every backend, the inline value and
 * the receive loan ask it; none keeps its own header size, padding recipe or stride.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <new>
#include <span>

#include "libtracer/segment.hpp"

/**
 * @file
 * @brief The L0 (`tr::mem`) placement module: segment header and padding, the one-block
 *        layout, the inline-value and receive-loan layouts.
 */

namespace tr::mem {

/**
 * @brief @p n rounded up to a multiple of @p align — THE padding rule, spelled once.
 *
 * @p align is an alignment, so a power of two, and the rule is a mask rather than a division:
 * a pool computes it from a runtime alignment, and a core without a hardware divider (Cortex-M0)
 * would otherwise pay a library call for it.
 */
[[nodiscard]] constexpr std::size_t pad_to(std::size_t n, std::size_t align) noexcept {
    return (n + align - 1) & ~(align - 1);
}

/**
 * @brief The alignment a segment block is drawn at for a backend that guarantees @p align: the
 *        stricter of @p align and the header's own (RFC-0028 §4.9).
 */
[[nodiscard]] constexpr std::size_t segment_block_align(std::size_t align) noexcept {
    return align < alignof(view::segment_t) ? alignof(view::segment_t) : align;
}

/**
 * @brief Bytes the @ref view::segment_t header occupies at the head of a one-block segment,
 *        padded so the payload after it starts at `segment_block_align(align)`.
 */
[[nodiscard]] constexpr std::size_t segment_header_bytes(std::size_t align) noexcept {
    return pad_to(sizeof(view::segment_t), segment_block_align(align));
}

/** @brief The whole block a one-block segment of @p size payload bytes draws at @p align. */
[[nodiscard]] constexpr std::size_t segment_block_bytes(std::size_t size,
                                                        std::size_t align) noexcept {
    return segment_header_bytes(align) + size;
}

/**
 * @brief The stride of a slot that holds one-block segments of up to @p payload bytes at
 *        @p align: the block, padded so the next slot's header is aligned too.
 */
[[nodiscard]] constexpr std::size_t segment_slot_bytes(std::size_t payload,
                                                       std::size_t align) noexcept {
    return pad_to(segment_block_bytes(payload, align), segment_block_align(align));
}

/**
 * @brief Whether @p classes is a usable size-class table: non-empty and strictly ascending.
 *
 * The host slab pool asserts it of `tr::graph::default_config_t::kSizeClasses`
 * (`tr::mem::slab_classes_valid`, with the pool's own rules on top), so a fragment that binds an
 * unsorted table fails the build rather than serving a request from the wrong class.
 */
[[nodiscard]] constexpr bool size_classes_valid(std::span<const std::size_t> classes) noexcept {
    if (classes.empty()) return false;
    for (std::size_t i = 1; i < classes.size(); ++i) {
        if (classes[i] <= classes[i - 1]) return false;
    }
    return true;
}

/**
 * @brief Place a @ref view::segment_t over the one block @p block, reclaimed by @p owner: the
 *        header at the head, @p size payload bytes after it (a null, empty span for 0).
 *
 * @p block was drawn as `segment_block_bytes(size, align)` at `segment_block_align(align)`.
 */
[[nodiscard]] inline view::segment_t* place_segment(mem_backend_t* owner, void* block,
                                                    std::size_t size, std::size_t align) noexcept {
    auto* const base = static_cast<std::byte*>(block);
    std::byte* const payload = size != 0 ? base + segment_header_bytes(align) : nullptr;
    return new (base) view::segment_t(owner, std::span<std::byte>(payload, size));
}

/**
 * @brief Where an INLINE value's embedded segment sits in its block: after the value's own
 *        @p prefix bytes (its header and one link), padded to the segment's alignment
 *        (RFC-0028 §5.1).
 */
[[nodiscard]] constexpr std::size_t inline_segment_offset(std::size_t prefix) noexcept {
    return pad_to(prefix, alignof(view::segment_t));
}

/**
 * @brief The alignment an inline value's bytes start at: they follow the embedded segment, whose
 *        size is a multiple of its alignment.
 */
inline constexpr std::size_t kInlinePayloadAlign = alignof(view::segment_t);

/**
 * @brief The block an INLINE value of @p len bytes occupies: the @p prefix, the embedded segment,
 *        and the bytes.
 */
[[nodiscard]] constexpr std::size_t inline_block_bytes(std::size_t prefix,
                                                       std::size_t len) noexcept {
    return inline_segment_offset(prefix) + sizeof(view::segment_t) + len;
}

/**
 * @brief Place an inline value's embedded segment in @p block (drawn as
 *        `inline_block_bytes(prefix, len)`), reclaimed by @p owner, over its @p len bytes.
 */
[[nodiscard]] inline view::segment_t* place_inline(mem_backend_t* owner, void* block,
                                                   std::size_t prefix, std::size_t len) noexcept {
    std::byte* const at = static_cast<std::byte*>(block) + inline_segment_offset(prefix);
    return new (at) view::segment_t(owner, std::span<std::byte>(at + sizeof(view::segment_t), len));
}

/** @brief The block an inline value's embedded segment @p seg was placed in by @ref place_inline.
 */
[[nodiscard]] inline std::byte* inline_block_of(view::segment_t* seg, std::size_t prefix) noexcept {
    return reinterpret_cast<std::byte*>(seg) - inline_segment_offset(prefix);
}

/**
 * @brief Bytes a loaned receive block reserves in front of its frame (RFC-0028 §6.9, #1626):
 *        the claim word, then room for a one-link `tr::graph::value_t` header.
 *
 * Sized for the value's header and its one link — 16 + 24 B on a 64-bit host, 12 + 12 B on
 * rv32 — after an aligned claim word. `%value.hpp` asserts the fit, so a change to either side
 * fails the build rather than overrunning a frame.
 */
inline constexpr std::size_t kRxLoanBytes = sizeof(void*) >= 8 ? 48 : 32;

/** @brief Offset of the value header inside the reserve: one pointer-aligned word past its
 *         start, which is where the claim word sits. */
inline constexpr std::size_t kRxLoanValueOffset = sizeof(void*);

/**
 * @brief Whether a backend whose segments hold at most @p cap bytes at @p align can carry the
 *        receive-loan reserve in front of a @p len-byte frame: room for both, aligned for the
 *        record's words.
 */
[[nodiscard]] constexpr bool rx_loan_fits(std::size_t len, std::size_t cap,
                                          std::size_t align) noexcept {
    return cap >= kRxLoanBytes && len <= cap - kRxLoanBytes && align >= alignof(void*);
}

namespace detail {

/** @brief The step of the class lookup: every row of a classed root's table is a multiple of it
 *         (`slab_classes_valid`, and the arena pool's own rule), so rounding a request up to it
 *         never skips a row. */
inline constexpr std::size_t kClassGrain = alignof(std::max_align_t);

/** @brief The table the lookup is built from, and its rows. */
inline constexpr const auto& kClassRows = graph::config_t::kSizeClasses;
inline constexpr std::size_t kClassRowCount = std::size(kClassRows);

/** @brief Lookup slots: one per @ref kClassGrain step up to the last row, then the one slot every
 *         larger request is clamped to. */
inline constexpr std::size_t kClassSlots = kClassRows[kClassRowCount - 1] / kClassGrain + 2;

static_assert(kClassRowCount < 255, "the class lookup stores a row index in one byte");
static_assert(kClassSlots <= 8 * 1024 + 2,
              "the class lookup is one byte per 16 B up to the last row: keep the last row at "
              "or below 128 KiB");

/** @brief Slot @c q → the index of the smallest row of at least `q * kClassGrain` bytes; the last
 *         slot → @ref kClassRowCount, "past the table". Built at compile time. */
inline constexpr std::array<std::uint8_t, kClassSlots> kClassSlot = [] {
    std::array<std::uint8_t, kClassSlots> t{};
    std::size_t r = 0;
    for (std::size_t q = 0; q + 1 < kClassSlots; ++q) {
        while (r < kClassRowCount && kClassRows[r] < q * kClassGrain) ++r;
        t[q] = static_cast<std::uint8_t>(r);
    }
    t[kClassSlots - 1] = static_cast<std::uint8_t>(kClassRowCount);
    return t;
}();

/** @brief The rows, then a 0 the "past the table" index reads (see @ref class_ceiling). */
inline constexpr std::array<std::size_t, kClassRowCount + 1> kClassRowOr0 = [] {
    std::array<std::size_t, kClassRowCount + 1> t{};
    for (std::size_t i = 0; i < kClassRowCount; ++i) t[i] = kClassRows[i];
    return t;
}();

}  // namespace detail

/**
 * @brief The block a classed root (the slab pool and the static arena both carve by
 *        `config_t::kSizeClasses`) draws for a @p bytes request: the smallest row that holds
 *        it, or @p bytes itself above the last row, where the block is drawn at its exact size.
 *
 * O(1) and branch-free: the request is clamped to the last lookup slot (a `min`), rounded up to
 * the 16 B grain, and two table loads, built at compile time, give the row. Above the table the
 * second load reads 0, and the `max` with @p bytes answers the exact size. No search.
 */
[[nodiscard]] constexpr std::size_t class_ceiling(std::size_t bytes) noexcept {
    using namespace detail;
    const std::size_t clamped = std::min(bytes, (kClassSlots - 1) * kClassGrain);
    const std::size_t slot = (clamped + kClassGrain - 1) / kClassGrain;
    return std::max(kClassRowOr0[kClassSlot[slot]], bytes);
}

/**
 * @brief Whether this build's composed block source shares a stored value by default
 *        (RFC-0022 Amendment 3): the copy-or-share trait, decided at compile time.
 *
 * - **Static arena and heap root: always.** The heap draws every block at its exact size, so a
 *   copy never draws a block smaller than the one the value already sits in (the heap-root
 *   default, resolved in #2143); the arena's fixed RAM is what both draws share.
 * - **Slab pool: on a class fit.** It shares when a copy, a @p copy_block -byte block, would
 *   draw a block no smaller than the @p held_block -byte block the value already sits in (a
 *   class no smaller inside the table; above the last row, where blocks are exact, no fewer
 *   bytes), and copies otherwise.
 *
 * On every source, `SIZE_MAX` for @p held_block means the value does not sit in one block (it
 * spans several links, or its bytes are borrowed): it copies, so a default value is always one
 * link. Branch-free: the slab build adds @ref class_ceiling's two table loads and a compare,
 * the other builds one compare. The operators are `&` and `|` so no operand is a branch.
 */
[[nodiscard]] constexpr bool share_fits(std::size_t held_block, std::size_t copy_block) noexcept {
    return (held_block != SIZE_MAX) & (!kSlabPool | (held_block <= class_ceiling(copy_block)));
}

/**
 * @brief Whether a receive block for a @p len-byte frame carries the receive-loan reserve
 *        (RFC-0028 §6.9): only where it costs nothing, that is, when the frame plus
 *        @ref kRxLoanBytes lands in the same size class as the frame alone (resolved in #2143).
 *
 * Never on the heap root, whose blocks are exact, and never above the last class row.
 */
[[nodiscard]] constexpr bool rx_loan_free(std::size_t len, std::size_t align) noexcept {
    if constexpr (kHeapRoot) {
        (void)len;
        (void)align;
        return false;
    } else {
        // Above the table each ceiling is its own exact size, so the two never match there.
        return class_ceiling(segment_block_bytes(len, align)) ==
               class_ceiling(segment_block_bytes(len + kRxLoanBytes, align));
    }
}

/** @brief The one-block layout (see the declaration in `%backend.hpp`). */
inline view::segment_t* mem_backend_t::alloc_in_block(std::size_t size,
                                                      std::size_t align) noexcept {
    void* const block = try_alloc(segment_block_bytes(size, align), segment_block_align(align));
    return block != nullptr ? place_segment(this, block, size, align) : nullptr;
}

/** @brief The mirror of @ref mem_backend_t::alloc_in_block. */
inline void mem_backend_t::destroy_in_block(view::segment_t* seg, std::size_t align) noexcept {
    const std::size_t size = seg->bytes.size();
    seg->~segment_t();
    release(seg, segment_block_bytes(size, align), segment_block_align(align));
}

/** @brief The default segment: one block through @ref mem_backend_t::try_alloc. */
inline view::segment_t* mem_backend_t::alloc(std::size_t size, alloc_hint_t /*hint*/) {
    return alloc_in_block(size, alignment());
}

/** @brief The default reclaim: one sized release of the block @ref mem_backend_t::alloc drew. */
inline void mem_backend_t::destroy(view::segment_t* seg) noexcept {
    destroy_in_block(seg, alignment());
}

}  // namespace tr::mem
