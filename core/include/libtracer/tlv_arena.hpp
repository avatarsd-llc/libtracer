/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The terminus arena decoder (ADR-0041, implementing ADR-0038 inv. #5 /
 * ADR-0039 §3): decode_into parses a frame into a flat, pre-order array of
 * arena nodes drawn from an injected std::pmr::memory_resource. Every byte
 * span points into the caller's input buffer — the arena holds structure
 * only, never bytes. It is the resolve-scoped view the FWD terminus reads;
 * the owning tlv_t model (frame.hpp decode/encode) is unchanged alongside.
 *
 * The borrowed-span contract (ADR-0041 §2): a span into the inbound frame may
 * be read, copied once to its owner, or sub-viewed off a refcounted owner —
 * it may never be stored as a borrowed span.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>

#include "libtracer/frame.hpp"
#include "libtracer/tlv.hpp"

namespace tr::wire {

/**
 * @brief One decoded TLV node in a @ref tlv_arena_t (structure only, zero-copy).
 *
 * Both spans borrow the `decode_into` input buffer, which must outlive the
 * arena. `wire` deliberately excludes the trailer so a whole-TLV copy made
 * from it (a stored WRITE value, the reply route bytes) is trailer-less at
 * rest by construction (ADR-0041 §4) — a copier must also clear the trailer
 * bits from the copied `opt` byte to keep the copy self-consistent.
 */
struct arena_tlv_t {
    type_t type{}; /**< @brief The TLV type code. */
    opt_t opt{};   /**< @brief The decoded `opt` bits (trailer bits still set as on the wire). */

    /** @brief Header + body bytes, trailer excluded — the whole-TLV-copy span. */
    std::span<const std::byte> wire{};

    /** @brief The body: payload bytes (opaque) or the children region (`opt.pl`). */
    std::span<const std::byte> body{};

    /**
     * @brief One past the last descendant's index (pre-order subtree encoding).
     *
     * Children of node `i` start at `i + 1`; iterate siblings with
     * `j = i + 1; while (j < node[i].end) { visit(j); j = node[j].end; }`.
     * An opaque node's `end` is its own index + 1.
     */
    std::uint32_t end = 0;

    /**
     * @brief For a PATH node: every child header is exactly `02 00 <u16 len>`.
     *
     * True iff the body is byte-identical to the canonical `path_key` form, so
     * it can be used as the graph vertex-map key with zero materialization
     * (ADR-0041 §3). Always false for non-PATH nodes.
     */
    bool canonical_path = false;
};

/**
 * @brief A frame decoded as a flat pre-order node array over borrowed spans.
 *
 * Produced by `decode`_into; drawn from the injected memory resource, which
 * must outlive the arena (as must the input buffer). A resolve-scoped object:
 * read it, take the ADR-0041 ownership copies, and drop it — never store it.
 */
class tlv_arena_t {
   public:
    /** @brief An empty arena drawing its nodes from @p mr. */
    explicit tlv_arena_t(std::pmr::memory_resource& mr) : nodes_(&mr) {}

    /** @brief The root node (index 0). Precondition: a successful decode (never empty). */
    [[nodiscard]] const arena_tlv_t& root() const noexcept { return nodes_.front(); }

    /** @brief The node at pre-order index @p i. */
    [[nodiscard]] const arena_tlv_t& operator[](std::size_t i) const noexcept { return nodes_[i]; }

    /** @brief Total node count (root + all descendants). */
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }

    /** @brief Index of node @p i's first child (valid iff `first_child(i) < (*this)[i].end`). */
    [[nodiscard]] static std::uint32_t first_child(std::uint32_t i) noexcept { return i + 1; }

    /** @brief Index of node @p i's next sibling within its parent (compare against the parent's
     * `end`). */
    [[nodiscard]] std::uint32_t next_sibling(std::uint32_t i) const noexcept {
        return nodes_[i].end;
    }

   private:
    friend std::expected<tlv_arena_t, err_t> decode_into(std::span<const std::byte>,
                                                         std::pmr::memory_resource&);
    std::pmr::vector<arena_tlv_t> nodes_;
};

/**
 * @brief Decode exactly one TLV filling @p input into a flat arena drawn from @p mr.
 *
 * The terminus-side counterpart of `decode` (ADR-0041 §1): identical
 * validation (bounds, reserved bits, type 0x00, trailer CRC, trailing bytes ⇒
 * FRAME_INVALID), iterative (no recursion) with the walk stack drawn from
 * @p mr — so the caller's arena IS the nesting-depth bound (RFC-0006; no
 * depth constant exists) — but the result is
 * a pre-order @ref arena_tlv_t array of spans into @p input instead of an
 * owning `tlv_t` tree — zero heap when @p mr is a stack-buffer
 * `monotonic_buffer_resource`. @p input and @p mr must outlive the arena.
 */
[[nodiscard]] std::expected<tlv_arena_t, err_t> decode_into(std::span<const std::byte> input,
                                                            std::pmr::memory_resource& mr);

}  // namespace tr::wire
