/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_heap — the host allocator backend. Owns malloc'd bytes; frees them and
 * the segment_t control block on destroy. The week-1 MVP backend for hosted
 * targets (docs/reference/09-memory-substrate.md §mem_heap).
 */
#pragma once

#include <cstddef>
#include <cstring>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "libtracer/backend.hpp"
#include "libtracer/config.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/placement.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief The `mem_heap` L0 backend (`tr::mem`) and its L1 alloc helper (`tr::view`),
 *        plus the nothrow `std::vector` growth primitives (`tr::detail`).
 */

namespace tr::detail {

// The test-only OOM-injection seam (`probe_fail_hook` / `probe_hook_ok`) lives one layer
// down in mem_source.hpp, so the process-default `heap_source_t` can honour it too.

/**
 * @brief Probe whether a @p bytes-sized heap allocation would succeed, nothrow — the ONE
 *        locus of the nothrow `operator new` probe.
 *
 * Factored out of the `try_*` growth templates so the `new`/`delete` pair is emitted ONCE,
 * not duplicated into every `T` instantiation (the esp32c6 footprint sentinel).
 *
 * @warning A probe is an ANSWER ABOUT THE PAST. The block it tests is freed before this
 *          returns, so "the following allocation will succeed" is only true while nothing
 *          else can take it — see @ref try_grow, which is why the growth helpers no longer
 *          rely on that inference on a profile that can catch (#923). Remaining callers use
 *          it as a *pressure gauge* ahead of work they are willing to skip, not as a
 *          guarantee for an allocation they cannot fail.
 * @retval false The allocation would fail (OOM) — nothing was allocated.
 */
[[nodiscard]] inline bool probe_bytes(std::size_t bytes) noexcept {
    if (!probe_hook_ok(bytes)) return false;  // test-only OOM injection
    void* p = ::operator new(bytes, std::nothrow);
    if (p == nullptr) return false;
    ::operator delete(p);
    return true;
}

/**
 * @brief DO NOT generalize the `try_*` helpers below to `std::pmr::vector`.
 *
 * It is the obvious next step — the templates differ only in the allocator parameter — and
 * it is wrong. On the `-fno-exceptions` profile the reason is the probe: @ref probe_bytes
 * tests the **global heap** while a `std::pmr` container allocates from its **injected
 * resource**, so it would answer "yes" about memory the container will never touch, the real
 * allocation would still throw, and that is the `abort()` these helpers exist to prevent.
 *
 * The failure mode is worse than a plain bug because it hides: under the default resource the
 * two allocators ARE the global heap, so the generalization looks correct in most tests. It
 * breaks only on a node whose resource is a slab with a null upstream — the configuration
 * `route_handle_test`'s "slab resource (null upstream — zero global heap)" case exists to
 * cover, and the one an MCU actually ships. The `probe_fail_hook` seam is equally misdirected
 * there: it gates the global-heap probe, not the injected resource.
 *
 * A `std::pmr` container cannot be made failable this way at all: `polymorphic_allocator`
 * reports exhaustion by throwing, which is the whole reason
 * [ADR-0065](../../../docs/adr/0065-failable-allocation-gets-its-own-seam-block-source.md)
 * introduced `tr::mem::block_source_t` (`try_alloc` returns `nullptr`) as a separate seam
 * rather than widening this one. A pmr-backed structure that must survive exhaustion migrates
 * to that seam; it does not get a `try_reserve` overload.
 *
 * @note **What IS generalized, and why the objection above does not reach it (#873 phase 1).**
 *       The helpers take any allocator, and @ref try_grow_from probes the block source a
 *       growth actually draws from with `try_alloc`/`release` instead of the global heap. Every
 *       sentence above turns on the probe testing a DIFFERENT allocator from the one the growth
 *       uses. The `-fno-exceptions` probe is still probe-then-commit and still carries the #850
 *       race window — what it stops being is an answer about the wrong memory.
 *       `probe_fail_hook` is honoured on both arms, so the OOM-injection seam still reaches
 *       these paths. (The std-`Allocator` adapter the graph's growth sites used for this,
 *       `source_allocator_t`, was retired in #1783 once core containers replaced them.)
 */

/**
 * @brief The capacity-doubling grow target for a full vector (min 1) — a non-template
 *        helper so the size math is not duplicated per `try_push_back<T>`.
 */
[[nodiscard]] inline std::size_t grow_capacity(std::size_t cap) noexcept {
    return cap == 0 ? 1u : cap * 2u;
}

/**
 * @brief True on a build whose growth failures are reported by a throw this header can
 *        catch (every hosted profile); false under the MCU profile's `-fno-exceptions`.
 */
#if defined(__cpp_exceptions) && __cpp_exceptions
#define LIBTRACER_GROWTH_IS_CATCHABLE 1
#else
#define LIBTRACER_GROWTH_IS_CATCHABLE 0
#endif

/**
 * @brief Run a THROWING container growth @p grow and answer its failure by value — the ONE
 *        locus that turns an allocation failure into `false` for the `try_*` helpers (#923).
 *
 * @par Why this is not a probe
 * The helpers used to `probe_bytes(bytes)` and then run the throwing grow, on the argument
 * that "the just-freed probe block satisfies it." That inference is single-threaded. The
 * probe block is freed BEFORE the grow allocates, and this library's own concurrency model
 * puts other threads on the same global heap (a segment self-routes its reclaim on whatever
 * thread drops the last ref; transport receive threads run concurrent with writers). Near
 * exhaustion — the regime these helpers exist for — a racer takes the block in the window and
 * the grow throws `bad_alloc` out of a `noexcept` function: `std::terminate`, measured (#850,
 * #923). Nor does it need SMP: a FreeRTOS context switch between the `operator delete` and
 * the `reserve` opens the same window on one core.
 *
 * So the grow's OWN allocation is the one whose failure is handled — there is no second
 * allocation, hence no window to lose. The `catch` sits inside a `noexcept` caller by
 * design: the exception is consumed here, and nothing crosses the boundary.
 *
 * @par The `-fno-exceptions` profile
 * There the failure has no representation at all — libstdc++ turns the `bad_alloc` into a
 * bare `abort()` inside `reserve`, and no wrapper can intercept it. The probe is retained as
 * the only available guard, and it is sound only to the extent that profile is single-
 * threaded. A path on that profile that must genuinely survive exhaustion does not get a
 * better `try_reserve`: it migrates to the failable seam (`tr::mem::block_source_t` /
 * `block_array_t`, ADR-0065), whose growth is ONE refusable `try_alloc` with no second step
 * — the move `transport_t::send(iov)` and `ws::try_encode_client_frame` already made.
 *
 * @param bytes The byte count the growth will request; the subject of @ref probe_fail_hook
 *              (so the OOM-injection seam still reaches these paths) and of the probe on the
 *              exception-free profile.
 * @param grow  The throwing growth. It must leave its container UNCHANGED when it throws —
 *              `std::vector::reserve` and `std::basic_string::assign` both do.
 * @retval false The growth allocation failed — the container is unchanged.
 */
template <class F>
[[nodiscard]] inline bool try_grow(std::size_t bytes, F&& grow) noexcept {
#if LIBTRACER_GROWTH_IS_CATCHABLE
    if (!probe_hook_ok(bytes)) return false;  // test-only OOM injection
    try {
        grow();
    } catch (...) {
        return false;  // bad_alloc / length_error — consumed here, never crosses the noexcept
    }
    return true;
#else
    if (!probe_bytes(bytes)) return false;
    grow();
    return true;
#endif
}

/**
 * @brief The @ref try_grow twin whose `-fno-exceptions` probe tests @p src rather than the
 *        global heap (#873 phase 1).
 *
 * Identical on a profile that can catch — the growth's own allocation is the one whose failure
 * is reported, and where the bytes came from is irrelevant to that. The difference is the
 * exception-free arm: probing the global heap for a growth that will draw from an injected
 * store answers about memory the container will never touch, which is precisely the defect
 * `%mem_heap.hpp`'s standing warning above describes for `std::pmr`. Here the store is
 * reachable, so the probe asks it.
 *
 * @param src   The store the growth will draw from; probed with `try_alloc`/`release`.
 *              `[[maybe_unused]]` because only the exception-free arm probes it — the
 *              catchable arm just runs @p grow and catches, so every host TU (where
 *              `LIBTRACER_GROWTH_IS_CATCHABLE` is on) would otherwise emit
 *              `-Wunused-parameter` on instantiation (#1601).
 * @param bytes The byte count the growth will request.
 * @param grow  The throwing growth; it must leave its container unchanged when it throws.
 * @retval false The growth allocation failed — the container is unchanged.
 */
template <class F>
[[nodiscard]] inline bool try_grow_from([[maybe_unused]] mem::block_source_t& src,
                                        std::size_t bytes, F&& grow) noexcept {
#if LIBTRACER_GROWTH_IS_CATCHABLE
    if (!probe_hook_ok(bytes)) return false;  // test-only OOM injection
    try {
        grow();
    } catch (...) {
        return false;
    }
    return true;
#else
    if (!probe_hook_ok(bytes)) return false;  // test-only OOM injection
    void* const p = src.try_alloc(bytes);
    if (p == nullptr) return false;
    src.release(p, bytes);
    grow();
    return true;
#endif
}

/**
 * @brief The store a growth of @p v will draw from, when its allocator publishes one.
 *
 * The two-overload dispatch that keeps the source-aware probe out of the default-allocator
 * path entirely: a plain `std::vector<T>` picks the `nullptr` overload and @ref try_reserve
 * routes to @ref try_grow exactly as before, so nothing about the pre-#873 codegen changes.
 */
template <class Alloc>
[[nodiscard]] constexpr mem::block_source_t* growth_source(const Alloc&) noexcept {
    return nullptr;
}

/**
 * @brief Nothrow `std::vector::reserve`: grow @p v to hold at least @p n elements
 *        WITHOUT ever aborting.
 *
 * Routes the throwing `reserve` through @ref try_grow, so the allocation whose failure is
 * reported is the one the vector actually performs — no probe-then-commit window (#923).
 * @retval false Allocation would fail / @p n is impossible — nothing changed.
 * @retval true  @p v now has capacity for at least @p n elements.
 */
template <class T, class Alloc>
[[nodiscard]] bool try_reserve(std::vector<T, Alloc>& v, std::size_t n) noexcept {
    if (n <= v.capacity()) return true;
    if (n > v.max_size()) return false;  // impossible count — the reserve would throw length_error
    const auto grow = [&v, n] { v.reserve(n); };
    if (mem::block_source_t* const src = growth_source(v.get_allocator()); src != nullptr) {
        return try_grow_from(*src, n * sizeof(T), grow);
    }
    return try_grow(n * sizeof(T), grow);
}

/**
 * @brief Nothrow `std::vector::push_back`: append @p x, growing (capacity-doubling)
 *        through @ref try_reserve so no reallocation can abort under `-fno-exceptions`.
 *
 * For a vector whose element count is not known up front (the composed-read pre-order
 * collection stacks) — where @ref try_reserve cannot be called once with the final
 * count. @p x is appended only when the growth (if any) succeeds; the just-reserved
 * capacity guarantees the `push_back` itself never reallocates, so the only allocation in
 * play is @ref try_reserve's — which reports its failure by value (#923).
 * @retval false The growth allocation failed — @p x was NOT appended.
 */
template <class T, class Alloc>
[[nodiscard]] bool try_push_back(std::vector<T, Alloc>& v, T&& x) noexcept {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "the in-capacity push_back must not be able to throw out of this noexcept");
    if (v.size() == v.capacity() && !try_reserve(v, grow_capacity(v.capacity()))) return false;
    v.push_back(std::move(x));  // guaranteed no reallocation now
    return true;
}

/**
 * @brief Nothrow byte-vector copy-assign: replace @p dst's contents with @p src WITHOUT
 *        ever aborting — @ref try_reserve then an in-capacity `assign`.
 *
 * Non-template (the one element type the store/delivery path copies is `std::byte` —
 * vertex keys, route records), per the footprint-sentinel discipline.
 * @retval false The growth allocation failed — @p dst is unchanged.
 */
[[nodiscard]] inline bool try_assign(std::vector<std::byte>& dst,
                                     std::span<const std::byte> src) noexcept {
    if (!try_reserve(dst, src.size())) return false;
    dst.assign(src.begin(), src.end());  // within capacity — no reallocation
    return true;
}

/**
 * @brief Nothrow string copy-assign: replace @p dst's contents with @p src WITHOUT ever
 *        aborting.
 *
 * A fitting copy (SSO or existing capacity) assigns directly; a growing one routes the
 * throwing `assign` through @ref try_grow and soft-fails instead. `basic_string` has the
 * strong guarantee, so a refused growth leaves @p dst exactly as it was.
 * @retval false The growth allocation failed — @p dst is unchanged.
 */
[[nodiscard]] inline bool try_assign(std::string& dst, std::string_view src) noexcept {
    if (src.size() <= dst.capacity()) {
        dst.assign(src);  // fits — no allocation, nothing to fail
        return true;
    }
    return try_grow(src.size() + 1, [&dst, src] { dst.assign(src); });  // +1: the NUL
}

}  // namespace tr::detail

namespace tr::mem {

namespace detail {

/**
 * @brief One block of the VALUE sub-pool of the host root (`%mem_slab_pool.hpp`, #1777), from
 *        this thread's cache; `nullptr` when the root refused a slab.
 *
 * Out of line: the per-thread cache lives in `mem_heap.cpp`. Called only where
 * `tr::mem::kSlabPool` is `true`.
 */
[[nodiscard]] void* host_value_alloc(std::size_t bytes, std::size_t align) noexcept;

/** @brief Return a block @ref host_value_alloc handed out, sized as asked. */
void host_value_release(void* p, std::size_t bytes, std::size_t align) noexcept;

/**
 * @brief One block of the VALUE sub-pool of the MCU static arena (`%mem_arena.hpp`, #1783);
 *        `nullptr` when the arena is spent. Called only where `tr::mem::kSlabPool` is `false`.
 */
[[nodiscard]] void* mcu_value_alloc(std::size_t bytes, std::size_t align) noexcept;

/** @brief Return a block @ref mcu_value_alloc handed out, sized as asked. */
void mcu_value_release(void* p, std::size_t bytes, std::size_t align) noexcept;

/**
 * @brief The per-value draw of this build: the host root's value sub-pool where
 *        `tr::mem::kSlabPool` is `true`, the MCU arena's otherwise. Never the platform heap.
 */
[[nodiscard]] inline void* value_block_alloc(std::size_t bytes, std::size_t align) noexcept {
    if constexpr (kSlabPool) {
        return host_value_alloc(bytes, align);
    } else {
        return mcu_value_alloc(bytes, align);
    }
}

/** @brief The sized return of @ref value_block_alloc. */
inline void value_block_release(void* p, std::size_t bytes, std::size_t align) noexcept {
    if constexpr (kSlabPool) {
        host_value_release(p, bytes, align);
    } else {
        mcu_value_release(p, bytes, align);
    }
}

}  // namespace detail

/**
 * @brief The process-default per-value backend: owns its blocks, returns them and the
 *        `segment_t` control block on destroy.
 *
 * Exposed here (rather than TU-local) so the module-set destroy dispatch
 * (backend_set.cpp, ADR-0047 §2) can devirtualize its release; a `final` class,
 * so the qualified call in that switch is a direct call.
 *
 * @par Where the bytes come from (#1777)
 * Every block is drawn by `detail::value_block_alloc`: on a host build (`kSlabPool`), the value
 * sub-pool of the host root (`%mem_slab_pool.hpp`), from this thread's cache, so the platform
 * allocator is asked for whole slabs and never for a segment; elsewhere the value sub-pool of
 * the MCU static arena (`%mem_arena.hpp`), with no heap at all. Both are direct calls, not the
 * virtual draw #873 phase 2 measured at +22.7 % on the hazard domain: this backend is the process
 * default on the hottest allocation path in the library.
 *
 * @par How many draws a segment costs (RFC-0028 §4.9, #1777)
 * ONE: the padded header and the payload share a block (RFC-0028 slice 10), at every size. The
 * two-block split above glibc's 1,032 B tcache ceiling (#1768) served only the per-value heap
 * draw this backend no longer makes on a host. The slab pool's classes have no such cliff: a
 * 1024 B value's 1072 B block is one 1152 B class block, from the same cache a 1000 B one comes
 * from. `bench_forward_heap`'s `allocs=` pins count the small-value case.
 */
class heap_backend_t final : public mem_backend_t {
   public:
    heap_backend_t() noexcept : mem_backend_t("mem_heap") {}

    /** @brief The block alignment a heap segment is drawn at. */
    static constexpr std::size_t kBlockAlign = segment_block_align(alignof(std::max_align_t));

    /** @brief One block of this build's per-value draw — nothrow. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return detail::value_block_alloc(bytes, align);
    }
    /** @brief Return a block @ref try_alloc drew, sized. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        detail::value_block_release(p, bytes, align);
    }

    /**
     * @brief A segment is ONE block, the header and the payload together (RFC-0028 §4.9).
     *
     * One block is the `producer-own` row of `bench_lean_value_path` (slice 10). Draws straight
     * from `detail::value_block_alloc` rather than through the virtual @ref try_alloc, so the
     * hot path pays no virtual call.
     */
    view::segment_t* alloc(std::size_t size, alloc_hint_t /*hint*/) override {
        void* const block =
            detail::value_block_alloc(segment_block_bytes(size, kBlockAlign), kBlockAlign);
        return block != nullptr ? place_segment(this, block, size, kBlockAlign) : nullptr;
    }

    /** @brief Return the block @ref alloc drew — sized, as drawn. */
    void destroy(view::segment_t* seg) noexcept override {
        const std::size_t size = seg->bytes.size();
        seg->~segment_t();
        detail::value_block_release(seg, segment_block_bytes(size, kBlockAlign), kBlockAlign);
    }

    /** @brief The payload follows a header padded to @ref kBlockAlign. */
    [[nodiscard]] std::size_t alignment() const noexcept override { return kBlockAlign; }

    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::HEAP; }

    // Module-set traits (ADR-0047 §2). `needs_cache_ops` is read by `mem::transfer`.
    static constexpr bool needs_cache_ops =
        false; /**< @brief No DMA cache maintenance (host RAM). */
    static constexpr bool is_isr_safe =
        false; /**< @brief A class lock or a slab draw from the heap — not ISR-safe. */
    static constexpr bool is_nonblocking =
        false; /**< @brief A class lock or a slab draw may wait or syscall (#928). */
    static constexpr bool owns_bytes = true; /**< @brief Owns its blocks — durably storable. */
};

/**
 * @brief The process-DEFAULT per-value backend (function-local static — no init-order trap).
 *
 * @warning The name is historical: this is NOT "the system heap" on every build. It draws
 *          through `detail::value_block_alloc`, so it is the host root's value sub-pool
 *          when `config_t::kSlabPool` is `true` (a host build; the platform allocator is
 *          asked for whole slabs only) and the static ARENA's value sub-pool when it is
 *          `false` (an MCU build; no heap at all, and an exhausted arena refuses). An app on
 *          an MCU that injects `heap_source()` and hands `heap_backend()` in as a fallback
 *          therefore fills the arena, not the heap. To send values to the platform heap on
 *          either build, wrap `heap_source()`:
 *          `static tr::mem::source_backend_t heap_values{tr::mem::heap_source()};`
 *          (@ref source_backend_t, one block per segment) and pass that.
 */
[[nodiscard]] mem_backend_t& heap_backend() noexcept;

/**
 * @brief The default root a `graph_t` takes when it is handed no source (ADR-0083 Decision 4,
 *        #1777, #1783): the host root (`%mem_slab_pool.hpp`) where `kSlabPool` is `true`, the
 *        MCU static arena (`%mem_arena.hpp`) otherwise.
 */
[[nodiscard]] block_source_t& default_root() noexcept;

/**
 * @brief The default VALUE sub-pool (`:stats.mem.values`): the host root's, from a per-thread
 *        cache, where `kSlabPool` is `true`; the MCU arena's otherwise.
 *
 * The source a value made outside any graph draws from (`tr::graph::value_ref_t::make`), and
 * the one @ref heap_backend draws its segments from.
 */
[[nodiscard]] block_source_t& value_source() noexcept;

/** @brief The default TABLE sub-pool (`:stats.mem.tables`), on the same terms as
 *         @ref value_source. */
[[nodiscard]] block_source_t& table_source() noexcept;

/**
 * @brief The default NET sub-pool (`:stats.mem.net`), on the same terms as @ref value_source, that
 * is: the block source a router or link draws from when the application injects none of its own
 * (ADR-0083 Q21).
 */
[[nodiscard]] block_source_t& net_source() noexcept;

/**
 * @brief The segment backend over @ref net_source, the default receive, flatten and egress
 *        backend of the router and the links when the application injects none (Q21).
 *
 * On the MCU arena (`kSlabPool` `false`) it is the same adapter over the arena's net
 * sub-pool.
 */
[[nodiscard]] mem_backend_t& net_backend() noexcept;

}  // namespace tr::mem

namespace tr::view {

/**
 * @brief Allocate a fresh, owned segment of @p size bytes from @p backend, wrapped in an
 *        adopting `segment_ptr_t` — the backend-taking form @ref heap_alloc is one call of
 *        (#793).
 *
 * An L1 helper (it produces an owning handle), so it lives in `tr::view`, not `tr::mem`
 * (docs/adr/0016 §2). It exists so an ownership COPY on a peer-driven path can draw from the
 * node's injected byte seam instead of the global heap — the rope-tier `own_wire`'s
 * single-link branch was the first such copy converted (#793), the span-tier
 * `arena_node::own_wire` the second (#801); the multi-link branch beside the former already
 * flattened through the injection (#766).
 * @retval {} An empty handle on allocation failure (the backend refused).
 */
[[nodiscard]] segment_ptr_t segment_alloc(mem::mem_backend_t& backend, std::size_t size);

/**
 * @brief A receive block as a transport allocated it: the segment and where its frame starts.
 *
 * @ref alloc_rx hands this back instead of a bare segment because a LOANED block (RFC-0028
 * §6.9) starts its frame @ref tr::mem::kRxLoanBytes into the segment, and every receive loop has to
 * read the frame to, and deliver the view from, that offset.
 */
struct rx_block_t {
    segment_ptr_t seg;   /**< @brief The block; empty when the backend refused. */
    std::size_t off = 0; /**< @brief Where the frame's bytes start inside it. */

    /** @brief The @p len frame bytes a receive loop reads into. */
    [[nodiscard]] std::span<std::byte> frame(std::size_t len) const noexcept {
        return seg->bytes.subspan(off, len);
    }
    /** @brief The owning view over the frame, handing this block's reference to it. */
    [[nodiscard]] view_t take(std::size_t len) noexcept { return view_t{std::move(seg), off, len}; }
};

/**
 * @brief Allocate a transport's receive block for a @p len-byte frame, with the INGRESS LOAN
 *        reserve when the frame is large enough to be shared (RFC-0028 §6.9, #1626).
 *
 * A frame the graph stores by SHARING (at or above a vertex's copy-or-share threshold) used to
 * cost the terminus one allocation for the record that links it (`value_t`, 40 B on the host).
 * A block drawn here instead carries @ref tr::mem::kRxLoanBytes of room in front of the frame,
 * marked by `segment_t::rx_loan`, and the terminus builds that record in the room: the stored value
 * IS the receive block, and a shared ingress allocates nothing past the transport's own receive.
 *
 * The reserve is taken only where it can pay:
 * - @p len at or above @p loan_min — the transport passes its build's
 *   `tr::graph::kShareThresholdBytes`, below which the default vertex COPIES and the record is
 *   the inline block the copy lands in anyway (a build that copies always, `SIZE_MAX`, never
 *   reserves);
 * - a HOST-space backend whose alignment can hold the record, with room for `len + reserve`
 *   under its `max_segment_size`.
 * Anywhere else it draws a plain `len`-byte block at offset 0 — today's shape, and still a
 * correct frame for every reader. Either way it makes ONE request: a backend that refuses the
 * reserved block is backpressure, not a cue to ask again smaller.
 *
 * @retval rx_block_t{} (empty `seg`) The backend refused the block — backpressure.
 */
[[nodiscard]] rx_block_t alloc_rx(mem::mem_backend_t& backend, std::size_t len,
                                  std::size_t loan_min) noexcept;

/**
 * @brief Allocate a fresh, owned heap segment of @p size bytes, wrapped in an
 *        adopting `segment_ptr_t`.
 *
 * An L1 helper (it produces an owning handle), so it lives in `tr::view`, not
 * `tr::mem` (docs/adr/0016 §2). Exactly @ref segment_alloc over @ref mem::heap_backend.
 * @retval {} An empty handle on allocation failure.
 */
[[nodiscard]] segment_ptr_t heap_alloc(std::size_t size);

/**
 * @brief Allocate a fresh heap segment, copy @p bytes into it, and return a view
 *        over it — the canonical "own a copy of these bytes as a view_t" idiom
 *        (heap_alloc + memcpy + view_t::over) in one place.
 *
 * Collapses the repeated alloc/copy/over triplet across the codec and runtime
 * (graph read_schema/read_acl, the FWD resolver's WRITE-payload and reply head,
 * fwd_router's local delivery) into one audited locus.
 *
 * The return type disambiguates the two outcomes an unowned `view_t` used to
 * conflate (docs/reference/08 §L1 contracts): `std::nullopt` is an allocation
 * failure the caller maps to BACKPRESSURE; an **engaged, empty** view is a
 * legitimately-empty @p bytes span (no allocation is attempted).
 *
 * @retval std::nullopt Allocation failure / backpressure.
 * @retval {engaged}    An owned copy of @p bytes (empty-and-unowned iff @p bytes
 *                      is empty).
 */
[[nodiscard]] inline std::optional<view_t> over_bytes(std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) return view_t{};  // engaged-empty: a legitimately-empty input
    segment_ptr_t seg = heap_alloc(bytes.size());
    if (!seg) return std::nullopt;  // allocation failure => BACKPRESSURE
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/**
 * @brief The seam-taking @ref over_bytes (#793): own a copy of @p bytes in a `segment` drawn
 *        from @p backend rather than from the global heap.
 *
 * Same contract, same two outcomes; the only difference is where the bytes come from. It
 * exists for the ADR-0041 §2 ownership copies on a PEER-DRIVEN path — the rope-tier
 * `view_node::own_wire`'s single-link branch was still copying through the
 * global heap after #766 seamed the multi-link branch beside it, so one function drew from
 * two different allocators depending on how the peer fragmented the frame. #801 took the
 * span tier's `arena_node::own_wire` through the same overload: the two tiers must not
 * differ on WHERE a stored value's bytes come from, since which one runs is decided by the
 * delivering transport (a rope-delivering child vs a span-delivering one) and not by
 * anything the application chose.
 *
 * @par Why an overload and not a defaulted parameter
 * A defaulted `mem_backend_t& = mem::heap_backend()` reads better and was the first shape
 * tried. It moves the `heap_backend()` call out of `heap_alloc` and into **every** existing
 * call site, and the library's object files stop compiling to the same bytes (measured:
 * 8 of them changed, +0.1 % text). The dynamic call count is unchanged and the difference is
 * almost certainly unmeasurable — but "almost certainly" is not the standard here, and the
 * separate overload makes the pre-#793 arm provably untouched: every object file that does
 * not opt in `cmp`s equal.
 *
 * @param bytes   The bytes to own a copy of.
 * @param backend The injected byte seam. A refusal answers `std::nullopt` — the same
 *                by-value BACKPRESSURE channel an OOM already used, never an abort.
 * @retval std::nullopt The backend refused (or @p bytes could not be owned).
 * @retval {engaged}    An owned copy of @p bytes.
 */
[[nodiscard]] inline std::optional<view_t> over_bytes(std::span<const std::byte> bytes,
                                                      mem::mem_backend_t& backend) noexcept {
    if (bytes.empty()) return view_t{};  // engaged-empty: a legitimately-empty input
    segment_ptr_t seg = segment_alloc(backend, bytes.size());
    if (!seg) return std::nullopt;  // refusal => BACKPRESSURE
    std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

}  // namespace tr::view
