/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_source — the L0 nothrow block seam every FAILABLE allocation draws from
 * (#551). Raw bytes, failure by value, no refcount.
 */
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <type_traits>
#include <utility>

#include "libtracer/guard.hpp"

/**
 * @file
 * @brief The nothrow failable-block seam (`tr::mem::block_source_t`), the
 *        process-wide platform-heap source that backs it by default, and the two
 *        companions the migrated call sites need: a bump source over a caller
 *        buffer and a nothrow growable array.
 */

namespace tr::detail {

/**
 * @brief Test-only OOM-injection seam over the nothrow heap draws: when set, a draw of
 *        @p bytes that the hook rejects soft-fails as if the heap were exhausted.
 *
 * The nothrow soft-fail paths cannot be exercised by really exhausting the host heap, so
 * this is their failure-injection tool — the global-heap twin of the failing `mem_backend_t`
 * the `graph_value_backend_test` precedent injects (ADR-0060 §3). Production never sets it;
 * the cost is one predictable null-check on the growth/probe paths and on the
 * process-default @ref tr::mem::heap_source_t draw (the one a `value_t` block takes when no
 * source is injected). It lives here, at L0, so that source can see it.
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

}  // namespace tr::detail

namespace tr::mem {

/**
 * @brief One block source's census, in the unified introspection vocabulary
 *        (`core/STYLE.md` §Introspection; #1503).
 *
 * The five nouns every bounded resource answers with, spelled the same way here as at
 * every other seam: an effective ceiling, used-polarity occupancy, a high-water mark,
 * and the two numbers a sizing operator actually needs — how often a request was
 * refused, and how big the biggest refused one was (#1492: the TAIL is what refuses, so
 * a median request size tells the operator nothing).
 *
 * All-zero is the honest default for a source that counts nothing, exactly as
 * `%tr::net::transport_drop_stats_t` is for a link that counts nothing (#932) — never a
 * fabricated number. A field a particular source cannot answer stays 0; `capacity == 0`
 * means "unbounded, or not reported", never "a zero-byte ceiling".
 *
 * Snapshot coherence is the `core/STYLE.md` §Introspection clause: monotonic since
 * construction, sampled unsynchronized, and the intended reading is the DIFFERENCE
 * between two snapshots rather than an instant.
 */
struct source_stats_t {
    /** @brief The effective byte ceiling this source serves from — the caller's injected
     *         slab, not a compile-time constant. 0 = unbounded / not reported. */
    std::size_t capacity = 0;
    /** @brief Bytes handed out and not returned to this source, USED-polarity (free is
     *         `capacity - in_use`, and is deliberately not the primary). */
    std::size_t in_use = 0;
    /** @brief High-water mark of @ref in_use since construction. */
    std::size_t peak = 0;
    /** @brief @ref block_source_t::try_alloc calls this source answered `nullptr` —
     *         requests refused BY VALUE, so the caller was told (it answered
     *         BACKPRESSURE). Distinct from a `dropped`, where nobody was told. */
    std::size_t refused = 0;
    /** @brief Bytes of the LARGEST request in @ref refused — the number a deployment
     *         grows its slab to. 0 iff @ref refused is 0. */
    std::size_t largest_refused = 0;
};

/**
 * @brief The nothrow block seam every FAILABLE allocation draws from — the ones a
 *        PEER can provoke (#551, ADR-0065; ADR-0039 erratum 5/6).
 *
 * @note "Failable", not "control-plane": CONTEXT.md already binds *control plane* to the
 *       `:` field-write addressing plane, and this seam is orthogonal to that axis — a
 *       DATA-plane branch write is one of its first consumers.
 *
 * RFC-0014 made vertex registration a **runtime, wire-driven** operation: a peer's
 * CREATE frame reaches `register_vertex_key`. Every allocation on that path is an
 * unguarded throwing one, and ESP-IDF link-wraps `__cxa_throw` /
 * `__cxa_allocate_exception` to `abort()` stubs — so on the shipping profile a peer
 * can reboot the node by exhausting the heap. This seam is the failure-by-value
 * answer: exhaustion returns `nullptr` and the operation answers BACKPRESSURE.
 *
 * @note Deliberately NOT a `std::pmr::memory_resource`, and not derived from one.
 *       That type's `allocate` is annotated `__attribute__((__returns_nonnull__))`
 *       (libstdc++ `bits/memory_resource.h`), so a caller's `if (p == nullptr)` is
 *       undefined-behaviour-deletable. Measured on riscv32-esp-elf-g++ 15.2.0 with
 *       the deployment flags: the soft-fail branch survives at `-O0`/`-O1`/`-O2`/`-O3`
 *       and is GONE at `-Os`/`-Oz` — the level the reference node ships at
 *       (`CONFIG_COMPILER_OPTIMIZATION_SIZE`), and the level at which no job exercises an
 *       allocation-failure path (see ADR-0065 §1).
 *       Inheriting would keep that `allocate()` publicly callable on this object,
 *       one token away from every correct `try_alloc` call site, with no diagnostic
 *       at any warning level. A separate type makes the slip a compile error.
 *
 * @warning DO NOT WRAP A `std::pmr::memory_resource` BEHIND THIS SEAM. The note above
 *          says why this type is not a pmr resource; this one is about the REVERSE
 *          adaptation, which is the mistake a host migrating an existing pmr arena
 *          actually makes (#1493). The obvious adapter compiles, looks correct and
 *          passes review:
 *          @code
 *          void* try_alloc(std::size_t n, std::size_t a) noexcept override {
 *              return mr_->allocate(n, a);   // <-- CANNOT report exhaustion
 *          }
 *          @endcode
 *          `%std::pmr::memory_resource::allocate` signals exhaustion only by THROWING and
 *          has no nothrow form, so this `try_alloc` either succeeds or never returns —
 *          it never answers `nullptr`. Under `-fno-exceptions` the throw reaches ESP-IDF's
 *          link-wrapped `__cxa_throw` → `abort()` stub, which is exactly the
 *          reboot-a-node-by-exhausting-the-heap failure this seam exists to remove,
 *          reintroduced by a class whose declaration promises the opposite. A comment
 *          on the caveat does not fix it; it labels the landmine.
 *
 *          **Nor does a budget-tracking variant fix it** — one that counts its own bytes
 *          and answers `nullptr` at the ceiling *before* delegating. Tracking a budget
 *          does not make the adapter honest: a FRAGMENTED pmr resource can throw well
 *          BELOW the budget, so the adapter is correct except exactly when the underlying
 *          resource is in the state the bound was supposed to protect against. Such an
 *          adapter is deliberately not offered and must not be added.
 *
 *          **The supported answer for "reuse my existing arena" is
 *          @ref pool_source_t's span constructor**, which carves from a caller-provided
 *          slab with caller-provided size classes and is not pmr at all — point it at the
 *          same storage the pmr resource was partitioning, rather than at the resource.
 *          The one direction that IS offered is the opposite one:
 *          `tr::mem::source_resource_t` (`%mem_source_pmr.hpp`) serves a `std::pmr`
 *          container FROM a `block_source_t`.
 *
 * @note Also distinct from @ref mem_backend_t, which vends a refcounted
 *       @ref view::segment_t. Control-plane blocks have a single owner and no
 *       header; a refcount on them is pure overhead (a `segment_t` measures 20 B on
 *       rv32 / 40 B on x86-64 against a `vertex_t` of 72 B on rv32 / 96 B on x86-64 —
 *       the sizes the `config_t` ratchets pin, re-measured by the #1487 census).
 *
 * @note Blocks are host-owned storage: the source MUST outlive the `graph_t` and
 *       every object built in its blocks. Teardown is driven by whoever holds the
 *       source, never by the object itself — a `vertex_t` has no room for the
 *       pointer (`core/tests/vertex_size_test.cpp`).
 *
 * @note Each source declares its own concurrency contract, exactly as
 *       @ref mem_backend_t does (ADR-0012). The RFC-0014 wire-driven registration
 *       path runs on a transport thread, so an injected source must be thread-safe
 *       on that target. @ref heap_source_t is.
 */
class block_source_t {
   public:
    /** @brief Construct a source with a stable, human-readable @p name (e.g. `"heap"`). */
    explicit constexpr block_source_t(const char* name) noexcept : name_(name) {}
    /** @brief Sources are held by pointer and outlive their users; virtual teardown. */
    virtual ~block_source_t() = default;

    /** @brief Non-copyable — a source is an identity, not a value. */
    block_source_t(const block_source_t&) = delete;
    /** @brief Non-assignable. */
    block_source_t& operator=(const block_source_t&) = delete;

    /**
     * @brief Obtain @p bytes of storage aligned to at least @p align — NOTHROW.
     *
     * @param bytes Size of the block; a zero-sized request is implementation-defined
     *              and callers do not make one.
     * @param align Minimum alignment, a power of two.
     * @retval nullptr Exhaustion. The caller answers BACKPRESSURE; it never falls back
     *                 to the global heap and never aborts.
     */
    [[nodiscard]] virtual void* try_alloc(
        std::size_t bytes, std::size_t align = alignof(std::max_align_t)) noexcept = 0;

    /**
     * @brief Return a block previously handed out by @ref try_alloc.
     *
     * @warning @p bytes and @p align MUST match the originating @ref try_alloc call
     *          (sized reclaim), so a bump or pool source needs no per-block header.
     */
    virtual void release(void* p, std::size_t bytes,
                         std::size_t align = alignof(std::max_align_t)) noexcept = 0;

    /** @brief The source's stable name, for census and diagnostics. */
    [[nodiscard]] const char* name() const noexcept { return name_; }

    /**
     * @brief This source's census — the interface-level introspection seam (#1492, #1503).
     *
     * The whole vocabulary of this seam used to be @ref name, so a host holding a
     * `block_source_t&` could introspect NOTHING: not the ceiling it injected, not how
     * much of it was gone, and above all not whether anything had been refused —
     * `try_alloc → nullptr` was uncounted at every implementation in the tree.
     *
     * Optional, in the `%tr::net::transport_t::drop_stats` mould (#932): the DEFAULT is
     * all-zero, which is the honest answer for a source that counts nothing, never a
     * fabricated number. Concrete sources override it — @ref bump_source_t and
     * @ref pool_source_t do; @ref heap_source_t does not (the platform heap's ceiling is
     * not this seam's to report), and neither does @ref null_source_t, whose refusals are
     * its whole contract and are the CALLER's to count.
     *
     * Counted, never enforced, and never on the hot arm: the refusal counters are bumped
     * only where @ref try_alloc is already returning `nullptr`, so a successful allocation
     * pays nothing at all (`core/STYLE.md` §Introspection, counting doctrine 1 —
     * ADR-0039's `bench_forward_heap == 0` hop and ADR-0067's rv32 text figure are the
     * standing referees).
     */
    [[nodiscard]] virtual source_stats_t stats() const noexcept { return {}; }

   private:
    const char* name_; /**< @brief Borrowed literal; never owned. */
};

/**
 * @brief The allocation-seam contract as a concept (RFC-0028 §5.6): a type IS a block source
 *        when it derives from `block_source_t` — every `mem_backend_t` included, since
 *        slice 10 re-based the backend on this seam (§4.9).
 */
template <class B>
concept block_source = std::derived_from<B, block_source_t>;

/**
 * @brief The default source: the platform heap, nothrow.
 *
 * Behaviour is byte-identical to today for a host that injects nothing, EXCEPT that
 * exhaustion returns `nullptr` instead of reaching the ESP-IDF `__cxa_throw` abort
 * stub. Thread-safe: the global nothrow `operator new` is.
 */
class heap_source_t final : public block_source_t {
   public:
    /** @brief Constant-initializable, so the process-wide default costs no dynamic init. */
    constexpr heap_source_t() noexcept : block_source_t("heap") {}

    /**
     * @brief The platform-heap acquisition arm as a FREE, non-virtual entry point (#873 phase 3).
     *
     * The same two arms @ref try_alloc dispatches, callable without an object and therefore
     * without a virtual call. It exists so that the one in-tree @ref mem_backend_t that still
     * acquires its bytes from the platform heap — @ref heap_backend_t — can express that
     * acquisition **on the substrate** rather than on a second, independently-spelled
     * platform-heap pair. Phase 3's re-layering is a layering claim, not a new indirection:
     * @ref heap_backend_t is the process default and sits on the hottest allocation path in the
     * library, so routing it through a `block_source_t&` would have bought no bounding (a
     * deployer who wants bounding injects a source and gets @ref source_backend_t) at the cost
     * of the virtual draw #873 phase 2 measured at +22.7 % on the hazard domain. A `static`
     * entry point gives the layering with a direct call.
     *
     * @param bytes Size of the block.
     * @param align Minimum alignment, a power of two.
     * @retval nullptr Exhaustion.
     */
    [[nodiscard]] static void* acquire(std::size_t bytes, std::size_t align) noexcept {
        if (align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) return ::operator new(bytes, std::nothrow);
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }

    /** @brief The sized-reclaim twin of @ref acquire, on whichever arm served the block. */
    static void reclaim(void* p, std::size_t bytes, std::size_t align) noexcept {
        if (align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
            ::operator delete(p, bytes);
            return;
        }
        ::operator delete(p, bytes, std::align_val_t{align});
    }

    /**
     * @brief Nothrow heap allocation; `nullptr` on exhaustion.
     *
     * @par Why the plain-`new` arm exists (#873 phase 1)
     * A request at or below `__STDCPP_DEFAULT_NEW_ALIGNMENT__` takes the PLAIN nothrow
     * `operator new`, not the over-aligned one. The two are not the same code: libstdc++
     * routes the `align_val_t` overload through `aligned_alloc`/`posix_memalign` even when
     * the alignment is one the plain allocator already guarantees, which is a different
     * glibc path with a different size-class layout. That distinction became load-bearing
     * when #873 phase 1 moved channels that had ALWAYS used the plain `operator new` — the
     * child-registry chunks and the graph's pmr-served control blocks — behind this seam:
     * with this arm, "the process default preserves today's behaviour byte-for-byte" is
     * literally true rather than approximately true. Over-aligned requests (a DMA source's
     * clients, a cache-line-padded stripe) keep the aligned pair, so nothing loses a
     * guarantee it had.
     *
     * The virtual entry — and only it, never @ref acquire — consults the test-only
     * `tr::detail::probe_hook_ok` seam first, so an injected refusal reaches whatever draws
     * through the process-default source (a `value_t` block, a control-plane container)
     * without the hot direct-call arm @ref heap_backend_t takes paying for it.
     */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (!tr::detail::probe_hook_ok(bytes)) return nullptr;  // test-only OOM injection
        return acquire(bytes, align);
    }

    /** @brief Sized reclaim matching @ref try_alloc, on whichever arm served the block. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        reclaim(p, bytes, align);
    }
};

/**
 * @brief The process-wide default @ref block_source_t (the platform heap).
 *
 * A namespace-scope `constinit` object behind a function, NOT a function-local static:
 * the latter costs a `__cxa_guard` word in `.bss` and an acquire fence on every call.
 */
[[nodiscard]] block_source_t& heap_source() noexcept;

/**
 * @brief The source that serves nothing — every request is exhaustion.
 *
 * The upstream to give a @ref bump_source_t when its buffer must be the HARD bound, so a
 * frame that outgrows it is rejected rather than reaching the global heap. This is the
 * bounded-node composition, and the honest replacement for
 * `std::pmr::null_memory_resource()`, which signals the same thing by throwing.
 */
class null_source_t final : public block_source_t {
   public:
    /** @brief Constant-initializable, like @ref heap_source_t. */
    constexpr null_source_t() noexcept : block_source_t("null") {}
    /** @brief Always `nullptr`. */
    [[nodiscard]] void* try_alloc(std::size_t, std::size_t) noexcept override { return nullptr; }
    /** @brief Unreachable — this source hands out nothing to return. */
    void release(void*, std::size_t, std::size_t) noexcept override {}
};

/** @brief The process-wide @ref null_source_t (serves nothing; see the class docs). */
[[nodiscard]] block_source_t& null_source() noexcept;

/**
 * @brief A caller-owned buffer handed out by bump, falling back to @p upstream once full.
 *
 * The nothrow twin of `std::pmr::monotonic_buffer_resource` over a fixed span. Blocks
 * carved from the span are never individually reclaimed (@ref release is a no-op for
 * them, exactly as a monotonic resource behaves); blocks that came from the upstream are
 * returned to it, so a decode that outgrows the buffer still frees what it borrowed.
 *
 * @note The upstream is what keeps this a capability-preserving substitution: a
 *       `monotonic_buffer_resource` also spills past its buffer, but it spills to a
 *       THROWING default resource, which on `-fno-exceptions` is the `abort()` this whole
 *       seam exists to remove. Pass a bounded source (or a null-serving one) to make the
 *       buffer the hard limit instead.
 *
 * @warning SCOPE-LIFETIME USE ONLY. A bump block is never reclaimed, so a source that
 *          outlives one burst of work monotonically fills and then refuses everything.
 *          Construct it per operation (as the branch-write decode does), or @ref reset it
 *          between operations. It is NOT a long-lived seam: an 8 KiB bump source wired as
 *          a router's `rx` decoded 6 frames and rejected the next 194 — measured. A
 *          long-lived bounded seam wants @ref pool_source_t, which recycles.
 * @note Single-threaded by contract — a bump cursor is not synchronized. Its intended use
 *       is a function-scoped buffer on the calling thread's stack.
 */
class bump_source_t final : public block_source_t {
   public:
    /** @brief Carve from @p buffer; once it cannot serve a request, draw from @p upstream. */
    explicit bump_source_t(std::span<std::byte> buffer,
                           block_source_t& upstream = heap_source()) noexcept
        : block_source_t("bump"), buf_(buffer), upstream_(&upstream) {}

    /** @brief Bump-allocate, aligned; falls back to the upstream when the buffer cannot fit. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        const std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(buf_.data()) + used_;
        // Padding to the next `align` boundary. Mask, not modulo: `align` is a power of
        // two by contract, and two integer divisions here are worth ~20 % of a terminus
        // decode — measured, on the way in.
        const auto pad = static_cast<std::size_t>((~cur + 1U) & (align - 1U));
        if (pad <= buf_.size() - used_ && bytes <= buf_.size() - used_ - pad) {
            used_ += pad + bytes;
            return buf_.data() + (used_ - bytes);
        }
        void* const spill = upstream_->try_alloc(bytes, align);
        if (spill == nullptr) count_refusal(bytes);
        return spill;
    }

    /** @brief No-op for a bump block; a sized return to the upstream otherwise. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        auto* b = static_cast<std::byte*>(p);
        if (b >= buf_.data() && b < buf_.data() + buf_.size()) return;  // bump: never reclaimed
        upstream_->release(p, bytes, align);
    }

    /**
     * @brief Hand the whole buffer back for reuse — the next @ref try_alloc starts at 0.
     *
     * For a source reused across scopes (a terminus decoding frame after frame into the
     * same slab). Deliberately NOT called `release`: on a
     * `std::pmr::monotonic_buffer_resource` that name means exactly this, while on
     * @ref block_source_t it means "return one block", and the two must not be confusable
     * at a call site.
     *
     * @warning Every block previously carved from the buffer dangles afterwards. Blocks
     *          that came from the upstream are NOT reclaimed by this — return those first.
     */
    void reset() noexcept {
        // Fold the cycle that is ending into the high-water mark BEFORE the cursor is
        // rewound. This is the whole cost of `peak` for a bump source: the cursor is
        // monotonic within a cycle, so `peak` is just the largest cursor any cycle
        // reached, and nothing at all is added to `try_alloc`'s success arm.
        if (used_ > peak_) peak_ = used_;
        used_ = 0;
    }

    /** @brief Bytes carved from the buffer so far (diagnostics; excludes upstream blocks). */
    [[nodiscard]] std::size_t used() const noexcept { return used_; }

    /**
     * @brief This bump source's census (@ref source_stats_t; `core/STYLE.md` §Introspection).
     *
     * `capacity`/`in_use`/`peak` describe the CALLER'S BUFFER — the span size this source
     * was handed, how much of it the cursor has carved, and the deepest any @ref reset
     * cycle got. Upstream spill is deliberately outside all three: those bytes are the
     * upstream's census to report, and folding them in here would make `in_use` exceed
     * `capacity` on the very source whose ceiling the number exists to describe.
     *
     * `refused` counts what a caller experienced: a @ref try_alloc that answered `nullptr`,
     * which for this source means the buffer could not fit the request AND the upstream
     * refused it too. Against a bounded upstream (@ref null_source() — the composition that
     * makes the buffer a hard limit) that is exactly "the buffer overflowed"; against
     * @ref heap_source() it stays 0 until the platform heap is gone, which is the honest
     * reading in both cases.
     *
     * Plain counters, no atomics: this source is single-threaded BY CONTRACT (see the class
     * note), so the ownership discipline that already protects `used_` protects these
     * (`core/STYLE.md` §Introspection, counting doctrine 5).
     */
    [[nodiscard]] source_stats_t stats() const noexcept override {
        return source_stats_t{buf_.size(), used_, used_ > peak_ ? used_ : peak_, refused_,
                              largest_refused_};
    }

   private:
    /** @brief Record one refused request. Cold arm only — @ref try_alloc is already
     *         returning `nullptr` by the time this runs. */
    [[gnu::noinline]] void count_refusal(std::size_t bytes) noexcept {
        ++refused_;
        if (bytes > largest_refused_) largest_refused_ = bytes;
    }

    std::span<std::byte> buf_;
    block_source_t* upstream_;
    std::size_t used_ = 0;
    std::size_t peak_ = 0;            /**< @brief Deepest cursor of any completed cycle. */
    std::size_t refused_ = 0;         /**< @brief `try_alloc` calls answered `nullptr`. */
    std::size_t largest_refused_ = 0; /**< @brief Bytes of the largest of those. */
};

/**
 * @brief The no-op synchronization policy — the default, and the one the hot seam wants.
 *
 * A @ref pool_source_t owned by exactly one thread needs no synchronization at all, and
 * that is the intended shape for a per-receiver source: ownership removes the race
 * instead of guarding it. See @ref pool_source_t's threading note for why this matters
 * more than the choice of free-list algorithm.
 *
 * Since #1703 this is the layer-neutral `tr::no_guard_t` (`%guard.hpp`), so the memory layer
 * keeps one lock vocabulary, not two. A policy is any `tr::lockable` (`lock()`/`unlock()`,
 * both `noexcept`); a target supplies its own where it needs one (an interrupt-disable
 * critical section on single-core FreeRTOS, `tr::mem::sync_mutex_t` from
 * `%mem_source_sync.hpp` on a host). This header stays freestanding-clean, so it pulls in no
 * threading facility of its own.
 *
 * Deprecated: Kept as an alias for one release (#1703); name `tr::no_guard_t`.
 */
using sync_none_t = ::tr::no_guard_t;

/**
 * @brief One recycling free-list, keyed by the exact `(bytes, align)` pair it serves.
 *
 * Caller-supplied storage: a @ref pool_source_t is handed a span of these, so the number
 * of classes is a deployment property rather than a constant in this header (ADR-0065's
 * injected-bounds rule). Sizing it is measurable — see @ref pool_source_t::classes_used.
 */
struct size_class_t {
    std::size_t bytes = 0; /**< @brief Normalized payload size; 0 marks an unused slot. */
    std::size_t align = 0; /**< @brief Normalized alignment this class's blocks satisfy. */
    void* head = nullptr;  /**< @brief Intrusive free-list head; the link lives in the block. */
};

/**
 * @brief A BOUNDED, RECYCLING source: segregated exact-size free lists over a caller slab.
 *
 * The long-lived counterpart to @ref bump_source_t, and the source a node with a RAM
 * ceiling injects. Exhaustion is `nullptr` — never the platform heap, never an abort.
 *
 * ### Why exact-size classes, measured
 *
 * The demand at this seam is nearly degenerate. Recording every `try_alloc`/`release`
 * across the host suite (70,937 events) found **12 distinct sizes, three of which cover
 * 99.8 % of all allocations** — they are the arena's geometrically growing arrays. So
 * exact classes cost **zero internal fragmentation**, and a first-fit-with-coalescing
 * allocator's header buys nothing back. Replaying that trace against both policies:
 *
 * | policy | slab to serve the trace | vs peak-live floor |
 * | --- | ---: | ---: |
 * | this one | 26,176 B | +11.1 % |
 * | first-fit + coalescing | 27,448 B | +16.5 % |
 *
 * The gap is **1,088 B of external fragmentation and only 184 B of header** — splitting a
 * remainder under geometric growth rarely produces the size of the next request. Note what
 * that says about the usual argument for a header-free pool: here it is worth 0.7 % of the
 * difference, so it is not the reason to choose this shape.
 *
 * Code size of what actually ships, `riscv32-esp-elf-g++ -Os -fno-exceptions -fno-rtti`:
 * **322 B** of text (`try_alloc` 120, `release` 142, `find` 46, teardown 14) plus a 24 B
 * vtable (ADR-0067). #1503's refusal counting adds **+18 B** to `try_alloc`'s
 * already-returning-`nullptr` arm and **+4 B** of vtable for the @ref stats slot — measured
 * as a delta on riscv32-esp-elf-g++ 14.2.0 at the same flags, with `release`, `find` and
 * teardown byte-identical, so the shipped figure is **340 B** of text plus a 28 B vtable.
 * The success arm did not move, which is the gate that mattered (`core/STYLE.md`
 * §Introspection, counting doctrine 1).
 * The two 256 B / 380 B figures quoted while choosing between the policies were
 * feature-matched *prototypes* — neither carried the alignment key, the overflow counter
 * or the foreign-pointer check this one does — so they compare the shapes to each other
 * and are not the shipped cost of either.
 *
 * ### Threading — read this before sharing one
 *
 * @warning A shared pool is the WRONG shape for a hot multi-core path, and this is
 *          measured, not feared. ADR-0060 erratum 1 recorded a shared free-list pool
 *          collapsing to roughly **a fifteenth of its own single-thread rate** on a
 *          12-core host (8.3 M → 1.36 M ops/s; p50 60 ns → 3587 ns) while the platform
 *          heap *scaled* — a cacheline storm, not serialization. A lock-free CAS on the
 *          list head does not fix it: it replaces one contended word with the same word.
 *          The shape that works is per-thread free lists, and the cheapest way to get
 *          them is to give each receiver its **own** source rather than to guard a shared
 *          one. Reach for a locking @p Sync only where the seam is wiring-frequency (a
 *          graph's control source), never per-frame.
 *
 * @tparam Sync Synchronization policy, a `tr::lockable`; `tr::no_guard_t` by default, which
 *              compiles to nothing.
 */
template <::tr::lockable Sync = ::tr::no_guard_t>
class pool_source_t final : public block_source_t {
   public:
    /**
     * @brief Serve allocations from @p slab, recycling through @p classes.
     *
     * This is also **the supported "reuse my existing arena" path** (#1493). A host that
     * already partitions a static slab with `std::pmr` — the
     * `%monotonic_buffer_resource` under a `%synchronized_pool_resource` shape ADR-0039
     * describes — points this constructor at the SAME STORAGE rather than at the
     * resource. There is no pmr in the result and nothing to adapt, so exhaustion stays
     * a `nullptr` all the way down; see @ref block_source_t's warning for why wrapping
     * the resource instead cannot work.
     *
     * @param slab    Caller-owned storage; must outlive every block carved from it.
     * @param classes Caller-owned free-list slots. Running out is safe but lossy — see
     *                @ref overflowed.
     */
    pool_source_t(std::span<std::byte> slab, std::span<size_class_t> classes) noexcept
        : block_source_t("pool"), buf_(slab), cls_(classes) {}

    /** @brief Pop a recycled block of this exact shape, else carve a fresh one; `nullptr` when
     * full. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        normalize(bytes, align);
        const guard_t g{sync_};
        if (size_class_t* c = find(bytes, align); c != nullptr && c->head != nullptr) {
            void* const p = c->head;
            std::memcpy(&c->head, p, sizeof(void*));  // pop the intrusive link
            return p;
        }
        // Padding to the next `align` boundary. Mask, not modulo: `align` is a power of two
        // by contract, and the division showed up as ~20 % of a terminus decode.
        const std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(buf_.data()) + used_;
        const auto pad = static_cast<std::size_t>((~cur + 1U) & (align - 1U));
        if (pad > buf_.size() - used_ || bytes > buf_.size() - used_ - pad) {
            // PRIMARY EXHAUSTION — the slab cannot carve this shape and no recycled block
            // of it exists. Counted here and nowhere else: this is the arm that was
            // returning `nullptr` uncounted at every source in the tree (#1492), and the
            // success arm below is untouched by design (`core/STYLE.md` §Introspection,
            // counting doctrine 1). NOT `overflow_`, which counts a recycling degrade and
            // keeps that meaning — a pool can refuse with zero overflows and overflow with
            // zero refusals.
            ++refused_;
            if (bytes > largest_refused_) largest_refused_ = bytes;
            return nullptr;
        }
        used_ += pad + bytes;
        return buf_.data() + (used_ - bytes);
    }

    /**
     * @brief Return a block to its class's free list.
     *
     * @p bytes and @p align MUST match the originating @ref try_alloc, per the seam's sized
     * contract — that is what lets a block carry no header. A pointer from outside the slab
     * is ignored rather than trusted, mirroring `bump_source_t` — two compares are cheaper
     * than the corruption a foreign pointer would cause.
     */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        auto* const b = static_cast<std::byte*>(p);
        if (b < buf_.data() || b >= buf_.data() + buf_.size()) return;
        normalize(bytes, align);
        const guard_t g{sync_};
        size_class_t* c = find(bytes, align);
        if (c == nullptr) c = claim(bytes, align);
        if (c == nullptr) {
            // The class table is full. The block stays carved — bounded and safe, never
            // corrupt — and the loss is counted so a deployment can size the span.
            ++overflow_;
            return;
        }
        std::memcpy(p, &c->head, sizeof(void*));  // push the intrusive link
        c->head = p;
    }

    /** @brief Bytes carved from the slab so far, recycled blocks included (diagnostics). */
    [[nodiscard]] std::size_t used() const noexcept { return used_; }

    /** @brief Class slots in use — the number to size the injected span against. */
    [[nodiscard]] std::size_t classes_used() const noexcept { return n_; }

    /**
     * @brief Blocks lost because the class table was full; non-zero means the span is too
     *        small.
     *
     * @note A RECYCLING DEGRADE, not an allocation refusal, and the two are deliberately
     *       separate counters (`core/STYLE.md` §Introspection): the block stays carved —
     *       bounded and safe — and the caller that freed it was never refused anything.
     *       The refusal number is @ref refused.
     */
    [[nodiscard]] std::size_t overflowed() const noexcept { return overflow_; }

    /** @brief @ref try_alloc calls this pool answered `nullptr` — primary slab exhaustion
     *         (#1492). Also reachable via @ref stats. */
    [[nodiscard]] std::size_t refused() const noexcept { return refused_; }

    /** @brief Bytes of the largest request in @ref refused — the number to grow the slab to
     *         (#1492: the tail is what refuses). */
    [[nodiscard]] std::size_t largest_refused() const noexcept { return largest_refused_; }

    /**
     * @brief This pool's census (@ref source_stats_t; `core/STYLE.md` §Introspection).
     *
     * `capacity` is the injected slab, and `in_use` is @ref used — bytes CARVED, recycled
     * blocks sitting on a free list included, because a carved block is never returned to
     * the slab and so is not available to a different size class. That makes carving
     * monotonic, and `peak` therefore equals `in_use` by construction: the high-water mark
     * costs this source not one instruction.
     *
     * Plain counters under the existing @p Sync section, not atomics: the refusal bump sits
     * inside the same `guard_t` `try_alloc` already holds, so a shared pool's counters are
     * as synchronized as its free lists are and nothing new is locked. On rv32 an atomic
     * wide enough to matter is not lock-free anyway — it takes a hidden libatomic lock per
     * access (`core/STYLE.md` §Introspection, counting doctrine 5).
     *
     * @note @ref overflowed is NOT in this block. It counts a recycling degrade rather than
     *       a refusal, so it is neither `refused` nor `dropped` in the shared vocabulary,
     *       and it stays this type's own named accessor.
     */
    [[nodiscard]] source_stats_t stats() const noexcept override {
        return source_stats_t{buf_.size(), used_, used_, refused_, largest_refused_};
    }

   private:
    /** @brief RAII lock over the policy; empty and free when @p Sync is `tr::no_guard_t`. */
    struct guard_t {
        explicit guard_t(Sync& s) noexcept : s_(s) { s_.lock(); }
        ~guard_t() { s_.unlock(); }
        guard_t(const guard_t&) = delete;
        guard_t& operator=(const guard_t&) = delete;
        Sync& s_;
    };

    /**
     * @brief Widen a request so a freed block can host the intrusive link.
     *
     * Applied identically on both sides, so the `(bytes, align)` a `release` computes is
     * the one its `try_alloc` computed — the whole header-free scheme rests on that.
     * Alignment is part of the key rather than folded away: a block carved for `align=4`
     * cannot be handed back out for an `align=8` request.
     */
    static void normalize(std::size_t& bytes, std::size_t& align) noexcept {
        if (bytes < sizeof(void*)) bytes = sizeof(void*);
        if (align < alignof(void*)) align = alignof(void*);
    }

    /** @brief The slot serving this exact shape, or `nullptr`. */
    [[nodiscard]] size_class_t* find(std::size_t bytes, std::size_t align) noexcept {
        for (std::size_t i = 0; i < n_; ++i) {
            if (cls_[i].bytes == bytes && cls_[i].align == align) return &cls_[i];
        }
        return nullptr;
    }

    /** @brief Take the next free slot for this shape, or `nullptr` when the span is full. */
    [[nodiscard]] size_class_t* claim(std::size_t bytes, std::size_t align) noexcept {
        if (n_ == cls_.size()) return nullptr;
        cls_[n_] = size_class_t{bytes, align, nullptr};
        return &cls_[n_++];
    }

    std::span<std::byte> buf_;
    std::span<size_class_t> cls_;
    std::size_t used_ = 0;
    std::size_t n_ = 0;
    std::size_t overflow_ = 0;
    std::size_t refused_ = 0;         /**< @brief `try_alloc` answers of `nullptr`. */
    std::size_t largest_refused_ = 0; /**< @brief Bytes of the largest of those. */
    [[no_unique_address]] Sync sync_{};
};

/**
 * @brief The core's failable vector: a nothrow growable array of @p T drawn from a
 *        @ref block_source_t (ADR-0083 Decision 2, #1776).
 *
 * The container a failable path uses where a `std::vector` or `std::pmr::vector` would
 * otherwise sit (#551 Q2, #588). Three properties carry the whole point:
 *
 * 1. **Growth reports refusal by value and never throws.** `std::pmr::vector::push_back` on
 *    an exhausted resource throws, which on ESP-IDF reaches the link-wrapped `__cxa_throw`
 *    `abort()` stub — a peer-reachable reboot when the container sits on the RX decode path.
 *    Here every growing call answers `false` or `nullptr`, and a refused call leaves the
 *    array exactly as it was: same elements, same block, and an argument passed by rvalue
 *    is not consumed.
 * 2. **Every byte comes from the injected source.** Nothing reaches the global heap, so the
 *    array is usable on a static-arena node with no heap at all.
 * 3. **Relocation is a `memcpy` for a trivially copyable `T`.** That case needs no move
 *    loop and no destruction, and it is the shape the hot users have (`wire::arena_tlv_t`,
 *    the walk's open-node record). Any other `T` is relocated by move construction, which
 *    must be `noexcept`, and destroyed in place; that branch is compiled only for such a `T`,
 *    so a trivially copyable array generates the same code it always did.
 *
 * Same footprint as `std::pmr::vector` (four words), one virtual call per growth instead of
 * the allocator's two. Non-copyable: a copy is a failable allocation, so it is not hidden in
 * a constructor.
 */
template <class T>
class block_array_t {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "block_array_t relocates by move construction, which must be noexcept");
    static_assert(std::is_nothrow_destructible_v<T>, "block_array_t: ~T must be noexcept");

    /** @brief True when growth relocates by `memcpy` and leaves nothing to destroy. */
    static constexpr bool kTrivial =
        std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

   public:
    /** @brief The element type. */
    using value_type = T;
    /** @brief A mutable element iterator (a plain pointer: the storage is contiguous). */
    using iterator = T*;
    /** @brief A read-only element iterator. */
    using const_iterator = const T*;

    /** @brief An empty array that will draw its storage from @p src. */
    explicit block_array_t(block_source_t& src) noexcept : src_(&src) {}
    /** @brief Destroys the elements and returns the block, if one was taken. */
    ~block_array_t() { give_back(); }

    /** @brief Non-copyable — one array, one block. */
    block_array_t(const block_array_t&) = delete;
    /** @brief Non-assignable. */
    block_array_t& operator=(const block_array_t&) = delete;
    /** @brief Move-constructible so a decode can return its arena by value. */
    block_array_t(block_array_t&& o) noexcept
        : src_(o.src_), data_(o.data_), end_(o.end_), cap_(o.cap_) {
        o.data_ = o.end_ = o.cap_ = nullptr;
    }
    /** @brief Move-assignable (destroys and releases this array's contents first). */
    block_array_t& operator=(block_array_t&& o) noexcept {
        if (this != &o) {
            give_back();
            src_ = o.src_;
            data_ = o.data_;
            end_ = o.end_;
            cap_ = o.cap_;
            o.data_ = o.end_ = o.cap_ = nullptr;
        }
        return *this;
    }

    /**
     * @brief Ensure room for @p n elements without growing again.
     * @retval false The source is exhausted — the array is unchanged.
     */
    [[nodiscard]] bool reserve(std::size_t n) noexcept {
        return n <= static_cast<std::size_t>(cap_ - data_) || regrow(n);
    }

    /**
     * @brief Append a copy of @p v.
     *
     * @warning For a trivially copyable `T`, @p v must not refer to an element of this
     *          array: growth releases the old block before the copy is read. Use
     *          @ref emplace_back, which builds in the fresh block first, when it might.
     *
     * @retval false The source is exhausted — the array is unchanged (BACKPRESSURE).
     */
    [[nodiscard]] bool push_back(const T& v) noexcept {
        if constexpr (kTrivial) {
            // The shape every trivially copyable caller compiled against before #1776, kept
            // byte for byte: the symbol ratchet pins hot functions that inline it.
            if (end_ == cap_ && !grow()) return false;
            *end_++ = v;
        } else {
            if (end_ == cap_) return grow_emplace(size(), v) != nullptr;
            ::new (static_cast<void*>(end_)) T(v);
            ++end_;
        }
        return true;
    }

    /**
     * @brief Append @p v by move.
     *
     * Offered only for a `T` that is not trivially copyable: for one that is, a move is a
     * copy, and an rvalue keeps binding to the copying overload and its pre-#1776 code.
     *
     * @retval false The source is exhausted — the array is unchanged and @p v is not moved
     *               from.
     */
    [[nodiscard]] bool push_back(T&& v) noexcept
        requires(!kTrivial)
    {
        return emplace_back(std::move(v)) != nullptr;
    }

    /**
     * @brief Construct one element at the end from @p args and return it.
     *
     * An argument may refer to an element of this array: on growth the new element is
     * built in the fresh block before the old one is released.
     *
     * @retval nullptr The source is exhausted — the array is unchanged and no argument is
     *                 moved from (BACKPRESSURE).
     */
    template <class... Args>
    [[nodiscard]] T* emplace_back(Args&&... args) noexcept {
        if (end_ == cap_) return grow_emplace(size(), std::forward<Args>(args)...);
        T* slot = ::new (static_cast<void*>(end_)) T(std::forward<Args>(args)...);
        ++end_;
        return slot;
    }

    /**
     * @brief Construct one element at index @p i from @p args, shifting the tail up by one.
     *
     * The insertion the sorted map is built on. Precondition: `i <= size()`.
     *
     * @retval nullptr The source is exhausted — the array is unchanged and no argument is
     *                 moved from (BACKPRESSURE).
     */
    template <class... Args>
    [[nodiscard]] T* emplace_at(std::size_t i, Args&&... args) noexcept {
        if (end_ == cap_) return grow_emplace(i, std::forward<Args>(args)...);
        if (data_ + i == end_) return emplace_back(std::forward<Args>(args)...);
        // Build first: an argument may alias an element the shift is about to move.
        T made(std::forward<Args>(args)...);
        if constexpr (kTrivial) {
            std::memmove(data_ + i + 1, data_ + i,
                         static_cast<std::size_t>(end_ - (data_ + i)) * sizeof(T));
            ++end_;
            std::memcpy(static_cast<void*>(data_ + i), &made, sizeof(T));
        } else {
            ::new (static_cast<void*>(end_)) T(std::move(end_[-1]));
            for (T* p = end_ - 1; p != data_ + i; --p) *p = std::move(p[-1]);
            ++end_;
            data_[i] = std::move(made);
        }
        return data_ + i;
    }

    /**
     * @brief Claim one uninitialized slot at the end and return it — fill it IN PLACE.
     * @retval nullptr The source is exhausted — the array is unchanged (BACKPRESSURE).
     *
     * The form the hot paths use. `push_back(T{...})` has to materialize the aggregate on
     * the stack and copy it in, and for a 48-byte `T` written field-by-field then read back
     * as wide loads that is a store-forwarding stall on every element: measured on the
     * terminus decode, ~45 % slower with FEWER instructions executed. Writing through this
     * slot removes the temporary entirely. Offered only for a trivially copyable `T`, whose
     * lifetime the caller's stores begin; any other `T` uses @ref emplace_back.
     */
    [[nodiscard]] T* push_slot() noexcept
        requires kTrivial
    {
        if (end_ == cap_ && !grow()) return nullptr;
        return end_++;
    }

    /** @brief Drop the last element. Precondition: not empty. */
    void pop_back() noexcept {
        --end_;
        if constexpr (!kTrivial) end_->~T();
    }
    /** @brief Remove element @p i, shifting the tail down by one. Precondition: `i < size()`. */
    void erase_at(std::size_t i) noexcept {
        if constexpr (kTrivial) {
            std::memmove(data_ + i, data_ + i + 1,
                         static_cast<std::size_t>(end_ - (data_ + i + 1)) * sizeof(T));
            --end_;
        } else {
            for (T* p = data_ + i; p + 1 != end_; ++p) *p = std::move(p[1]);
            pop_back();
        }
    }
    /** @brief Destroy every element; the block is kept for reuse. */
    void clear() noexcept {
        destroy_all();
        end_ = data_;
    }
    /** @brief The last element. Precondition: not empty. */
    [[nodiscard]] T& back() noexcept { return end_[-1]; }
    /** @brief The last element (const). Precondition: not empty. */
    [[nodiscard]] const T& back() const noexcept { return end_[-1]; }
    /** @brief The first element. Precondition: not empty. */
    [[nodiscard]] T& front() noexcept { return *data_; }
    /** @brief The first element (const). Precondition: not empty. */
    [[nodiscard]] const T& front() const noexcept { return *data_; }
    /** @brief Element @p i, unchecked. */
    [[nodiscard]] T& operator[](std::size_t i) noexcept { return data_[i]; }
    /** @brief Element @p i, unchecked (const). */
    [[nodiscard]] const T& operator[](std::size_t i) const noexcept { return data_[i]; }
    /** @brief Element count. */
    [[nodiscard]] std::size_t size() const noexcept {
        return static_cast<std::size_t>(end_ - data_);
    }
    /** @brief Elements the current block holds before the next growth. */
    [[nodiscard]] std::size_t capacity() const noexcept {
        return static_cast<std::size_t>(cap_ - data_);
    }
    /** @brief True when no elements are held. */
    [[nodiscard]] bool empty() const noexcept { return end_ == data_; }
    /**
     * @brief First element, or `nullptr` when empty — the contiguous block.
     *
     * For handing the array to an API that takes a pointer/length pair, e.g. building a
     * `std::span` over an egress iov table. The pointer is invalidated by any growth.
     */
    [[nodiscard]] T* data() noexcept { return data_; }
    /** @brief First element (const), or `nullptr` when empty. */
    [[nodiscard]] const T* data() const noexcept { return data_; }
    /** @brief Iterator to the first element; invalidated by any growth. */
    [[nodiscard]] iterator begin() noexcept { return data_; }
    /** @brief Iterator past the last element. */
    [[nodiscard]] iterator end() noexcept { return end_; }
    /** @brief Read-only iterator to the first element. */
    [[nodiscard]] const_iterator begin() const noexcept { return data_; }
    /** @brief Read-only iterator past the last element. */
    [[nodiscard]] const_iterator end() const noexcept { return end_; }
    /** @brief The source this array draws from. */
    [[nodiscard]] block_source_t& source() const noexcept { return *src_; }

   private:
    /** @brief Destroy every element in place (nothing to do for a trivial `T`). */
    void destroy_all() noexcept {
        if constexpr (!kTrivial) {
            for (T* p = data_; p != end_; ++p) p->~T();
        }
    }

    /** @brief Destroy the elements and return the block to the source. */
    void give_back() noexcept {
        destroy_all();
        release_block();
    }

    /** @brief Return the block alone; the elements are already gone or relocated. */
    void release_block() noexcept {
        if (data_ != nullptr)
            src_->release(data_, static_cast<std::size_t>(cap_ - data_) * sizeof(T), alignof(T));
    }

    /** @brief The doubling policy: 8 elements first, then twice the current block. */
    [[nodiscard]] std::size_t next_capacity() const noexcept {
        const std::size_t have = capacity();
        return have < 4 ? 8 : have * 2;
    }

    // The cold half of push_slot, kept OUT OF LINE: growth happens once or twice per
    // terminus decode, and inlining it into the caller doubles the hot loop's live range.
    /** @brief Grow by the doubling policy. */
    [[gnu::noinline]] [[nodiscard]] bool grow() noexcept { return regrow(next_capacity()); }

    /** @brief Move elements `[first, last)` into the uninitialized @p to and end their lives. */
    static void relocate(T* first, T* last, T* to) noexcept {
        if constexpr (kTrivial) {
            if (first != last)
                std::memcpy(static_cast<void*>(to), first,
                            static_cast<std::size_t>(last - first) * sizeof(T));
        } else {
            for (; first != last; ++first, ++to) {
                ::new (static_cast<void*>(to)) T(std::move(*first));
                first->~T();
            }
        }
    }

    /** @brief Adopt @p fresh (room for @p want elements, @p n of them live) as the block. */
    void adopt(T* fresh, std::size_t want, std::size_t n) noexcept {
        release_block();
        data_ = fresh;
        end_ = fresh + n;
        cap_ = fresh + want;
    }

    /**
     * @brief Relocate into a block of @p want elements.
     * @retval false The source refused — the array is untouched, which is what lets every
     *               caller treat exhaustion as a clean reject.
     */
    [[gnu::noinline]] [[nodiscard]] bool regrow(std::size_t want) noexcept {
        T* fresh = static_cast<T*>(src_->try_alloc(want * sizeof(T), alignof(T)));
        if (fresh == nullptr) return false;
        const std::size_t n = size();
        relocate(data_, end_, fresh);
        adopt(fresh, want, n);
        return true;
    }

    /**
     * @brief The growing half of `emplace_at`: take a bigger block, build the new element
     *        at @p i in it FIRST (an argument may alias an old element), then relocate the
     *        old elements around it and release the old block.
     * @retval nullptr The source refused — the array and the arguments are untouched.
     */
    template <class... Args>
    [[gnu::noinline]] [[nodiscard]] T* grow_emplace(std::size_t i, Args&&... args) noexcept {
        const std::size_t want = next_capacity();
        T* fresh = static_cast<T*>(src_->try_alloc(want * sizeof(T), alignof(T)));
        if (fresh == nullptr) return nullptr;
        const std::size_t n = size();
        T* slot = ::new (static_cast<void*>(fresh + i)) T(std::forward<Args>(args)...);
        relocate(data_, data_ + i, fresh);
        relocate(data_ + i, end_, fresh + i + 1);
        adopt(fresh, want, n + 1);
        return slot;
    }

    block_source_t* src_; /**< @brief Where every block comes from; never null. */
    T* data_ = nullptr;   /**< @brief The block, or null before the first growth. */
    T* end_ = nullptr;    /**< @brief One past the last live element. */
    T* cap_ = nullptr;    /**< @brief One past the block's last slot. */
};

}  // namespace tr::mem
