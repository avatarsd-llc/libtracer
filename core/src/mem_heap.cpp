// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

#include "libtracer/mem_heap.hpp"

#include <cstdint>
#include <new>
#include <span>

namespace tracer::mem {
namespace {

class HeapBackend final : public MemBackend {
   public:
    HeapBackend() noexcept : MemBackend("mem_heap") {}

    Segment* alloc(std::size_t size, std::uint32_t /*hint*/) override {
        const std::align_val_t al{alignof(std::max_align_t)};
        void* raw = size ? ::operator new(size, al, std::nothrow) : nullptr;
        if (size && raw == nullptr) return nullptr;
        auto* seg = new (std::nothrow)
            Segment(this, std::span<std::byte>(static_cast<std::byte*>(raw), size));
        if (seg == nullptr) {
            if (raw) ::operator delete(raw, al);
            return nullptr;
        }
        return seg;
    }

    void destroy(Segment* seg) noexcept override {
        if (!seg->bytes.empty()) {
            ::operator delete(seg->bytes.data(), std::align_val_t{alignof(std::max_align_t)});
        }
        delete seg;
    }
};

}  // namespace

MemBackend& heap_backend() noexcept {
    static HeapBackend backend;
    return backend;
}

SegmentPtr heap_alloc(std::size_t size) { return SegmentPtr::adopt(heap_backend().alloc(size, 0)); }

}  // namespace tracer::mem
