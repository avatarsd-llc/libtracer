/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/mem_heap.hpp"

#include <new>
#include <span>

namespace tr::mem {
namespace {

class heap_backend_t final : public mem_backend_t {
   public:
    heap_backend_t() noexcept : mem_backend_t("mem_heap") {}

    view::segment_t* alloc(std::size_t size, alloc_hint_t /*hint*/) override {
        const std::align_val_t al{alignof(std::max_align_t)};
        void* raw = size ? ::operator new(size, al, std::nothrow) : nullptr;
        if (size && raw == nullptr) return nullptr;
        auto* seg = new (std::nothrow)
            view::segment_t(this, std::span<std::byte>(static_cast<std::byte*>(raw), size));
        if (seg == nullptr) {
            if (raw) ::operator delete(raw, al);
            return nullptr;
        }
        return seg;
    }

    void destroy(view::segment_t* seg) noexcept override {
        if (!seg->bytes.empty()) {
            ::operator delete(seg->bytes.data(), std::align_val_t{alignof(std::max_align_t)});
        }
        delete seg;
    }
};

}  // namespace

mem_backend_t& heap_backend() noexcept {
    static heap_backend_t backend;
    return backend;
}

}  // namespace tr::mem

namespace tr::view {

segment_ptr_t heap_alloc(std::size_t size) {
    return segment_ptr_t::adopt(mem::heap_backend().alloc(size, mem::alloc_hint_t::NONE));
}

}  // namespace tr::view
