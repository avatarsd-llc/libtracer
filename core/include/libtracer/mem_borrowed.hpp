// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// mem_borrowed — wrap caller-owned bytes in a segment WITHOUT taking ownership
// of them. destroy frees only the small segment_t control block libtracer
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

/**
 * @file
 * @brief The `mem_borrowed` L0 backend (`tr::mem`) and its L1 borrow helpers (`tr::view`).
 */

namespace tr::mem {

namespace detail {

/** @brief Backend for borrowed bytes: reclaims only the `segment_t` control block. */
class borrowed_backend_t final : public mem_backend_t {
   public:
    borrowed_backend_t() noexcept : mem_backend_t("mem_borrowed") {}
    // No alloc: a borrow wraps existing bytes (see tr::view::borrow below).
    void destroy(view::segment_t* seg) noexcept override { delete seg; }  // control block only
};

/** @brief The process-wide borrowed backend (function-local static). */
[[nodiscard]] inline mem_backend_t& borrowed_backend() noexcept {
    static borrowed_backend_t backend;
    return backend;
}

/** @brief A borrowed backend that reports `DEVICE` space — for non-CPU memory
 *         (and as a CUDA-free stand-in to test the codec's device-link handling). */
class borrowed_device_backend_t final : public mem_backend_t {
   public:
    borrowed_device_backend_t() noexcept : mem_backend_t("mem_borrowed_device") {}
    void destroy(view::segment_t* seg) noexcept override { delete seg; }
    [[nodiscard]] mem_space_t space() const noexcept override { return mem_space_t::DEVICE; }
};

/** @brief The process-wide device-borrowed backend (function-local static). */
[[nodiscard]] inline mem_backend_t& borrowed_device_backend() noexcept {
    static borrowed_device_backend_t backend;
    return backend;
}

}  // namespace detail

}  // namespace tr::mem

namespace tr::view {

/**
 * @brief Wrap writable caller-owned @p bytes in a segment without owning them.
 *
 * An L1 handle producer (docs/adr/0016 §2). The caller guarantees the bytes
 * outlive every view that holds them.
 */
[[nodiscard]] inline segment_ptr_t borrow(std::span<std::byte> bytes) {
    return segment_ptr_t::adopt(new (std::nothrow)
                                    segment_t(&mem::detail::borrowed_backend(), bytes));
}

/**
 * @brief Wrap read-only caller-owned @p bytes (ROM, a const table, an MMIO read view).
 *
 * The span is const; libtracer never writes through a borrowed-const segment,
 * so the `const_cast` only restores the segment's uniform writable-at-the-type-
 * level base.
 */
[[nodiscard]] inline segment_ptr_t borrow_const(std::span<const std::byte> bytes) {
    std::span<std::byte> writable(const_cast<std::byte*>(bytes.data()), bytes.size());
    return segment_ptr_t::adopt(new (std::nothrow)
                                    segment_t(&mem::detail::borrowed_backend(), writable));
}

/**
 * @brief Wrap caller-owned @p bytes as a `DEVICE`-space (non-CPU) segment.
 *
 * The resulting view reports @ref view_t::is_device; the codec must not
 * CPU-dereference it (docs/adr/0024). The real device backend is `mem_cuda`;
 * this borrow tags existing memory `DEVICE` (e.g. for tests or a custom binding).
 */
[[nodiscard]] inline segment_ptr_t borrow_device(std::span<std::byte> bytes) {
    return segment_ptr_t::adopt(new (std::nothrow)
                                    segment_t(&mem::detail::borrowed_device_backend(), bytes));
}

}  // namespace tr::view
