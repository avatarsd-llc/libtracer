// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// The L0 memory-backend seam: the small, user-implementable interface every
// substrate implements (heap, borrowed/live, fixed pool — and, later, DMA,
// lwIP pbuf, SHM). libtracer never allocates on its own; it receives memory
// from a backend, refcounts it (segment.hpp), and casts the bytes to TLVs.
// Each backend owns and declares its own per-architecture concurrency/coherency
// contract — the protocol mandates none. See docs/reference/09-memory-substrate.md
// and ADR-0012 (modular memory binding; libtracer is a transparent byte router).
#pragma once

#include <cstddef>
#include <cstdint>

namespace tracer {

struct Segment;  // segment.hpp

// Direction of a DMA / cache-coherency transfer, for the optional cache hooks.
enum class IoDir : std::uint8_t {
    DeviceToCpu = 1,  // CPU will read bytes a device just wrote (invalidate)
    CpuToDevice = 2,  // a device will read bytes the CPU just wrote (clean)
};

// A memory backend. Subclass this to bind libtracer to any allocator — a heap,
// a fixed caller-owned arena, live registers, lwIP pbufs, DMA descriptors. The
// interface deliberately does not make allocation mandatory: many substrates
// cannot allocate (MMIO, hardware FIFOs), so `alloc` may return nullptr.
class MemBackend {
   public:
    explicit MemBackend(const char* name) noexcept : name_(name) {}
    virtual ~MemBackend() = default;

    MemBackend(const MemBackend&) = delete;
    MemBackend& operator=(const MemBackend&) = delete;

    // Allocate a fresh segment of at least `size` bytes (refcount = 1, for the
    // caller to adopt). Returns nullptr when the backend cannot satisfy the
    // request: out of memory, pool exhausted (the BACKPRESSURE signal,
    // docs/reference/09 §pressure), or allocation unsupported (live/MMIO). The
    // `hint` is backend-defined (e.g. a region tag); pass 0 for "don't care".
    [[nodiscard]] virtual Segment* alloc(std::size_t /*size*/, std::uint32_t /*hint*/) {
        return nullptr;
    }

    // Reclaim a segment whose refcount has reached zero. Frees whatever the
    // backend owns (the bytes and/or the Segment control block) and nothing it
    // does not — a borrowed backend never frees the user's bytes. Never called
    // on a live segment.
    virtual void destroy(Segment* seg) noexcept = 0;

    // Optional cache-coherency hooks for non-coherent DMA paths. No-ops by
    // default; only DMA-class backends override them (docs/reference/09 §cache).
    virtual void prepare_for_io(Segment* /*seg*/, IoDir /*dir*/) noexcept {}
    virtual void finalize_after_io(Segment* /*seg*/, IoDir /*dir*/) noexcept {}

    [[nodiscard]] virtual std::size_t alignment() const noexcept {
        return alignof(std::max_align_t);
    }
    [[nodiscard]] virtual std::size_t max_segment_size() const noexcept { return ~std::size_t{0}; }

    [[nodiscard]] const char* name() const noexcept { return name_; }

   private:
    const char* name_;
};

}  // namespace tracer
