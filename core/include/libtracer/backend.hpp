/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The L0 memory-backend seam: the small, user-implementable interface every
 * substrate implements (heap, borrowed/live, fixed pool — and, later, DMA,
 * lwIP pbuf, SHM). libtracer never allocates on its own; it receives memory
 * from a backend, refcounts it (segment.hpp), and casts the bytes to TLVs.
 * Each backend owns and declares its own per-architecture concurrency/coherency
 * contract — the protocol mandates none. See docs/reference/09-memory-substrate.md,
 * docs/adr/0012 (modular memory binding; transparent byte router) and
 * docs/adr/0016 (layer namespaces; no templates through the seam).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

/**
 * @file
 * @brief L0 (`tr::mem`) memory-backend interface and its DMA/allocation enums.
 */

namespace tr::view {
// The L0↔L1 boundary type, defined at L1 (segment.hpp) because it carries the
// refcount. It is the one `tr::view` symbol the L0 interface is permitted to
// name (docs/adr/0016 §2): `alloc` returns one, `destroy` reclaims one.
struct segment_t;
}  // namespace tr::view

namespace tr::mem {

/**
 * @brief Direction of a DMA / cache-coherency transfer, for the cache hooks.
 *
 * The hook method carries the *timing* (before/after the transfer); this enum
 * carries the *direction*; the backend maps the pair to clean/invalidate.
 */
enum class io_dir_t : std::uint8_t {
    DEVICE_TO_CPU = 1, /**< @brief After DMA-in: invalidate so the CPU reads HW's writes. */
    CPU_TO_DEVICE = 2, /**< @brief Before DMA-out: clean so HW reads the CPU's writes. */
};

/**
 * @brief Opaque, backend-private allocation hint.
 *
 * A hint's meaning is private to the backend that defines it: there is **no**
 * cross-backend hint registry, no two backends share a value's meaning, and a
 * hint-ignoring backend accepts any value (docs/adr/0016 §"Considered options").
 * This strong typedef also stops a hint being swapped for a `size` argument.
 */
enum class alloc_hint_t : std::uint32_t {
    NONE = 0, /**< @brief "Don't care" — the default for every `alloc` call. */
};

/**
 * @brief The address space a backend's bytes live in.
 *
 * `HOST` bytes are CPU-addressable; `DEVICE` bytes are not (e.g. CUDA device
 * memory — docs/adr/0024). The codec must never CPU-dereference a `DEVICE` link:
 * such a segment may back only an opaque VALUE payload, with the header/trailer
 * kept in a `HOST` segment (a heterogeneous host+device rope).
 */
enum class mem_space_t : std::uint8_t {
    HOST = 0,   /**< @brief CPU-addressable bytes. */
    DEVICE = 1, /**< @brief Non-CPU-addressable bytes (GPU/accelerator); codec must not deref. */
};

/**
 * @brief Which build-time-closed backend a segment came from — the module-set
 *        tag (ADR-0047 §2).
 *
 * A segment carries its backend's tag so the per-segment-release destroy
 * dispatch (`segment_ptr_t::reset` → @ref destroy_dispatch) is a `switch` →
 * devirtualized direct call rather than a vtable indirect — foldable to a single
 * direct call when a target links only one backend. An unrecognized tag
 * (`UNKNOWN`, or a backend not in the fast set — e.g. `CUDA`) routes to the
 * backend's virtual `destroy`, so dispatch is correct regardless.
 */
enum class backend_tag : std::uint8_t {
    UNKNOWN = 0,     /**< @brief No fast-path tag → virtual `destroy` fallback. */
    HEAP,            /**< @brief `mem_heap` (mem_heap.hpp). */
    POOL,            /**< @brief `mem_pool` (mem_pool.hpp). */
    BORROWED,        /**< @brief `mem_borrowed` (mem_borrowed.hpp). */
    BORROWED_DEVICE, /**< @brief `mem_borrowed` device-space variant. */
    CUDA,            /**< @brief `mem_cuda` (device; dispatched via the virtual fallback). */
};

/**
 * @brief A memory backend: the L0 seam libtracer binds any substrate behind.
 *
 * Subclass this to bind libtracer to any allocator — a heap, a fixed
 * caller-owned arena, live registers, lwIP pbufs, DMA descriptors. The
 * interface deliberately does not make allocation mandatory: many substrates
 * cannot allocate (MMIO, hardware FIFOs), so @ref alloc may return `nullptr`.
 *
 * @note Each backend declares its own concurrency/ISR-safety contract; the
 *       protocol mandates none (docs/adr/0012).
 */
class mem_backend_t {
   public:
    /** @brief Construct a backend with a stable, human-readable @p name (e.g. "mem_heap"). */
    explicit mem_backend_t(const char* name) noexcept : name_(name) {}
    virtual ~mem_backend_t() = default;

    mem_backend_t(const mem_backend_t&) = delete;
    mem_backend_t& operator=(const mem_backend_t&) = delete;

    /**
     * @brief Allocate a fresh segment of at least @p size bytes (refcount = 1).
     *
     * The returned segment is the caller's to adopt via
     * `tr::view::segment_ptr_t::adopt`. A **raw** `segment_t*` is returned, not
     * a `segment_ptr_t`, to keep L0 from naming L1's owning handle
     * (docs/adr/0016 §2). Allocation-incapable substrates (MMIO, FIFOs) leave
     * this default and return `nullptr`.
     *
     * @param hint     Backend-private allocation hint; `NONE` for "don't care".
     * @retval nullptr Backpressure (pool exhausted / OOM) or allocation unsupported.
     */
    [[nodiscard]] virtual view::segment_t* alloc(
        [[maybe_unused]] std::size_t size,
        [[maybe_unused]] alloc_hint_t hint = alloc_hint_t::NONE) {
        return nullptr;
    }

    /**
     * @brief Reclaim a segment whose refcount has reached zero (the only reclaim path).
     *
     * Frees whatever the backend owns (the bytes and/or the `segment_t` control
     * block) and nothing it does not — a borrowed backend never frees the
     * user's bytes. Invoked by `segment_ptr_t` at zero, never by user code.
     * @warning Never called on a live segment.
     */
    virtual void destroy(view::segment_t* seg) noexcept = 0;

    /**
     * @brief Cache prep *before* handing the segment to a DMA transfer.
     *
     * Clean or invalidate per @p dir so the device sees coherent memory. No-op
     * by default and on cacheless cores (Cortex-M0/M3/M4); only DMA-class
     * backends override it (docs/reference/09 §cache coherency).
     */
    virtual void before_io(view::segment_t* /*seg*/, io_dir_t /*dir*/) noexcept {}

    /**
     * @brief Cache reconcile *after* a DMA transfer completes.
     *
     * Invalidate per @p dir so the next CPU reader sees HW's writes. No-op by
     * default and on cacheless cores.
     */
    virtual void after_io(view::segment_t* /*seg*/, io_dir_t /*dir*/) noexcept {}

    /** @brief The alignment (bytes) this backend guarantees for allocated bytes. */
    [[nodiscard]] virtual std::size_t alignment() const noexcept {
        return alignof(std::max_align_t);
    }
    /** @brief The largest single segment this backend can produce. */
    [[nodiscard]] virtual std::size_t max_segment_size() const noexcept { return ~std::size_t{0}; }

    /**
     * @brief The address space this backend's segments live in (default `HOST`).
     *
     * A `DEVICE` backend (e.g. `mem_cuda`) must override this; segments inherit
     * it (segment.hpp), and the codec uses it to skip CPU access to device links.
     */
    [[nodiscard]] virtual mem_space_t space() const noexcept { return mem_space_t::HOST; }

    /**
     * @brief The build-time-closed module-set tag (default `UNKNOWN`, ADR-0047 §2).
     *
     * A backend that participates in the fast destroy dispatch overrides this to
     * return its @ref backend_tag; segments read it once at construction (like
     * @ref space). A backend that leaves the default is dispatched through its
     * virtual `destroy`.
     */
    [[nodiscard]] virtual backend_tag tag() const noexcept { return backend_tag::UNKNOWN; }

    /** @brief The backend's stable identifier (e.g. for introspection / metrics). */
    [[nodiscard]] const char* name() const noexcept { return name_; }

   private:
    const char* name_;
};

/**
 * @brief Reclaim @p seg through its backend — the module-set destroy dispatch
 *        (ADR-0047 §2), called by `segment_ptr_t::reset` at refcount zero.
 *
 * Switches on the segment's @ref backend_tag to a devirtualized direct call for a
 * linked fast-set backend, and falls back to the backend's virtual `destroy` for
 * any other tag, so the result is identical to `seg->backend->destroy(seg)` for
 * every backend. Defined in `backend_set.cpp` (the one TU that sees the concrete
 * backend types), keeping this L0 seam free of an upward dependency.
 */
void destroy_dispatch(view::segment_t* seg) noexcept;

/**
 * @brief Move @p host.size() bytes between segment @p seg and host memory
 *        @p host in direction @p dir — the module-set host↔device transfer
 *        (ADR-0047 §2), bracketed by the backend's cache hooks.
 *
 * The single tag-dispatched byte-mover the codec routes a copy through,
 * generalizing (and retiring) the CUDA-named `cuda_copy_from_host` /
 * `cuda_copy_to_host` free functions:
 * - `io_dir_t::CPU_TO_DEVICE` copies @p host **into** @p seg (host is the source);
 * - `io_dir_t::DEVICE_TO_CPU` copies @p seg **out to** @p host (host is the sink).
 *
 * A host-addressable backend transfers with a `memcpy`, bracketed by
 * `before_io`/`after_io` only when its `static constexpr needs_cache_ops` trait
 * is set — so a cacheless backend (every one today) folds the hooks away at
 * compile time (they are the traits' first in-tree consumer, review finding #8).
 * A `DEVICE`-space backend (`mem_cuda`, built only with `LIBTRACER_WITH_CUDA`)
 * routes to its device copy — where `after_io` gets its first caller (the CUDA
 * stream barrier). Defined in `backend_set.cpp` (the module-set TU).
 *
 * @param seg  The segment to read from or write to; `nullptr` yields `false`.
 * @param host CPU-addressable bytes; a `.size()` larger than @p seg's yields `false`.
 * @param dir  Which way the bytes move (also the cache-hook direction).
 * @retval false Null segment, an over-long @p host, or a device copy failure.
 */
[[nodiscard]] bool transfer(view::segment_t* seg, std::span<std::byte> host, io_dir_t dir) noexcept;

}  // namespace tr::mem
