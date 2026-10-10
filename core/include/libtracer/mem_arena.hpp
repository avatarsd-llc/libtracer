/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_arena — the MCU default behind the one allocation seam (ADR-0083 Decisions 4 and 6,
 * #1783): a static arena sized at compile time, carved into size-classed free lists, with no
 * heap behind it.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <new>
#include <span>

#include "libtracer/config.hpp"
#include "libtracer/guard.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/placement.hpp"

/**
 * @file
 * @brief The static arena (`tr::mem::arena_t`), its size-classed sub-pool
 *        (`tr::mem::arena_pool_t`) and the MCU default root (`tr::mem::arena_root_t`).
 */

namespace tr::mem {

/**
 * @brief A fixed region handed out front to back, never given back (ADR-0083 Decision 6: the
 *        MCU arena never trims).
 *
 * The region is the caller's: on the MCU default root it is a static array of
 * `config_t::kArenaBytes`, so the linker map shows exactly what the node holds. The arena only
 * moves its cursor; the @ref arena_pool_t sub-pools above it recycle what they carved.
 *
 * @tparam Sync The lock around the cursor, a `tr::lockable`; `tr::no_guard_t` costs nothing.
 */
template <::tr::lockable Sync>
class arena_t {
   public:
    /** @brief An arena over @p region, which must outlive it. */
    explicit constexpr arena_t(std::span<std::byte> region) noexcept
        : base_(region.data()), size_(region.size()) {}

    arena_t(const arena_t&) = delete;
    arena_t& operator=(const arena_t&) = delete;

    /** @brief @p bytes from the front of what is left, aligned to @p align (a power of two);
     *         `nullptr` when they do not fit. */
    [[nodiscard]] void* carve(std::size_t bytes, std::size_t align) noexcept {
        lock_.lock();
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(base_);
        const std::size_t at =
            static_cast<std::size_t>(((base + used_ + align - 1) & ~(align - 1)) - base);
        void* p = nullptr;
        if (at <= size_ && bytes <= size_ - at) {
            used_ = at + bytes;
            p = base_ + at;
        }
        lock_.unlock();
        return p;
    }

    /** @brief The region's size in bytes. */
    [[nodiscard]] std::size_t capacity() const noexcept { return size_; }

    /** @brief Bytes carved so far, alignment padding included. */
    [[nodiscard]] std::size_t used() const noexcept {
        lock_.lock();
        const std::size_t n = used_;
        lock_.unlock();
        return n;
    }

   private:
    std::byte* base_;      /**< @brief The region's first byte. */
    std::size_t size_;     /**< @brief The region's size. */
    std::size_t used_ = 0; /**< @brief Bytes carved so far. */
    mutable Sync lock_{};  /**< @brief Guards `used_`. */
};

/**
 * @brief A size-classed sub-pool over an @ref arena_t, with one free list per row of the
 *        size-class table, and fresh blocks carved from the arena (ADR-0083 Decisions 3 and 4,
 *        #1783).
 *
 * A request takes the smallest class that holds it. A block comes off that class's free list,
 * or is carved from the arena when the list is empty, and goes back on the list when it is
 * released. Nothing ever returns to the arena, so what a class has carved stays that class's:
 * the RAM map is fixed at build time, and a node sizes the arena against its peak per class.
 *
 * - **Bounded.** Exhaustion is a `nullptr`, counted in @ref stats, and the caller answers
 *   BACKPRESSURE, or, at init, stops with a sizing message (`exhausted_at_init`).
 * - **No header.** A block's class comes from the sized `release`, as on every block source.
 * - **Oversize.** A request larger than the last row is carved at its own size, rounded up
 *   to @ref kMinAlign, and on release goes onto one oversize list, where only a request of
 *   exactly that rounded size takes it again. That serves the fixed-size large buffers a
 *   node actually asks for (a datagram receive buffer, a frame flatten) without a class row
 *   for each.
 * - **Alignment.** Every block of a class is carved at the largest power of two that divides
 *   its size, up to @ref kMaxAlign, so any request the class accepts finds its alignment on
 *   any block of it. An oversize block is carved at @ref kMaxAlign. A request aligned past
 *   @ref kMaxAlign is refused.
 * - **Locking.** One lock of type @p Sync for the pool, and the arena's own around the
 *   cursor. On a single-threaded node `tr::no_guard_t` removes both.
 * - **Caller storage.** The size-class table and the free-list heads are the caller's, so a
 *   constant-initialized pool holds only pointers: the heads stay in `.bss` and the table in
 *   `.rodata`, and neither is copied into initialized RAM.
 *
 * @tparam Sync The lock, a `tr::lockable`.
 * @tparam N    Rows in the size-class table.
 */
template <::tr::lockable Sync, std::size_t N>
class arena_pool_t final : public block_source_t {
    static_assert(N >= 1, "an arena pool has at least one size class");

   public:
    /** @brief The alignment every block keeps at least. */
    static constexpr std::size_t kMinAlign = alignof(std::max_align_t);
    /** @brief The highest alignment a block can be asked for. */
    static constexpr std::size_t kMaxAlign = 64;
    /** @brief @ref class_of's answer for a request no class serves. */
    static constexpr std::size_t kNoClass = N;

    /**
     * @brief A pool over @p classes, carving from @p arena.
     *
     * @param name    The census name (`"values"`, `"tables"`, `"net"`).
     * @param classes The size-class table: ascending, every row a multiple of @ref kMinAlign.
     *                It must outlive the pool.
     * @param heads   One free-list head per class, all `nullptr`; the pool's from here on.
     * @param arena   Where blocks are carved; it must outlive the pool.
     */
    constexpr arena_pool_t(const char* name, std::span<const std::size_t, N> classes,
                           std::span<void*, N> heads, arena_t<Sync>& arena) noexcept
        : block_source_t(name), arena_(&arena), bytes_(classes), free_(heads) {}

    /** @brief The class that serves @p bytes at @p align, or @ref kNoClass (an oversize block,
     *         or a refusal past @ref kMaxAlign). A function of the two arguments alone, so
     *         `release` finds the class `try_alloc` chose. */
    [[nodiscard]] std::size_t class_of(std::size_t bytes, std::size_t align) const noexcept {
        if (align > kMaxAlign) return kNoClass;
        std::size_t i = static_cast<std::size_t>(
            std::lower_bound(bytes_.begin(), bytes_.end(), bytes) - bytes_.begin());
        while (i < N && bytes_[i] % align != 0) ++i;
        return i;
    }

    /** @brief The block size of class @p i. */
    [[nodiscard]] std::size_t class_bytes(std::size_t i) const noexcept { return bytes_[i]; }

    /** @brief A block of the smallest class that holds @p bytes, or an oversize block;
     *         `nullptr` when the arena is spent or @p align is past @ref kMaxAlign. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (!tr::detail::probe_hook_ok(bytes)) return nullptr;  // test-only OOM injection
        if (align > kMaxAlign) return refuse(bytes);
        const std::size_t i = class_of(bytes, align);
        if (i == kNoClass) return oversize_alloc(bytes);
        lock_.lock();
        void* p = free_[i];
        if (p != nullptr) {
            std::memcpy(&free_[i], p, sizeof(void*));
            account(bytes_[i]);
        }
        lock_.unlock();
        if (p != nullptr) return p;
        p = arena_->carve(bytes_[i], block_align(bytes_[i]));
        if (p == nullptr) return refuse(bytes);
        lock_.lock();
        account(bytes_[i]);
        lock_.unlock();
        return p;
    }

    /** @brief Return a block to its class's free list, or an oversize block to the oversize
     *         list. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        const std::size_t i = class_of(bytes, align);
        if (i == kNoClass) {
            oversize_release(p, bytes);
            return;
        }
        lock_.lock();
        std::memcpy(p, &free_[i], sizeof(void*));
        free_[i] = p;
        in_use_ -= bytes_[i];
        lock_.unlock();
    }

    /**
     * @brief The census: class bytes handed out and not returned, their high-water mark, and
     *        the refusals. `capacity` is 0: the arena, shared by the sub-pools, is the bound.
     */
    [[nodiscard]] source_stats_t stats() const noexcept override {
        lock_.lock();
        const source_stats_t s{0, in_use_, peak_, refused_, largest_refused_};
        lock_.unlock();
        return s;
    }

    /** @brief Rows in the size-class table. */
    [[nodiscard]] static constexpr std::size_t classes() noexcept { return N; }

   private:
    /** @brief The alignment class blocks of @p bytes are carved at. */
    [[nodiscard]] static constexpr std::size_t block_align(std::size_t bytes) noexcept {
        std::size_t a = kMinAlign;
        while (a < kMaxAlign && bytes % (2 * a) == 0) a *= 2;
        return a;
    }

    /** @brief A returned oversize block: the next one, and its rounded size. */
    struct oversize_t {
        oversize_t* next;  /**< @brief The next returned oversize block. */
        std::size_t bytes; /**< @brief This block's size. */
    };

    /** @brief An oversize block of exactly @p bytes rounded up: a returned one, or carved. */
    [[nodiscard]] void* oversize_alloc(std::size_t bytes) noexcept {
        const std::size_t n = pad_to(bytes, kMinAlign);
        lock_.lock();
        oversize_t** at = &oversize_;
        while (*at != nullptr && (*at)->bytes != n) at = &(*at)->next;
        void* p = *at;
        if (p != nullptr) *at = (*at)->next;
        if (p != nullptr) account(n);
        lock_.unlock();
        if (p != nullptr) return p;
        p = arena_->carve(n, kMaxAlign);
        if (p == nullptr) return refuse(bytes);
        lock_.lock();
        account(n);
        lock_.unlock();
        return p;
    }

    /** @brief Put an oversize block of @p bytes on the oversize list. */
    void oversize_release(void* p, std::size_t bytes) noexcept {
        const std::size_t n = pad_to(bytes, kMinAlign);
        lock_.lock();
        oversize_ = new (p) oversize_t{oversize_, n};
        in_use_ -= n;
        lock_.unlock();
    }

    /** @brief Count @p n bytes out and raise the high-water mark; under the lock. */
    void account(std::size_t n) noexcept {
        in_use_ += n;
        peak_ = std::max(peak_, in_use_);
    }

    /** @brief Count a refused request of @p bytes and answer `nullptr`. Cold arm only. */
    [[gnu::noinline, gnu::cold]] void* refuse(std::size_t bytes) noexcept {
        lock_.lock();
        ++refused_;
        largest_refused_ = std::max(largest_refused_, bytes);
        lock_.unlock();
        return nullptr;
    }

    arena_t<Sync>* arena_;                  /**< @brief Where blocks are carved. */
    std::span<const std::size_t, N> bytes_; /**< @brief Block size per class. */
    std::span<void*, N> free_;        /**< @brief Returned blocks per class, linked in place. */
    oversize_t* oversize_ = nullptr;  /**< @brief Returned oversize blocks. */
    std::size_t in_use_ = 0;          /**< @brief Class bytes out. */
    std::size_t peak_ = 0;            /**< @brief High-water of `in_use_`. */
    std::size_t refused_ = 0;         /**< @brief Requests answered `nullptr`. */
    std::size_t largest_refused_ = 0; /**< @brief Bytes of the largest of those. */
    mutable Sync lock_{};             /**< @brief Guards the lists and the census. */
};

/**
 * @brief The MCU default root (ADR-0083 Decision 4, #1783): one static arena with the value,
 *        table and net sub-pools derived from it.
 *
 * Where `config_t::kSlabPool` is `false`, a `graph_t` constructed without a source uses this
 * root (`tr::mem::default_root()`), over a static array of `config_t::kArenaBytes`: its values
 * come from @ref values, its registration and container blocks from @ref tables, and the router
 * and transport defaults from @ref net when the application injects nothing for them (Q21). No
 * byte of it comes from a heap.
 *
 * As a source of its own the root serves from @ref tables. Its census is the arena's:
 * `capacity` the region, `in_use` and `peak` the bytes carved from it (the arena never takes
 * any back), and the three sub-pools' refusals.
 *
 * @tparam Sync The lock of each sub-pool and of the arena.
 * @tparam N    Rows in the size-class table.
 */
template <::tr::lockable Sync, std::size_t N>
class arena_root_t final : public block_source_t {
   public:
    /** @brief The free-list heads a root's three sub-pools keep. */
    static constexpr std::size_t kHeads = 3 * N;

    /**
     * @brief A root over @p region, with every sub-pool on @p classes.
     *
     * @param region  The arena; it must outlive the root.
     * @param classes The size-class table; it must outlive the root.
     * @param heads   The sub-pools' free-list heads, all `nullptr` (`.bss` on the MCU default).
     */
    constexpr arena_root_t(std::span<std::byte> region, std::span<const std::size_t, N> classes,
                           std::span<void*, kHeads> heads) noexcept
        : block_source_t("arena_root"),
          arena_(region),
          values_("values", classes, heads.template subspan<0, N>(), arena_),
          tables_("tables", classes, heads.template subspan<N, N>(), arena_),
          net_("net", classes, heads.template subspan<2 * N, N>(), arena_) {}

    /** @brief The value sub-pool (`:stats.mem.values`). */
    [[nodiscard]] arena_pool_t<Sync, N>& values() noexcept { return values_; }
    /** @brief The table sub-pool (`:stats.mem.tables`). */
    [[nodiscard]] arena_pool_t<Sync, N>& tables() noexcept { return tables_; }
    /** @brief The net sub-pool (`:stats.mem.net`). */
    [[nodiscard]] arena_pool_t<Sync, N>& net() noexcept { return net_; }

    /** @brief A block from the table sub-pool. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return tables_.try_alloc(bytes, align);
    }
    /** @brief Return a block @ref try_alloc handed out. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tables_.release(p, bytes, align);
    }
    /** @brief The arena's census: its size, the bytes carved, and every sub-pool's refusals. */
    [[nodiscard]] source_stats_t stats() const noexcept override {
        source_stats_t s{arena_.capacity(), arena_.used(), arena_.used(), 0, 0};
        for (const source_stats_t sub : {values_.stats(), tables_.stats(), net_.stats()}) {
            s.refused += sub.refused;
            s.largest_refused = std::max(s.largest_refused, sub.largest_refused);
        }
        return s;
    }

   private:
    arena_t<Sync> arena_;          /**< @brief The region every sub-pool carves from. */
    arena_pool_t<Sync, N> values_; /**< @brief The value sub-pool. */
    arena_pool_t<Sync, N> tables_; /**< @brief The table sub-pool. */
    arena_pool_t<Sync, N> net_;    /**< @brief The net sub-pool. */
};

/**
 * @brief Rows of `config_t::kSizeClasses` the MCU arena keeps: those no larger than
 *        `config_t::kArenaBytes`, and at least one (#2090).
 *
 * A class whose block is larger than the region could never be carved, so its free-list heads
 * would be dead `.bss`. With the default table that is 48 rows at 4 KiB (heads: 3 x 48
 * pointers, 576 B on a 32-bit target) and 72 at 32 KiB (864 B), where all 81 rows cost 972 B. A
 * request above the last kept row is carved as an oversize block at its own size, so the cut
 * refuses nothing the full table served.
 */
inline constexpr std::size_t kArenaClasses = std::max<std::size_t>(
    1, static_cast<std::size_t>(std::upper_bound(std::begin(graph::config_t::kSizeClasses),
                                                 std::end(graph::config_t::kSizeClasses),
                                                 graph::config_t::kArenaBytes) -
                                std::begin(graph::config_t::kSizeClasses)));

/** @brief The MCU default root type: the build's guard, the rows of its size-class table the
 *         arena can carve (#1783, #2090). */
using mcu_root_t = arena_root_t<graph::guard_t, kArenaClasses>;

/**
 * @brief The process-wide MCU default root (@ref mcu_root_t), over a static array of
 *        `config_t::kArenaBytes`.
 *
 * Constant-initialized and never destroyed. Exists only on a build whose `kSlabPool` is
 * `false` and whose `kArenaBytes` is not 0; elsewhere calling it aborts, and nothing in the
 * library does.
 */
[[nodiscard]] mcu_root_t& mcu_root() noexcept;

}  // namespace tr::mem
