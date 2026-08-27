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
#include "libtracer/mem_source.hpp"
#include "libtracer/segment.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief The `mem_heap` L0 backend (`tr::mem`) and its L1 alloc helper (`tr::view`),
 *        plus the nothrow `std::vector` growth primitives (`tr::detail`).
 */

namespace tr::detail {

/**
 * @brief Test-only OOM-injection seam over @ref probe_bytes: when set, a probe of @p bytes
 *        that the hook rejects soft-fails as if the heap were exhausted.
 *
 * The nothrow soft-fail paths cannot be exercised by really exhausting the host heap, so
 * this is their failure-injection tool — the global-heap twin of the failing `mem_backend_t`
 * the `graph_value_backend_test` precedent injects (ADR-0060 §3). Production never sets it;
 * the cost is one predictable null-check on the (cold) growth/probe paths.
 */
inline bool (*probe_fail_hook)(std::size_t bytes) noexcept = nullptr;

/**
 * @brief The @ref probe_fail_hook gate alone (no real probe): true when no hook is set or
 *        the hook admits @p bytes.
 *
 * For soft-fail sites whose failure leg is not the probe itself (e.g. a host-profile
 * `catch (bad_alloc)`) but that must still honor the test seam.
 */
[[nodiscard]] inline bool probe_hook_ok(std::size_t bytes) noexcept {
    return probe_fail_hook == nullptr || probe_fail_hook(bytes);
}

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
 *       The helpers now take any allocator, and `tr::mem::source_allocator_t`
 *       (`%mem_source_alloc.hpp`) is the one the graph's migrated growth sites use. Every
 *       sentence above turns on the probe testing a DIFFERENT allocator from the one the
 *       growth uses; a source allocator publishes the store it draws from
 *       (`source_allocator_t::source()`), so @ref try_grow probes THAT store with
 *       `try_alloc`/`release` instead of the global heap. The `-fno-exceptions` probe is still
 *       probe-then-commit and still carries the #850 race window — what it stops being is
 *       an answer about the wrong memory. `probe_fail_hook` is honoured on both arms, so the
 *       OOM-injection seam still reaches these paths.
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
 * @param bytes The byte count the growth will request.
 * @param grow  The throwing growth; it must leave its container unchanged when it throws.
 * @retval false The growth allocation failed — the container is unchanged.
 */
template <class F>
[[nodiscard]] inline bool try_grow_from(mem::block_source_t& src, std::size_t bytes,
                                        F&& grow) noexcept {
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

/**
 * @brief The host allocator backend: owns platform-heap bytes, frees them and the
 *        `segment_t` control block on destroy.
 *
 * Exposed here (rather than TU-local) so the module-set destroy dispatch
 * (backend_set.cpp, ADR-0047 §2) can devirtualize its release; a `final` class,
 * so the qualified call in that switch is a direct call.
 *
 * @par The layering, after #873 phase 3
 * Both draws go through @ref heap_source_t::acquire and both returns through
 * @ref heap_source_t::reclaim — the substrate's OWN platform-heap arm, so the backend tier no
 * longer spells the platform-heap allocation a second time. It is a `static` entry point
 * rather than an injected @ref block_source_t reference on purpose: this backend
 * is the process default on the hottest allocation path in the library, and an injected draw
 * there would buy no bounding whatever (a deployer who wants bounding injects a source into
 * `graph_t` and gets @ref source_backend_t) while costing the virtual call #873 phase 2
 * measured at +22.7 % on the hazard domain. The re-layering is a layering claim; it is not an
 * excuse to add an indirection nobody can use.
 *
 * @note One consequence is worth naming rather than leaving to be discovered. The payload
 *       draw used to name the OVER-ALIGNED `operator new` unconditionally
 *       (`std::align_val_t{alignof(std::max_align_t)}`), which libstdc++ routes through
 *       `aligned_alloc`/`posix_memalign` even when the plain allocator already guarantees that
 *       alignment. @ref heap_source_t::acquire takes the plain nothrow arm whenever
 *       `align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__` — which is the *definition* of the
 *       guarantee plain `operator new` gives, so nothing loses an alignment it had — and the
 *       reclaim becomes the SIZED `operator delete(p, bytes)`. Both are the arms every other
 *       #873 channel already takes; the allocation COUNT is unchanged (two draws per segment,
 *       which is what `bench_forward_heap`'s `allocs=` pins count).
 */
class heap_backend_t final : public mem_backend_t {
   public:
    heap_backend_t() noexcept : mem_backend_t("mem_heap") {}

    view::segment_t* alloc(std::size_t size, alloc_hint_t /*hint*/) override {
        constexpr std::size_t kPayloadAlign = alignof(std::max_align_t);
        void* raw = size ? heap_source_t::acquire(size, kPayloadAlign) : nullptr;
        if (size && raw == nullptr) return nullptr;
        void* const cb = heap_source_t::acquire(sizeof(view::segment_t), alignof(view::segment_t));
        if (cb == nullptr) {
            if (raw) heap_source_t::reclaim(raw, size, kPayloadAlign);
            return nullptr;
        }
        return new (cb)
            view::segment_t(this, std::span<std::byte>(static_cast<std::byte*>(raw), size));
    }

    void destroy(view::segment_t* seg) noexcept override {
        const std::span<std::byte> bytes = seg->bytes;
        seg->~segment_t();
        heap_source_t::reclaim(seg, sizeof(view::segment_t), alignof(view::segment_t));
        if (!bytes.empty()) {
            heap_source_t::reclaim(bytes.data(), bytes.size(), alignof(std::max_align_t));
        }
    }

    [[nodiscard]] backend_tag tag() const noexcept override { return backend_tag::HEAP; }

    // Module-set traits (ADR-0047 §2). `needs_cache_ops` is read by `mem::transfer`.
    static constexpr bool needs_cache_ops =
        false; /**< @brief No DMA cache maintenance (host RAM). */
    static constexpr bool is_isr_safe =
        false; /**< @brief `alloc`/`destroy` call `operator new`/`delete` — not ISR-safe. */
    static constexpr bool is_nonblocking =
        false; /**< @brief `operator new`/`delete` may lock or syscall (#928). */
    static constexpr bool owns_bytes =
        true; /**< @brief Owns the `operator new`'d bytes — durably storable. */
};

/** @brief The process-wide heap backend (function-local static — no init-order trap). */
[[nodiscard]] mem_backend_t& heap_backend() noexcept;

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
