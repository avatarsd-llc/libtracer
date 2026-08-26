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

view::segment_t* source_backend_t::alloc(std::size_t size, alloc_hint_t /*hint*/) {
    void* raw = nullptr;
    if (size != 0) {
        raw = src_->try_alloc(size);
        if (raw == nullptr) return nullptr;
    }
    void* const cb = src_->try_alloc(sizeof(view::segment_t), alignof(view::segment_t));
    if (cb == nullptr) {
        if (raw != nullptr) src_->release(raw, size);
        return nullptr;
    }
    return new (cb) view::segment_t(this, std::span<std::byte>(static_cast<std::byte*>(raw), size));
}

void source_backend_t::destroy(view::segment_t* seg) noexcept {
    const std::span<std::byte> bytes = seg->bytes;
    seg->~segment_t();
    src_->release(seg, sizeof(view::segment_t), alignof(view::segment_t));
    if (!bytes.empty()) src_->release(bytes.data(), bytes.size());
}

}  // namespace tr::mem
