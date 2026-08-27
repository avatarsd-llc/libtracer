/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_source_pmr — the HOSTED std-container adapter over tr::mem::block_source_t
 * (#873, ADR-0079). One direction only: block_source_t -> std::pmr::memory_resource.
 */
#pragma once

#include <cstddef>
#include <cstdlib>
#include <memory_resource>
#include <new>

#include "libtracer/mem_source.hpp"

/**
 * @file
 * @brief A `std::pmr::memory_resource` whose bytes come from an injected
 *        @ref tr::mem::block_source_t — the adapter ADR-0079 leaves `std::pmr` alive as.
 *
 * Deliberately a separate header, for the same reason `%mem_source_sync.hpp` is one:
 * `%mem_source.hpp` is compiled into the freestanding footprint sentinel, and
 * `<memory_resource>` is a hosted facility. It stays out of that sentinel's include
 * set for that reason.
 *
 * @note Until #873 phase 1 nothing under `core/` included this header at all. `%graph.hpp`
 *       now does: the collapsed `graph_t` constructor builds a @ref tr::mem::source_resource_t
 *       over its single injected source, which is how the graph's `std::pmr` channel reaches
 *       the substrate. That costs `%graph.hpp` nothing it did not already carry — it has
 *       named `std::pmr::memory_resource` since ADR-0039 — but the old claim that no in-tree
 *       header includes this one is no longer true, and is corrected here rather than left to
 *       mislead the next reader of the footprint sentinel's include list.
 *
 * @note ONE DIRECTION. The reverse adapter — wrapping a `std::pmr::memory_resource`
 *       so it can be used *as* a `block_source_t` — is not offered and must not be
 *       added. `memory_resource::allocate` is annotated `returns_nonnull` in libstdc++
 *       and signals exhaustion only by throwing, so such a wrapper's `try_alloc` could
 *       not answer `nullptr` honestly: on the shipping `-Os` profile the caller's null
 *       check is deleted outright (measured, ADR-0065 §1) and the throw reaches
 *       ESP-IDF's `__cxa_throw` → `abort()` stub. That is the exact defect
 *       @ref tr::mem::block_source_t exists to escape. A host with an existing pmr arena
 *       migrates by pointing @ref tr::mem::pool_source_t's span constructor at the same
 *       STORAGE instead (#1493); @ref tr::mem::block_source_t's warning carries the full
 *       reasoning, including why a budget-tracking variant is declined too.
 */

namespace tr::mem {

/**
 * @brief Serve a `std::pmr` container from an injected @ref block_source_t.
 *
 * The adapter ADR-0079 ends on: *"`std::pmr` survives only as a thin adapter for
 * std-container interop on non-failable paths."* It exists for the one case a store
 * migration cannot solve by retyping — a `std::pmr` container whose element type is
 * neither trivially copyable nor trivially destructible, so @ref block_array_t's two
 * static assertions reject it (`tr::net::can_reassembly_t`'s slice map holds a
 * refcounted `tr::view::view_t`; #873 family 5). Pointing such a container at a bounded
 * @ref pool_source_t is strictly better than leaving it on the process heap.
 *
 * @warning THIS DELIVERS PLACEMENT AND BOUNDING, NOT FAILABILITY. `std::pmr`'s only
 *          exhaustion signal is a throw, so this adapter's boundary is a `std::bad_alloc`
 *          on a hosted build and a `std::abort()` under `-fno-exceptions` — byte-for-byte
 *          the behaviour libstdc++ itself produces for the same throw on that profile.
 *          A peer-provoked path must therefore NOT be moved onto a `std::pmr` container
 *          just because this exists: a store that has to SURVIVE exhaustion migrates via
 *          the route-handle pattern (`docs/reference/09-memory-substrate.md`) onto
 *          @ref block_array_t and fails by value. What this buys is that the bytes come
 *          from the deployer's slab instead of the global heap, and that the slab's size
 *          is the bound.
 *
 * @note Stateless beyond the one pointer. There is deliberately NO refusal counter here:
 *       a non-atomic one races on a shared adapter and an atomic one puts a shared RMW on
 *       an allocation path, which #873's cadence rules a reject. Counting belongs to the
 *       injected source, which already has the vocabulary
 *       (@ref pool_source_t::used, @ref pool_source_t::classes_used,
 *       @ref pool_source_t::overflowed).
 *
 * @note Concurrency is entirely the injected source's contract — the adapter adds no
 *       shared state of its own. Own **one source per receiver**: ADR-0060 erratum 1
 *       measured a shared free-list pool collapsing to ~1/15 of its single-thread rate on
 *       a 12-core host. Wrapping it in a `memory_resource` does not change that.
 *
 * @note Blocks are host-owned: both the source and this adapter must outlive every
 *       container built over them.
 */
class source_resource_t final : public std::pmr::memory_resource {
   public:
    /** @brief Serve every request from @p src; @p src must outlive this adapter. */
    explicit source_resource_t(block_source_t& src) noexcept : src_(&src) {}

    /** @brief Non-copyable — a resource is an identity, exactly as a source is. */
    source_resource_t(const source_resource_t&) = delete;
    /** @brief Non-assignable. */
    source_resource_t& operator=(const source_resource_t&) = delete;

    /** @brief The source the bytes come from — for census and for sizing its slab. */
    [[nodiscard]] block_source_t& source() const noexcept { return *src_; }

   protected:
    /**
     * @brief Draw @p bytes aligned to @p alignment from the source.
     *
     * @throws std::bad_alloc The source refused. This is the adapter's boundary and the
     *         reason it is not a failable seam; under `-fno-exceptions` the refusal is
     *         `std::abort()` instead, which is what libstdc++ does with the same throw.
     */
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* const p = src_->try_alloc(bytes, alignment);
        if (p == nullptr) {
#if defined(__cpp_exceptions) && __cpp_exceptions
            throw std::bad_alloc();
#else
            std::abort();
#endif
        }
        return p;
    }

    /**
     * @brief Return a block to the source.
     *
     * `std::pmr`'s deallocate carries the original size and alignment, which maps 1:1 onto
     * @ref block_source_t::release's sized-reclaim contract — that is what lets a
     * @ref pool_source_t recycle these blocks with no per-block header.
     */
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        src_->release(p, bytes, alignment);
    }

    /**
     * @brief Identity comparison — two adapters are equal only when they are the SAME object.
     *
     * Address identity rather than a `dynamic_cast` on the source pointer: the reference
     * node ships `-fno-rtti`, so a cross-type `dynamic_cast` is not available to this
     * header at all. libstdc++'s own `monotonic_buffer_resource` answers the same way.
     *
     * @note The consequence is worth stating: two `source_resource_t`s over the SAME
     *       @ref block_source_t compare **unequal**, so containers built over them will
     *       copy rather than steal storage on a container move-assign. Construct one
     *       adapter per source and pass it around, rather than one per container.
     */
    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

   private:
    block_source_t* src_; /**< @brief Borrowed; never owned, and must outlive this. */
};

}  // namespace tr::mem
