/**
 * @file
 * @brief `tr::net::fixed_array_t` — a run of @p T drawn ONCE from a `block_source_t`, the
 *        shape every once-sized buffer and table in this component's links has.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What a `std::unique_ptr<T[]>(new (std::nothrow) T[n])` becomes on the one allocation seam
 * (ADR-0083, #1880): the link's RX scratch, its TX slot pool and their payload blocks, the
 * large size class and its claimed flags, a per-frame payload that outgrew the scratch, and
 * the teardown's session snapshot. None of them ever grows, and two of them hold atomics,
 * which `mem::block_array_t` cannot (it relocates by move). So the run is drawn at its final
 * size, its elements are built in place and never move, and the block goes back at the size
 * and alignment it was drawn with.
 *
 * @ref leak is the one thing `std::unique_ptr::release` was used for in these links: a
 * teardown that cannot prove a queued send has finished with a block abandons it rather
 * than free memory that send may still address (the #815 discipline).
 */
#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>

#include "libtracer/mem_source.hpp"

namespace tr::net {

/**
 * @brief The sole owner of `size()` default-initialized @p T in one block from a
 *        @ref mem::block_source_t. Empty until @ref assign succeeds; never resized after.
 *
 * Non-copyable and non-movable: the links hand out pointers into it (a slot's inline
 * buffer, a work item's claimed flag), so it never relocates.
 */
template <class T>
class fixed_array_t {
   public:
    /** @brief An empty array (holds no block). */
    fixed_array_t() noexcept = default;
    /** @brief Destroys the elements and returns the block, if one is held. */
    ~fixed_array_t() { reset(); }
    /** @brief Non-copyable — one run, one owner. */
    fixed_array_t(const fixed_array_t&) = delete;
    /** @brief Non-assignable. */
    fixed_array_t& operator=(const fixed_array_t&) = delete;

    /**
     * @brief Free what is held, then draw @p n elements from @p src and build them.
     *        Zero elements draw nothing and succeed, leaving the array empty.
     * @retval false @p src refused (or `n * sizeof(T)` overflows); the array is left empty.
     */
    [[nodiscard]] bool assign(mem::block_source_t& src, std::size_t n) noexcept {
        // Here rather than on the class, so a member may name a @p T the header only
        // declares: the link's TX slot is defined in its own translation unit.
        static_assert(std::is_nothrow_default_constructible_v<T>,
                      "fixed_array_t builds its elements in place, which must not throw");
        static_assert(std::is_nothrow_destructible_v<T>, "fixed_array_t: ~T must be noexcept");
        reset();
        if (n == 0) return true;
        if (n > static_cast<std::size_t>(-1) / sizeof(T)) return false;
        void* const block = src.try_alloc(n * sizeof(T), alignof(T));
        if (block == nullptr) return false;
        p_ = static_cast<T*>(block);
        // A trivial element (a payload byte) is left as the source served it, as
        // `new std::byte[n]` left it: zeroing a per-frame buffer that is about to be
        // overwritten would be a second pass over every byte for nothing.
        if constexpr (!std::is_trivially_default_constructible_v<T>)
            for (std::size_t i = 0; i < n; ++i) std::construct_at(p_ + i);
        src_ = &src;
        n_ = n;
        return true;
    }
    /** @brief Destroy the elements and return the block, if one is held. */
    void reset() noexcept {
        if (p_ == nullptr) return;
        if constexpr (!std::is_trivially_destructible_v<T>)
            for (std::size_t i = 0; i < n_; ++i) std::destroy_at(p_ + i);
        src_->release(p_, n_ * sizeof(T), alignof(T));
        p_ = nullptr;
        n_ = 0;
    }
    /**
     * @brief Abandon the block: forget it without destroying or returning it, so it stays
     *        addressable for whatever may still reach it after its owner is gone.
     */
    void leak() noexcept {
        p_ = nullptr;
        n_ = 0;
    }

    /** @brief The first element, or null when empty. */
    [[nodiscard]] T* data() const noexcept { return p_; }
    /** @brief Element @p i. Precondition: `i < size()`. */
    [[nodiscard]] T& operator[](std::size_t i) const noexcept { return p_[i]; }
    /** @brief Elements held; 0 when empty. */
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    /** @brief True when a block is held. */
    [[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }
    /** @brief True when empty (`a == nullptr`). */
    friend bool operator==(const fixed_array_t& a, std::nullptr_t) noexcept {
        return a.p_ == nullptr;
    }

   private:
    mem::block_source_t* src_ = nullptr; /**< @brief The source that served `p_`. */
    T* p_ = nullptr;                     /**< @brief The run, or null. */
    std::size_t n_ = 0;                  /**< @brief Elements in the run. */
};

}  // namespace tr::net
