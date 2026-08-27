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
#include <span>

#include "libtracer/segment.hpp"

namespace tr::mem {

/**
 * @brief One draw: the control block at the head of the block, the payload behind it (#873
 *        phase 3).
 *
 * The zero-size request keeps a NULL, empty `bytes` span rather than a one-past pointer into
 * the header, so an empty segment from this backend is indistinguishable from
 * @ref heap_backend_t's — the block is still released at @ref block_bytes(0), which is what
 * @ref destroy recomputes from `bytes.size()`.
 */
view::segment_t* source_backend_t::alloc(std::size_t size, alloc_hint_t /*hint*/) {
    void* const block = src_->try_alloc(block_bytes(size), kBlockAlign);
    if (block == nullptr) return nullptr;
    auto* const base = static_cast<std::byte*>(block);
    std::byte* const payload = size != 0 ? base + kHeaderBytes : nullptr;
    return new (base) view::segment_t(this, std::span<std::byte>(payload, size));
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

}  // namespace tr::mem
