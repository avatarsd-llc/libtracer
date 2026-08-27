/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_source_alloc — the standard-Allocator face of tr::mem::block_source_t, so a
 * std::vector whose element type block_array_t rejects can still draw from the node's
 * own store (#873 phase 1, ADR-0065/ADR-0079).
 */
#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#include "libtracer/mem_source.hpp"

/**
 * @file
 * @brief A C++ `Allocator` over @ref tr::mem::block_source_t (@ref tr::mem::source_allocator_t)
 *        and the `std::vector` alias the migrated growth sites spell
 *        (@ref tr::mem::source_vector_t).
 *
 * A separate header for the reason `%mem_source_sync.hpp` and `%mem_source_pmr.hpp` are:
 * `%mem_source.hpp` is compiled into the freestanding footprint sentinel, and this one pulls
 * in `<vector>`. A target that never names @ref tr::mem::source_allocator_t compiles
 * byte-identical objects with and without it.
 */

namespace tr::mem {

/**
 * @brief A standard Allocator serving @p T from an injected `block_source_t`.
 *
 * @par Why this exists when block_array_t already does
 * `block_array_t` is the right container for a failable path and the wrong one for most of
 * the graph's growth: it requires trivially copyable AND trivially destructible elements, and
 * the graph's collection tables hold `std::vector<std::byte>` keys, `std::shared_ptr` LKVs and
 * refcounted `view_t`s. Those sites were therefore stranded on `std::vector` over the GLOBAL
 * heap — #873's channel 4 — each with a `#981 residual` note in the source saying exactly
 * that. An allocator changes only WHERE the vector's block comes from and leaves the element
 * type and its destructors alone, so those sites can be bounded without being rewritten.
 *
 * @par The probe this makes CORRECT, which is the real prize
 * `%mem_heap.hpp` carries a standing warning against generalizing `tr::detail::try_reserve` to
 * `std::pmr::vector`, and the reason is the probe: on the `-fno-exceptions` profile the helper
 * tests the GLOBAL heap with a throwaway `operator new`, while a pmr container allocates from
 * its INJECTED resource — so it answers a question about memory the container will never
 * touch, and the real allocation still aborts. That objection does **not** apply here, because
 * this allocator's storage and the probe can be the SAME source: `try_reserve`'s
 * allocator-aware overload probes with `src.try_alloc` / `src.release` on the very store the
 * growth will draw from. The probe is still probe-then-commit and still carries the #850 race
 * window; what changes is that it stops asking the wrong allocator.
 *
 * @warning `allocate` still THROWS on exhaustion, because the standard Allocator requirements
 *          leave it no other signal. That is not a regression — a `std::vector` growing on the
 *          global heap throws too — and the growth helpers in `%mem_heap.hpp` are what turn it
 *          into a value. A path that must SURVIVE exhaustion with no throw anywhere in the
 *          picture still migrates to @ref block_array_t; this is for the paths that cannot.
 *
 * @note Stateful (one pointer), so a container carrying it is one word wider, and two
 *       allocators over different sources compare UNEQUAL — which is correct: a container
 *       move-assign across them must copy rather than steal a block the other source owns.
 * @note The source must outlive every container built over this allocator.
 */
template <class T>
class source_allocator_t {
   public:
    /** @brief The element type the standard Allocator requirements name. */
    using value_type = T;

    /** @brief Serve from @p src; @p src must outlive every container using this. */
    explicit source_allocator_t(block_source_t& src) noexcept : src_(&src) {}

    /** @brief Rebinding conversion — a container allocates its own node types through this. */
    template <class U>
    explicit(false) source_allocator_t(const source_allocator_t<U>& o) noexcept
        : src_(&o.source()) {}

    /** @brief The store the blocks come from — for a census, and for the growth helpers' probe. */
    [[nodiscard]] block_source_t& source() const noexcept { return *src_; }

    /**
     * @brief Take storage for @p n elements.
     * @throws std::bad_alloc The source refused. See the class warning: the Allocator
     *         requirements admit no by-value refusal, so the conversion to a value happens
     *         one level up, in `tr::detail::try_reserve` / `try_push_back`.
     */
    [[nodiscard]] T* allocate(std::size_t n) {
        void* const p = src_->try_alloc(n * sizeof(T), alignof(T));
        if (p == nullptr) {
#if defined(__cpp_exceptions) && __cpp_exceptions
            throw std::bad_alloc();
#else
            std::abort();
#endif
        }
        return static_cast<T*>(p);
    }

    /** @brief Return storage for @p n elements — the seam's SIZED reclaim, matched exactly. */
    void deallocate(T* p, std::size_t n) noexcept { src_->release(p, n * sizeof(T), alignof(T)); }

    /** @brief Two allocators are interchangeable iff they serve from the SAME source. */
    template <class U>
    [[nodiscard]] bool operator==(const source_allocator_t<U>& o) const noexcept {
        return src_ == &o.source();
    }

   private:
    block_source_t* src_; /**< @brief Borrowed; never owned, and must outlive every container. */
};

/**
 * @brief A `std::vector` drawn from an injected @ref block_source_t — the spelling the
 *        migrated growth sites use.
 *
 * An alias rather than a bare `std::vector<T, source_allocator_t<T>>` at each site, so the
 * migration reads as one decision and a later change of substrate container is one edit.
 */
template <class T>
using source_vector_t = std::vector<T, source_allocator_t<T>>;

/**
 * @brief Tell `tr::detail::try_reserve` which store a @ref source_allocator_t draws from —
 *        the ADL hook that aims its `-fno-exceptions` probe at the right memory.
 *
 * Declared in `tr::mem` rather than beside the generic `tr::detail::growth_source` on
 * purpose: the call in `try_reserve` is unqualified with a dependent argument, so ordinary
 * lookup at the template's definition sees only the generic overload and this one is reached
 * by ARGUMENT-DEPENDENT lookup at instantiation. Partial ordering then prefers it. Putting it
 * in `tr::detail` would compile and would never be selected.
 */
template <class T>
[[nodiscard]] constexpr block_source_t* growth_source(const source_allocator_t<T>& a) noexcept {
    return &a.source();
}

}  // namespace tr::mem
