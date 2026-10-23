/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_borrowed — wrap caller-owned bytes in a segment WITHOUT taking ownership
 * of them. destroy returns only the small segment_t control block libtracer
 * drew; the user's bytes are never touched. This is the transparent
 * byte-router / live-raw MVP (ADR-0012): point a segment at a register, a
 * program variable, a const ROM table, or any externally-owned buffer, and
 * libtracer routes those bytes — no copy, no CRC imposed. The caller guarantees
 * the bytes stay valid while any view holds them.
 *
 * The control block is drawn from the default value source (`mem::value_source()`) and
 * returned to it, never from the process heap: the host root's value pool, the MCU arena,
 * or the platform heap only on the heap-rooted build (#2090). A truly zero-allocation
 * static descriptor — the permanent MMIO segment whose refcount is never released — is the
 * deferred mem_mmio backend.
 */
#pragma once

#include <new>
#include <span>

#include "libtracer/backend.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/segment.hpp"

/**
 * @file
 * @brief The `mem_borrowed` L0 backend (`tr::mem`) and its L1 borrow helpers (`tr::view`).
 */

namespace tr::mem {

namespace detail {

/**
 * @brief Backend for borrowed bytes: draws and returns only the `segment_t` control block,
 *        through the block source it was built over.
 *
 * One class serves both address spaces: @p space is what its segments report, and the tag
 * follows it, so `mem::transfer` refuses a `DEVICE` borrow like any other device segment.
 */
class borrowed_backend_t final : public mem_backend_t {
    // The state comes first: a trailing `private:` in this excluded detail class leaks into
    // the enclosing namespace under Doxygen and hides `view::borrow*` from the docs.
    block_source_t* src_; /**< @brief Where the control blocks come from. */
    mem_space_t space_;   /**< @brief What its segments report. */

   public:
    /** @brief A backend whose control blocks come from @p src and whose segments report
     *         @p space. @p src must outlive every segment it served. */
    borrowed_backend_t(block_source_t& src, mem_space_t space) noexcept
        : mem_backend_t(space == mem_space_t::DEVICE ? "mem_borrowed_device" : "mem_borrowed"),
          src_(&src),
          space_(space) {}

    /**
     * @brief A segment over @p bytes, its control block drawn from this backend's source.
     * @retval nullptr The source refused the control block (backpressure).
     */
    [[nodiscard]] view::segment_t* wrap(std::span<std::byte> bytes) noexcept {
        void* const p = src_->try_alloc(sizeof(view::segment_t), alignof(view::segment_t));
        return p == nullptr ? nullptr : ::new (p) view::segment_t(this, bytes);
    }
    /** @brief Return the control block to the source it came from; the bytes are not ours. */
    void destroy(view::segment_t* seg) noexcept override {
        seg->~segment_t();
        src_->release(seg, sizeof(view::segment_t), alignof(view::segment_t));
    }
    /** @brief The address space its segments report. */
    [[nodiscard]] mem_space_t space() const noexcept override { return space_; }
    /** @brief `BORROWED`, or `BORROWED_DEVICE` for a device-space borrow. */
    [[nodiscard]] backend_tag tag() const noexcept override {
        return space_ == mem_space_t::DEVICE ? backend_tag::BORROWED_DEVICE : backend_tag::BORROWED;
    }

    // Module-set traits (ADR-0047 §2). `needs_cache_ops` is read by `mem::transfer`. A
    // DEVICE borrow is host memory tagged DEVICE (a vendor-free stand-in), and
    // `mem::transfer` refuses it by its SPACE tag, not this trait (#928).
    static constexpr bool needs_cache_ops =
        false; /**< @brief No DMA cache maintenance (host RAM). */
    static constexpr bool is_isr_safe =
        false; /**< @brief `destroy` returns the control block to a source that may lock. */
    static constexpr bool is_nonblocking =
        false; /**< @brief The source may lock or draw a slab (#928). */
    static constexpr bool owns_bytes =
        false; /**< @brief Borrows caller-owned bytes — must NOT be durably stored. */
};

/** @brief The process-wide borrowed backend (function-local static) over the default value
 *         source. */
[[nodiscard]] inline borrowed_backend_t& borrowed_backend() noexcept {
    static borrowed_backend_t backend(value_source(), mem_space_t::HOST);
    return backend;
}

/** @brief The process-wide borrowed backend whose segments report `DEVICE` space — for
 *         non-CPU memory (and as a vendor-free stand-in to test the codec's device-link
 *         handling). */
[[nodiscard]] inline borrowed_backend_t& borrowed_device_backend() noexcept {
    static borrowed_backend_t backend(value_source(), mem_space_t::DEVICE);
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
    return segment_ptr_t::adopt(mem::detail::borrowed_backend().wrap(bytes));
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
    return segment_ptr_t::adopt(mem::detail::borrowed_backend().wrap(writable));
}

/**
 * @brief Wrap caller-owned @p bytes as a `DEVICE`-space (non-CPU) segment.
 *
 * The resulting view reports @ref view_t::is_device; the codec must not
 * CPU-dereference it (docs/adr/0024). A real device backend lives in the
 * `backends/` tier and registers its own byte-move (`register_device_backend`);
 * this borrow tags existing memory `DEVICE` (e.g. for tests or a custom binding),
 * registers nothing, and so `mem::transfer` refuses it.
 */
[[nodiscard]] inline segment_ptr_t borrow_device(std::span<std::byte> bytes) {
    return segment_ptr_t::adopt(mem::detail::borrowed_device_backend().wrap(bytes));
}

}  // namespace tr::view
