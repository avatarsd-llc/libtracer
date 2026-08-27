/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_source_backend — the `mem_backend_t` WRAPPER over a `tr::mem::block_source_t`
 * (#873 phase 1, ADR-0079). One direction only, exactly as `%mem_source_pmr.hpp` is:
 * block_source_t -> mem_backend_t.
 */
#pragma once

#include <cstddef>
#include <new>
#include <span>

#include "libtracer/backend.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/segment.hpp"

/**
 * @file
 * @brief A @ref tr::mem::mem_backend_t whose bytes come from an injected
 *        @ref tr::mem::block_source_t — need C (refcounted segments + DMA hooks)
 *        re-expressed as a wrapper over the substrate.
 */

namespace tr::mem {

/**
 * @brief Serve refcounted @ref view::segment_t allocations from an injected
 *        @ref block_source_t.
 *
 * @par What this type IS, after #873 phase 1
 * ADR-0079 kept `mem_backend_t` separate from `block_source_t` because a segment carries
 * things raw bytes do not: an intrusive refcount and the DMA cache-op hook pair. The
 * 2026-08-26 ruling settled the relationship between them — the substrate is
 * @ref block_source_t, and `mem_backend_t` survives as a **wrapper TYPE** (a source plus
 * that refcount/DMA-hook table) rather than as an injection seam of its own. This class is
 * that wrapper. `graph_t` no longer takes a backend at construction; it builds one of these
 * over the single source it IS given.
 *
 * @par The failure convention
 * The substrate speaks raw `nullptr`-on-exhaustion and does no wrapping (the ruling's
 * round-2 detail). This adapter translates at its own boundary and nowhere else: a refused
 * `try_alloc` becomes a **null `segment_t*`**, which is precisely the BACKPRESSURE signal
 * @ref mem_backend_t::alloc already documents. Nothing throws, nothing aborts, and no
 * `result_t` appears in the substrate — contrast @ref source_resource_t, whose `std::pmr`
 * contract forces it to translate the same `nullptr` into a `std::bad_alloc`.
 *
 * @par Two blocks per segment, deliberately
 * The control block and the payload bytes are two separate @ref block_source_t::try_alloc
 * calls, mirroring @ref heap_backend_t's `operator new` pair one for one. Packing them into
 * one block would be cheaper and is the obvious improvement — it is deliberately NOT taken
 * here, because phase 1's contract is that the process-default composition behaves as it
 * always did; re-layering the backend is #873 phase 3.
 *
 * @note The tag stays @ref backend_tag::UNKNOWN, so reclaim takes the virtual `destroy`
 *       fallback rather than the ADR-0047 §2 devirtualized switch arm. That is the same
 *       path every out-of-core backend already takes, and it costs the DEFAULT composition
 *       nothing: `graph_t` folds a process-default source back onto @ref heap_backend
 *       (tagged `HEAP`) and never constructs this type at all. Giving the module set a
 *       `SOURCE` enumerator is a phase-3 question, since it is the backend/pmr re-layering
 *       that decides whether this type is the only backend left.
 *
 * @note @ref alloc and @ref destroy are defined OUT OF LINE (`core/src/mem_source_backend.cpp`),
 *       unlike @ref heap_backend_t's, and that is deliberate rather than stylistic. This type is
 *       tagged `UNKNOWN`, so its reclaim is a virtual call in every case and inlining the bodies
 *       buys nothing — while making them visible in a TU that *holds* one (`graph.cpp` does, as
 *       `graph_t`'s internal wrapper) MEASURABLY re-partitions GCC's inline budget there: it
 *       flipped `tr::view::segment_ptr_t::reset` from an out-of-line call into
 *       `graph_t::dispatch_edge_remote`, growing that pinned symbol by 32 B (318 → 350) for no
 *       reason connected to what the wrapper does. Bisected against the symbol ratchet; the same
 *       hazard class `graph_t`'s `payload_right_store_` comment records. Keep them out of line.
 *
 * @note Concurrency, alignment and lifetime are ENTIRELY the injected source's contract —
 *       this adapter adds no state beyond one pointer. ADR-0060 §2's requirement stands:
 *       a value segment self-routes its reclaim on whatever thread drops the last ref, so
 *       a source injected into a graph must be thread-safe on a target where that happens.
 */
class source_backend_t final : public mem_backend_t {
   public:
    /** @brief Serve every segment from @p src; @p src must outlive this backend. */
    explicit source_backend_t(block_source_t& src) noexcept
        : mem_backend_t("mem_source"), src_(&src) {}

    /** @brief The source the bytes come from — for census and for sizing its slab. */
    [[nodiscard]] block_source_t& source() const noexcept { return *src_; }

    /**
     * @brief Allocate a @p size-byte segment (refcount 1) from the source.
     *
     * @param size Payload bytes; a zero-size request yields an empty-but-valid segment,
     *             exactly as @ref heap_backend_t's does.
     * @retval nullptr The source refused either block — BACKPRESSURE. Any block already
     *                 taken is returned before answering, so a refusal leaks nothing.
     */
    [[nodiscard]] view::segment_t* alloc(std::size_t size,
                                         alloc_hint_t hint = alloc_hint_t::NONE) override;

    /** @brief Return both blocks to the source, in the sizes @ref alloc took them in. */
    void destroy(view::segment_t* seg) noexcept override;

    /** @brief The alignment @ref block_source_t::try_alloc's default requests. */
    [[nodiscard]] std::size_t alignment() const noexcept override {
        return alignof(std::max_align_t);
    }

    // Module-set traits (ADR-0047 §2), read only by a `transfer_host<>` instantiation.
    static constexpr bool needs_cache_ops =
        false; /**< @brief The source declares its own DMA needs; this wrapper adds none. */
    static constexpr bool is_isr_safe =
        false; /**< @brief Whatever the source is; assume the strictest. */
    static constexpr bool is_nonblocking =
        false; /**< @brief Whatever the source is; assume the strictest. */
    static constexpr bool owns_bytes =
        true; /**< @brief The blocks are the source's, held for the segment's whole life. */

   private:
    block_source_t* src_; /**< @brief Borrowed; never owned, and must outlive this. */
};

}  // namespace tr::mem
