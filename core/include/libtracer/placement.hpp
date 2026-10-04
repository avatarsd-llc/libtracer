/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The placement module (ADR-0083 Decision 5, #1775): the one owner of how a segment's header
 * and payload are laid out in the blocks a backend draws. Every backend, the inline value and
 * the receive loan ask it; none keeps its own header size, padding recipe or stride.
 */
#pragma once

#include <cstddef>
#include <new>
#include <span>

#include "libtracer/segment.hpp"

/**
 * @file
 * @brief The L0 (`tr::mem`) placement module: segment header, padding and the one-block-or-split
 *        rule, the inline-value and receive-loan layouts.
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
 * @brief The header block of a SPLIT segment: a bare @ref view::segment_t at its own size.
 *
 * Nothing follows the header in its block, so padding it to the payload's alignment would buy
 * nothing and could only push a header-sized draw into a larger size class.
 */
inline constexpr std::size_t kSplitHeaderBytes = sizeof(view::segment_t);

/** @brief The alignment @ref kSplitHeaderBytes is drawn at: the header's own. */
inline constexpr std::size_t kSplitHeaderAlign = alignof(view::segment_t);

/**
 * @brief Whether @p classes is a usable size-class table: non-empty and strictly ascending.
 *
 * A backend that reads `tr::graph::default_config_t::kSizeClasses` asserts this, so a fragment
 * that binds an unsorted table fails the build rather than splitting at the wrong row.
 */
[[nodiscard]] constexpr bool size_classes_valid(std::span<const std::size_t> classes) noexcept {
    if (classes.empty()) return false;
    for (std::size_t i = 1; i < classes.size(); ++i) {
        if (classes[i] <= classes[i - 1]) return false;
    }
    return true;
}

/**
 * @brief Whether a @p size-byte segment at @p align is ONE block (the header padded in front of
 *        the payload) rather than two, against the size-class table @p classes.
 *
 * The table lists the classes the host allocator serves on its fast path, ascending, so its last
 * row is that path's ceiling (`tr::graph::default_config_t::kSizeClasses`). A segment whose one
 * block fits the ceiling is one block: one draw instead of two, which made a 64 B value about
 * 30% cheaper (RFC-0028 slice 10). A larger one is split, the payload and a bare header: one
 * block that misses the fast path costs more than two that hit it, which is the 1 KiB cliff
 * #1768 measured (a 1024 B value's 1072 B block doubled `lkv-store-heap 1024B` on glibc).
 *
 * A function of the size alone, so a backend's `alloc` and `destroy` always agree on it.
 *
 * Only a backend that draws from a host allocator asks this (`heap_backend_t`). A backend over a
 * source or a pool the deployer sized keeps one block always: such a store is sized for one draw
 * per segment, and a split would spend two of its slots or classes on one segment.
 */
[[nodiscard]] constexpr bool is_one_block(std::size_t size, std::size_t align,
                                          std::span<const std::size_t> classes) noexcept {
    const std::size_t ceiling = classes.back();
    const std::size_t header = segment_header_bytes(align);
    return ceiling >= header && size <= ceiling - header;
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
 * @brief Place a SPLIT segment, reclaimed by @p owner: a bare header in @p header (drawn as
 *        @ref kSplitHeaderBytes at @ref kSplitHeaderAlign) over the @p size bytes at @p payload.
 */
[[nodiscard]] inline view::segment_t* place_split(mem_backend_t* owner, void* payload, void* header,
                                                  std::size_t size) noexcept {
    return new (header)
        view::segment_t(owner, std::span<std::byte>(static_cast<std::byte*>(payload), size));
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
