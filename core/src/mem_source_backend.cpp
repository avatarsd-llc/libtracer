/**
 * @file
 * @brief `tr::mem::source_backend_t`'s out-of-line allocation pair (#873 phase 1).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A TU of its own, for the reason the class's own `@note` records: this backend is tagged
 * `UNKNOWN`, so its reclaim is a virtual call regardless and inlining `alloc`/`destroy` buys
 * nothing — while leaving their bodies visible in a TU that HOLDS one re-partitions GCC's
 * inline budget in that TU. `graph.cpp` holds one (`graph_t`'s internal wrapper over the
 * injected source), and with the bodies in the header it inlined
 * `tr::view::segment_ptr_t::reset` into `graph_t::dispatch_edge_remote` and grew that pinned
 * symbol by 32 B. Bisected against `bench/symbol_ratchet.json`.
 */
#include "libtracer/mem_source_backend.hpp"

#include <cstddef>
#include <new>

#include "libtracer/placement.hpp"

namespace tr::mem {

/**
 * @brief One draw: the control block at the head of the block, the payload behind it (#873
 *        phase 3), placed by the placement module (@ref place_segment).
 *
 * The zero-size request keeps a NULL, empty `bytes` span rather than a one-past pointer into
 * the header, so an empty segment from this backend is indistinguishable from
 * @ref heap_backend_t's — the block is still released at @ref block_bytes(0), which is what
 * @ref destroy recomputes from `bytes.size()`.
 */
view::segment_t* source_backend_t::alloc(std::size_t size, alloc_hint_t /*hint*/) {
    void* const block = src_->try_alloc(block_bytes(size), kBlockAlign);
    return block != nullptr ? place_segment(this, block, size, kBlockAlign) : nullptr;
}

/**
 * @brief Destroy the control block and return the single block, sized as @ref alloc took it.
 *
 * `bytes.size()` is the originating request: nothing in the tree rewrites a live segment's
 * span, and @ref heap_backend_t's reclaim depends on the same invariant.
 */
void source_backend_t::destroy(view::segment_t* seg) noexcept {
    const std::size_t size = seg->bytes.size();
    seg->~segment_t();
    src_->release(seg, block_bytes(size), kBlockAlign);
}

namespace {

/** @brief The @ref detached_reclaimer type: reads the source back from the block's head. */
class detached_reclaimer_t final : public mem_backend_t {
   public:
    detached_reclaimer_t() noexcept : mem_backend_t("mem_detached") {}
    void destroy(view::segment_t* seg) noexcept override {
        constexpr std::size_t kPrefix = detached_source_backend_t::kPrefixBytes;
        std::byte* const block = inline_block_of(seg, kPrefix);
        block_source_t* const src = *reinterpret_cast<block_source_t* const*>(block);
        const std::size_t bytes = inline_block_bytes(kPrefix, seg->bytes.size());
        seg->~segment_t();
        src->release(block, bytes, detached_source_backend_t::kBlockAlign);
    }
    [[nodiscard]] std::size_t alignment() const noexcept override { return kInlinePayloadAlign; }
};

}  // namespace

mem_backend_t& detached_reclaimer() noexcept {
    // Never destroyed: a segment can be released after every static destructor has run.
    alignas(detached_reclaimer_t) static std::byte storage[sizeof(detached_reclaimer_t)];
    static detached_reclaimer_t* const r = new (storage) detached_reclaimer_t();
    return *r;
}

view::segment_t* detached_source_backend_t::alloc(std::size_t size, alloc_hint_t /*hint*/) {
    void* const block = src_->try_alloc(inline_block_bytes(kPrefixBytes, size), kBlockAlign);
    if (block == nullptr) return nullptr;
    *static_cast<block_source_t**>(block) = src_;
    return place_inline(&detached_reclaimer(), block, kPrefixBytes, size);
}

}  // namespace tr::mem
