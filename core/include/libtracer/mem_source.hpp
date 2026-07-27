/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_source — the L0 nothrow block seam every FAILABLE control-plane
 * allocation draws from (#551). Raw bytes, failure by value, no refcount.
 */
#pragma once

#include <cstddef>
#include <new>

/**
 * @file
 * @brief The nothrow control-plane block seam (`tr::mem::block_source_t`) and the
 *        process-wide platform-heap source that backs it by default.
 */

namespace tr::mem {

/**
 * @brief The nothrow block seam every FAILABLE control-plane allocation draws from
 *        (#551, ADR-0039 erratum 5).
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
 *       and is GONE at `-Os`/`-Oz` — the level the node ships at
 *       (`CONFIG_COMPILER_OPTIMIZATION_SIZE`) and the one level no test executes at.
 *       Inheriting would keep that `allocate()` publicly callable on this object,
 *       one token away from every correct `try_alloc` call site, with no diagnostic
 *       at any warning level. A separate type makes the slip a compile error.
 *
 * @note Also distinct from @ref mem_backend_t, which vends a refcounted
 *       @ref view::segment_t. Control-plane blocks have a single owner and no
 *       header; a refcount on them is pure overhead (a `segment_t` measures 20 B on
 *       rv32 / 40 B on x86-64 against an 80 B `vertex_t`).
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

   private:
    const char* name_; /**< @brief Borrowed literal; never owned. */
};

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

    /** @brief Nothrow aligned heap allocation; `nullptr` on exhaustion. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return ::operator new(bytes, std::align_val_t{align}, std::nothrow);
    }

    /** @brief Sized, aligned reclaim matching @ref try_alloc. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        ::operator delete(p, bytes, std::align_val_t{align});
    }
};

/**
 * @brief The process-wide default @ref block_source_t (the platform heap).
 *
 * A namespace-scope `constinit` object behind a function, NOT a function-local static:
 * the latter costs a `__cxa_guard` word in `.bss` and an acquire fence on every call.
 */
[[nodiscard]] block_source_t& heap_source() noexcept;

}  // namespace tr::mem
