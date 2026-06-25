// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// mem_borrowed — wrap caller-owned bytes in a segment WITHOUT taking ownership
// of them. destroy frees only the small Segment control block libtracer
// allocated; the user's bytes are never touched. This is the transparent
// byte-router / live-raw MVP (ADR-0012): point a segment at a register, a
// program variable, a const ROM table, or any externally-owned buffer, and
// libtracer routes those bytes — no copy, no CRC imposed. The caller guarantees
// the bytes stay valid while any view holds them.
//
// (A truly zero-allocation static descriptor — the permanent MMIO segment whose
// refcount is never released — is the deferred mem_mmio backend; this dynamic
// borrow heap-allocates only the ~32-byte control block.)
#pragma once

#include <new>
#include <span>

#include "libtracer/backend.hpp"
#include "libtracer/segment.hpp"

namespace tracer::mem {

namespace detail {

class BorrowedBackend final : public MemBackend {
   public:
    BorrowedBackend() noexcept : MemBackend("mem_borrowed") {}
    // No alloc: a borrow wraps existing bytes (see borrow() below).
    void destroy(Segment* seg) noexcept override { delete seg; }  // control block only
};

[[nodiscard]] inline MemBackend& borrowed_backend() noexcept {
    static BorrowedBackend backend;
    return backend;
}

}  // namespace detail

// Wrap writable caller-owned bytes.
[[nodiscard]] inline SegmentPtr borrow(std::span<std::byte> bytes) {
    return SegmentPtr::adopt(new (std::nothrow) Segment(&detail::borrowed_backend(), bytes));
}

// Wrap read-only caller-owned bytes (ROM, a const table, an MMIO read view). The
// span is const; libtracer never writes through a borrowed-const segment, so the
// const_cast only restores the segment's uniform writable-at-the-type-level base.
[[nodiscard]] inline SegmentPtr borrow_const(std::span<const std::byte> bytes) {
    std::span<std::byte> writable(const_cast<std::byte*>(bytes.data()), bytes.size());
    return SegmentPtr::adopt(new (std::nothrow) Segment(&detail::borrowed_backend(), writable));
}

}  // namespace tracer::mem
