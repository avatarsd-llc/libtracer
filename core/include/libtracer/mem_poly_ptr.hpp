/**
 * @file
 * @brief `tr::mem::poly_ptr_t` — the sole owner of one object, possibly of a class derived
 *        from the pointer's type, in a block drawn from a `block_source_t`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * What a `std::unique_ptr<base_t>` holding a derived object becomes on the one allocation seam
 * (ADR-0083, #1780). A source is sized-reclaim (`release(p, bytes, align)` must name the shape
 * `try_alloc` served, and the slab pool files a block by that shape), so an owner that only
 * knows the BASE type cannot return a derived object's block: it would name the base's size.
 * This owner records the block, its size and its alignment when the object is made, and returns
 * exactly that shape, whatever the pointer has since been converted to. The transports are the
 * first users: a factory makes a concrete link and hands it back as a `poly_ptr_t<transport_t>`,
 * and a server's session table owns its protocol's derived sessions.
 *
 * It also records how to destroy the object: `make_poly` stores the destructor of the class it
 * built, so @p T needs no virtual destructor. That is what lets the seam bases
 * (`block_source_t`, `transport_t`, `can_link_t`, a server's session) have a protected,
 * non-virtual one: a class with a virtual destructor emits a deleting destructor that names
 * `operator delete` in every object that emits its vtable, and the MCU archive must name none
 * (#2022).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "libtracer/mem_source.hpp"

namespace tr::mem {

/**
 * @brief The sole owner of one @p T (or of an object of a class derived from @p T) in a block
 *        from a @ref block_source_t; destroys it as the class @ref make_poly built and returns
 *        the block it was made in, at the size and alignment it was made with.
 *
 * Made by @ref make_poly. Movable, non-copyable. Converts from `poly_ptr_t<U>` when `U*`
 * converts to `T*`. The object is destroyed through the destructor of the class `make_poly`
 * built, recorded then, never through `~T`, so @p T needs no virtual destructor.
 *
 * Five words on a 64-bit target: the source, the block, the object, its destructor, and the
 * block's shape.
 */
template <class T>
class poly_ptr_t {
   public:
    /** @brief An empty owner. */
    poly_ptr_t() noexcept = default;
    /** @brief An empty owner (mirrors `std::unique_ptr(nullptr)`). */
    poly_ptr_t(std::nullptr_t) noexcept {}  // NOLINT(google-explicit-constructor)
    /** @brief Destroys the object and returns its block. */
    ~poly_ptr_t() { reset(); }
    /** @brief Non-copyable — one object, one owner. */
    poly_ptr_t(const poly_ptr_t&) = delete;
    /** @brief Non-assignable by copy. */
    poly_ptr_t& operator=(const poly_ptr_t&) = delete;
    /** @brief Take over @p o's object; @p o is left empty. */
    poly_ptr_t(poly_ptr_t&& o) noexcept { steal(o); }
    /** @brief Take over @p o's object (an owner of a derived type); @p o is left empty. */
    template <class U>
        requires(!std::is_same_v<U, T> && std::is_convertible_v<U*, T*>)
    poly_ptr_t(poly_ptr_t<U>&& o) noexcept {  // NOLINT(google-explicit-constructor)
        steal(o);
    }
    /** @brief Free this owner's object, then take over @p o's. */
    poly_ptr_t& operator=(poly_ptr_t&& o) noexcept {
        if (this != &o) {
            reset();
            steal(o);
        }
        return *this;
    }
    /** @brief Free the object, if any (mirrors `std::unique_ptr = nullptr`). */
    poly_ptr_t& operator=(std::nullptr_t) noexcept {
        reset();
        return *this;
    }

    /** @brief Destroy the object, if any, and return its block. */
    void reset() noexcept {
        if (p_ == nullptr) return;
        destroy_(block_);
        src_->release(block_, bytes_, align_);
        p_ = nullptr;
    }
    /** @brief The object, or null. */
    [[nodiscard]] T* get() const noexcept { return p_; }
    /** @brief Member access. Precondition: not empty. */
    [[nodiscard]] T* operator->() const noexcept { return p_; }
    /** @brief The object. Precondition: not empty. */
    [[nodiscard]] T& operator*() const noexcept { return *p_; }
    /** @brief True when an object is owned. */
    [[nodiscard]] explicit operator bool() const noexcept { return p_ != nullptr; }
    /** @brief True when empty (`p == nullptr`). */
    friend bool operator==(const poly_ptr_t& p, std::nullptr_t) noexcept { return p.p_ == nullptr; }

   private:
    template <class U>
    friend class poly_ptr_t;
    template <class U, class... Args>
    friend poly_ptr_t<U> make_poly(block_source_t& src, Args&&... args) noexcept;

    /** @brief Move @p o's state here and leave @p o empty. */
    template <class U>
    void steal(poly_ptr_t<U>& o) noexcept {
        src_ = o.src_;
        block_ = o.block_;
        p_ = o.p_;
        destroy_ = o.destroy_;
        bytes_ = o.bytes_;
        align_ = o.align_;
        o.p_ = nullptr;
    }

    block_source_t* src_ = nullptr; /**< @brief The source that served `block_`. */
    void* block_ = nullptr;         /**< @brief The block, as the source handed it out. */
    T* p_ = nullptr;                /**< @brief The owned object, or null. */
    /** @brief Runs the destructor of the class `make_poly` built in `block_`. */
    void (*destroy_)(void* block) noexcept = nullptr;
    std::uint32_t bytes_ = 0; /**< @brief The block's size, as requested. */
    std::uint32_t align_ = 0; /**< @brief The block's alignment, as requested. */
};

/**
 * @brief Construct one @p T from @p args in a block from @p src and own it — the core's
 *        failable `std::make_unique` (ADR-0083 Decision 1).
 * @return An empty owner when the source refused; nothing was constructed.
 */
template <class T, class... Args>
[[nodiscard]] poly_ptr_t<T> make_poly(block_source_t& src, Args&&... args) noexcept {
    static_assert(sizeof(T) <= 0xFFFFFFFFu, "poly_ptr_t records the block size in 32 bits");
    poly_ptr_t<T> out;
    void* block = src.try_alloc(sizeof(T), alignof(T));
    if (block == nullptr) return out;
    out.src_ = &src;
    out.block_ = block;
    out.p_ = ::new (block) T(std::forward<Args>(args)...);
    out.destroy_ = [](void* b) noexcept { std::launder(static_cast<T*>(b))->~T(); };
    out.bytes_ = static_cast<std::uint32_t>(sizeof(T));
    out.align_ = static_cast<std::uint32_t>(alignof(T));
    return out;
}

}  // namespace tr::mem
