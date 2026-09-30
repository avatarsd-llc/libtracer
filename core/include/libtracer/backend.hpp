/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The L0 memory-backend seam: the small, user-implementable interface every
 * substrate implements (heap, borrowed/live, fixed pool — and, later, DMA,
 * lwIP pbuf, SHM). libtracer never allocates on its own; it receives memory
 * from a backend, refcounts it (`%segment.hpp`), and casts the bytes to TLVs.
 * Each backend owns and declares its own per-architecture concurrency/coherency
 * contract — the protocol mandates none. See docs/reference/09-memory-substrate.md,
 * docs/adr/0012 (modular memory binding; transparent byte router) and
 * docs/adr/0016 (layer namespaces; no templates through the seam).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "libtracer/mem_source.hpp"

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
 * `HOST` bytes are CPU-addressable; `DEVICE` bytes are not (e.g. GPU/accelerator
 * device memory — docs/adr/0024). The codec must never CPU-dereference a `DEVICE` link:
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
 * (`UNKNOWN`, or any backend outside the fast set — every out-of-core device
 * backend is) routes to the backend's virtual `destroy`, so dispatch is correct
 * regardless.
 *
 * The set is closed over the backends `core/` itself compiles. A vendor backend
 * from the `backends/` tier does **not** get an enumerator: it is identified by
 * its `mem_backend_t` object (@ref register_device_backend), which is what
 * `destroy` already routes on, so core never has to name it.
 */
enum class backend_tag : std::uint8_t {
    UNKNOWN = 0,     /**< @brief No fast-path tag → virtual `destroy` fallback. */
    HEAP,            /**< @brief `mem_heap` (`%mem_heap.hpp`). */
    POOL,            /**< @brief `mem_pool` (`%mem_pool.hpp`). */
    BORROWED,        /**< @brief `mem_borrowed` (`%mem_borrowed.hpp`). */
    BORROWED_DEVICE, /**< @brief `mem_borrowed` device-space variant. */
};

/**
 * @brief A memory backend: a @ref block_source_t that also vends refcounted segments — the
 *        L0 seam libtracer binds any substrate behind (RFC-0028 §4.9, D9).
 *
 * A backend **is** a block source (RFC-0028 slice 10): the raw failable block
 * (@ref block_source_t::try_alloc / @ref block_source_t::release) is the one allocation seam,
 * and a backend is that seam plus a refcounted segment and the space / cache hooks device
 * memory needs (ADR-0024). The default @ref alloc draws ONE block through @ref try_alloc and
 * places the @ref view::segment_t header at its head, the payload after it; the default
 * @ref destroy hands that one block back through a sized @ref release. So a backend that
 * implements the two block virtuals gets segments for free, and a deployer that injects one
 * slab has one slab — every injection point that takes a `block_source_t&` accepts a backend.
 *
 * Subclass this to bind libtracer to any allocator — a heap, a fixed caller-owned arena, live
 * registers, lwIP pbufs, DMA descriptors. Allocation stays optional: many substrates cannot
 * allocate (MMIO, hardware FIFOs), and the default @ref try_alloc refuses, so the default
 * @ref alloc returns `nullptr` for them. A substrate whose segment does not live in one block
 * (a borrowed span, device memory with a host-side header) overrides @ref alloc and
 * @ref destroy directly.
 *
 * @note Each backend declares its own concurrency/ISR-safety contract; the
 *       protocol mandates none (docs/adr/0012).
 */
class mem_backend_t : public block_source_t {
   public:
    /** @brief Construct a backend with a stable, human-readable @p name (e.g. "mem_heap"). */
    explicit mem_backend_t(const char* name) noexcept : block_source_t(name) {}

    /**
     * @brief Obtain one raw block — the @ref block_source_t half of a backend.
     *
     * The default refuses, which is right for an allocation-incapable substrate (a borrowed
     * span, MMIO, a hardware FIFO): its @ref alloc then refuses too.
     * @retval nullptr Exhaustion, or a substrate that cannot allocate.
     */
    [[nodiscard]] void* try_alloc(
        [[maybe_unused]] std::size_t bytes,
        [[maybe_unused]] std::size_t align = alignof(std::max_align_t)) noexcept override {
        return nullptr;
    }

    /** @brief Return a block @ref try_alloc handed out. The default has nothing to return. */
    void release([[maybe_unused]] void* p, [[maybe_unused]] std::size_t bytes,
                 [[maybe_unused]] std::size_t align = alignof(std::max_align_t)) noexcept override {
    }

    /**
     * @brief Allocate a fresh segment of at least @p size bytes (refcount = 1).
     *
     * The returned segment is the caller's to adopt via
     * `tr::view::segment_ptr_t::adopt`. A **raw** `segment_t*` is returned, not
     * a `segment_ptr_t`, to keep L0 from naming L1's owning handle
     * (docs/adr/0016 §2).
     *
     * The default is ONE block: @ref try_alloc for the header padded to @ref alignment plus
     * @p size, the header placed at the block's head (`%segment.hpp`). One allocation where
     * the pre-slice-10 heap backend made two.
     *
     * @param hint     Backend-private allocation hint; `NONE` for "don't care".
     * @retval nullptr Backpressure (pool exhausted / OOM) or allocation unsupported.
     */
    [[nodiscard]] virtual view::segment_t* alloc(
        std::size_t size, [[maybe_unused]] alloc_hint_t hint = alloc_hint_t::NONE);

    /**
     * @brief Reclaim a segment whose refcount has reached zero (the only reclaim path).
     *
     * Frees whatever the backend owns (the bytes and/or the `segment_t` control
     * block) and nothing it does not — a borrowed backend never frees the
     * user's bytes. Invoked by `segment_ptr_t` at zero, never by user code. The default is
     * the mirror of the default `alloc()`: one sized @ref release of the whole block.
     * @warning Never called on a live segment.
     */
    virtual void destroy(view::segment_t* seg) noexcept;

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
     * A `DEVICE` backend (one from the `backends/` tier) must override this;
     * segments inherit it (`%segment.hpp`), and the codec uses it to skip CPU
     * access to device links.
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

   protected:
    /**
     * @brief The one-block segment layout: draw `header + size` bytes at @p align through
     *        @ref try_alloc and place the header at the head. Defined in `%segment.hpp`.
     *
     * Non-virtual so a concrete backend's own @ref alloc can reuse the layout with a
     * compile-time @p align and no virtual call on its hot path.
     */
    [[nodiscard]] view::segment_t* alloc_in_block(std::size_t size, std::size_t align) noexcept;

    /** @brief The mirror of `alloc_in_block()`: one sized @ref release of the whole block. */
    void destroy_in_block(view::segment_t* seg, std::size_t align) noexcept;
};

/**
 * @brief The device byte-move a `DEVICE`-space backend registers with
 *        @ref register_device_backend — @ref transfer's out-of-core arm.
 *
 * Same contract as @ref transfer, narrowed to one backend's segments: move
 * `host.size()` bytes between @p seg and @p host in direction @p dir, `false` on
 * refusal. A plain function pointer, not a `std::function`: the seam must cost a
 * pointer and never allocate (ADR-0047 §2).
 */
using device_transfer_fn_t = bool (*)(view::segment_t* seg, std::span<std::byte> host,
                                      io_dir_t dir) noexcept;

/**
 * @brief Register @p fn as the byte-mover @ref transfer routes @p backend's
 *        `DEVICE`-space segments through.
 *
 * The L0 mirror of `tr::net::transport_vertex_t::register_transport_type`: a
 * module outside core supplies the value, the composition root wires it in, and
 * core never names the module (docs/adr/0024 Amendment 1; the module seam is
 * docs/adr/0043 §1). The `backends/` tier holds the first in-tree caller.
 *
 * Keyed by the **backend object**, not by @ref mem_space_t — `DEVICE` is one
 * enumerator shared by every accelerator, and the segment's `backend` pointer is
 * already the identity `destroy` routes on — so a second vendor plugs in without
 * adding a name to core.
 *
 * Bounded and allocation-free: the table holds @ref tr::mem::kDeviceBackendSlots
 * entries (`%config.hpp`), so registration cannot fail for lack of heap, only for
 * lack of a slot. Registering the same @p backend twice **replaces** its hook
 * (`insert_or_assign` semantics), so a backend and its hook can never disagree.
 *
 * @note Call at setup, before frames flow, from one thread — the same contract
 *       `register_transport_type` carries. Concurrent *lookups* by @ref transfer
 *       are safe against a completed registration.
 * @retval false @p fn was null, or the table is full — nothing was registered.
 */
[[nodiscard]] bool register_device_backend(const mem_backend_t& backend,
                                           device_transfer_fn_t fn) noexcept;

namespace detail {

/**
 * @brief Look @p seg's backend up in the @ref register_device_backend table and
 *        run its hook — @ref transfer's whole `DEVICE`-space arm.
 *
 * Defined in `device_backend.cpp`, a TU of its own so that a single-backend
 * (`LIBTRACER_BACKEND_SET_POOL_ONLY`) target, which has no device arm at all,
 * never links the table.
 * @retval false Nothing is registered for that backend (the pre-registry
 *               behaviour for every unrecognized `DEVICE` segment), or the
 *               registered hook refused.
 */
[[nodiscard]] bool device_transfer(view::segment_t* seg, std::span<std::byte> host,
                                   io_dir_t dir) noexcept;

}  // namespace detail

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
 * The single tag-dispatched byte-mover the codec routes a copy through, which
 * replaced the vendor-named per-device copy pair the module set retired:
 * - `io_dir_t::CPU_TO_DEVICE` copies @p host **into** @p seg (host is the source);
 * - `io_dir_t::DEVICE_TO_CPU` copies @p seg **out to** @p host (host is the sink).
 *
 * A host-addressable backend transfers with a `memcpy`, bracketed by
 * `before_io`/`after_io` only when its `static constexpr needs_cache_ops` trait
 * is set — so a cacheless backend (every one today) folds the hooks away at
 * compile time (they are the traits' first in-tree consumer, review finding #8).
 * A `DEVICE`-space segment takes the registry arm instead: its backend's
 * @ref register_device_backend hook, or `false` when nothing is registered for it
 * — no host arm ever sees a pointer the CPU may not dereference (#928). That is
 * where `after_io` gets its first caller, in the `backends/` tier module that owns
 * the device copy (docs/adr/0024). Defined in `backend_set.cpp` (the module-set TU).
 *
 * @param seg  The segment to read from or write to; `nullptr` yields `false`.
 * @param host CPU-addressable bytes; a `.size()` larger than @p seg's yields `false`.
 * @param dir  Which way the bytes move (also the cache-hook direction).
 * @retval false Null segment, an over-long @p host, or a device copy failure.
 */
[[nodiscard]] bool transfer(view::segment_t* seg, std::span<std::byte> host, io_dir_t dir) noexcept;

}  // namespace tr::mem

// The segment type, and the inline bodies of the one-block defaults above, which need its
// size. Included LAST so `%segment.hpp` (which includes this header first) sees the class.
#include "libtracer/segment.hpp"
