/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_slab_pool — the host default behind the one allocation seam (ADR-0083 Decisions 4, 6
 * and 10, #1777): a size-classed slab pool that asks its root only for whole slabs, and the
 * host default root that derives the value, table and net sub-pools from it.
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <type_traits>

#include "libtracer/config.hpp"
#include "libtracer/guard.hpp"
#include "libtracer/guard_mutex.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/placement.hpp"

#if defined(__SANITIZE_ADDRESS__)
#define LIBTRACER_SLAB_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LIBTRACER_SLAB_ASAN 1
#endif
#endif
#if defined(LIBTRACER_SLAB_ASAN)
#include <sanitizer/asan_interface.h>
#endif

/**
 * @file
 * @brief The size-classed slab pool (`tr::mem::slab_pool_t`) and the host default root
 *        (`tr::mem::host_root_t`) with its three derived sub-pools.
 */

namespace tr::mem {

namespace detail {

template <bool kOn>
struct host_storage_t;

/** @brief Poison @p n bytes at @p p for AddressSanitizer: a freed block reads as freed. */
inline void slab_poison(void* p, std::size_t n) noexcept {
#if defined(LIBTRACER_SLAB_ASAN)
    ASAN_POISON_MEMORY_REGION(p, n);
#else
    (void)p;
    (void)n;
#endif
}

/** @brief Unpoison @p n bytes at @p p: the block is handed out (or its link word is read). */
inline void slab_unpoison(void* p, std::size_t n) noexcept {
#if defined(LIBTRACER_SLAB_ASAN)
    ASAN_UNPOISON_MEMORY_REGION(p, n);
#else
    (void)p;
    (void)n;
#endif
}

/** @brief The intrusive free-list link stored in the first word of a free block. */
[[nodiscard]] inline void* slab_next(void* block) noexcept {
    void* next;
    std::memcpy(&next, block, sizeof(void*));
    return next;
}

/** @brief Store @p next as the free-list link of @p block. */
inline void slab_link(void* block, void* next) noexcept {
    std::memcpy(block, &next, sizeof(void*));
}

}  // namespace detail

/**
 * @brief Whether @p classes is a usable slab-pool table: non-empty, strictly ascending, at most
 *        255 rows, and every row a multiple of `alignof(std::max_align_t)`.
 *
 * `host_root_t` asserts it of `tr::graph::default_config_t::kSizeClasses`, so a fragment that
 * binds a table the pool cannot serve fails the build.
 */
[[nodiscard]] constexpr bool slab_classes_valid(std::span<const std::size_t> classes) noexcept {
    if (!size_classes_valid(classes) || classes.size() > 255) return false;
    for (const std::size_t c : classes)
        if (c == 0 || c % alignof(std::max_align_t) != 0) return false;
    return true;
}

/**
 * @brief One size class's detail, for tuning the size-class table (`kInstrumentCounters`).
 *
 * Not part of `:stats` (RFC-0010 Amendment 3 keeps per-class detail out of the wire surface);
 * a C++ accessor on a build that opts into the counters.
 */
struct slab_class_stats_t {
    std::size_t bytes = 0;    /**< @brief The class's block size. */
    std::size_t slabs = 0;    /**< @brief Slabs the class holds from the root now. */
    std::size_t live = 0;     /**< @brief Blocks out of the class: handed out, or parked in a
                                   thread cache. */
    std::size_t released = 0; /**< @brief Slabs given back to the root, by the cap or a trim. */
    std::size_t rounding = 0; /**< @brief Bytes the live blocks lose to rounding up: the class
                                   size less what each request asked, summed over the blocks
                                   @ref slab_pool_t::try_alloc handed out (#1646). A block a
                                   thread cache holds is live but not in it. */
};

/**
 * @brief A size-classed slab pool: the host default adapter behind the allocation seam
 *        (ADR-0083 Decisions 4, 6 and 10, #1777).
 *
 * A request takes the smallest class of the table that holds it and is served from a SLAB of
 * that class: a block of the root's memory, a power of two in size and aligned to it, carved
 * into equal blocks. The root is asked for whole slabs only, never for one block, so the
 * platform allocator's own size classes stop deciding what a payload costs (the 1 KiB cliff of
 * #1768 was one of them).
 *
 * - **Lazy carving.** A fresh slab is carved one block at a time, so its untouched pages are
 *   address space, not resident memory.
 * - **Release.** Each class keeps at most `cap` fully FREE slabs, however many slabs are live.
 *   A slab whose last block comes back while its class already keeps `cap` free ones is released
 *   to the root at once; otherwise it is kept for the next burst, so an alloc/free pair at a slab
 *   boundary does not draw and release a slab on every turn. @ref trim releases every fully free
 *   slab, on the caller's schedule — the library keeps no timer to do it.
 * - **Oversize.** A request above the last class, or aligned past @ref kHeaderBytes, is its
 *   own block from the root, at its own size, and goes back to the root when freed.
 * - **Bounded.** Over a bounded root, a @ref pool_source_t on a caller slab, the pool is bounded
 *   by that slab and never touches a heap. That is the shape for sizes a PEER chooses (receive
 *   segments, WRITE payloads, label routes; #1646): an exact-size pool gives every distinct
 *   length a class of its own, where this one rounds it up to a row of the table. The root
 *   then sees only slab sizes, a few powers of two, so its exact classes are degenerate again,
 *   and a slab one class frees can serve another.
 * - **Locking.** One lock per class, of type @p Sync. Under `tr::no_guard_t` it is empty and
 *   costs nothing. The slab of a block is found by masking the block's address, and the class
 *   from the sized `release`, so a block carries no header.
 * - **Counting.** The `:stats` census (@ref stats) counts the SLAB bytes this pool takes from its
 *   root — not oversize blocks, which pass through uncounted — and is updated only on the slab
 *   path, never on the block path (`core/STYLE.md`
 *   §Introspection, counting doctrine 1). Per-class detail is @ref class_stats, rounding
 *   waste included, compiled only with @p kCounters.
 *
 * @tparam Sync     The per-class lock, a `tr::lockable`.
 * @tparam N        Rows in the size-class table.
 * @tparam kCounters Whether the per-class block counters of @ref class_stats are kept.
 */
template <::tr::lockable Sync, std::size_t N, bool kCounters = graph::kInstrumentCounters>
class slab_pool_t final : public block_source_t {
    static_assert(N >= 1 && N <= 255, "a slab-pool table has 1 to 255 rows");

   public:
    /** @brief The alignment every class block keeps: the platform allocator's guarantee. */
    static constexpr std::size_t kMinAlign = alignof(std::max_align_t);
    /** @brief Bytes the slab header takes at the head of a slab; also the highest alignment a
     *         class block can be asked for. */
    static constexpr std::size_t kHeaderBytes = 64;
    /** @brief @ref class_of's answer for a request no class serves (an oversize block). */
    static constexpr std::size_t kNoClass = N;
    /** @brief Requests up to this size find their class by one table load. */
    static constexpr std::size_t kLookupBytes = 4096;

    /**
     * @brief A pool over @p classes, drawing slabs from @p root.
     *
     * @param name       The census name (`"values"`, `"tables"`, `"net"`).
     * @param classes    The size-class table; @ref slab_classes_valid must hold of it.
     * @param root       Where slabs come from; it must outlive the pool.
     * @param slab_bytes The base slab size, a power of two of at least 4 KiB.
     * @param cap        The fully free slabs a class keeps (at least 1).
     */
    constexpr slab_pool_t(const char* name, std::span<const std::size_t, N> classes,
                          block_source_t& root, std::size_t slab_bytes = kSlabBytes,
                          std::size_t cap = kSlabClassCap) noexcept
        : block_source_t(name), root_(&root), base_(slab_bytes), cap_(cap < 1 ? 1 : cap) {
        for (std::size_t i = 0; i < N; ++i) {
            bytes_[i] = classes[i];
            std::size_t sb = base_;
            while (sb - kHeaderBytes < 8 * classes[i]) sb *= 2;
            slab_[i] = sb;
        }
        std::size_t c = 0;
        for (std::size_t q = 0; q < lookup_.size(); ++q) {
            while (c < N && bytes_[c] < q * kMinAlign) ++c;
            lookup_[q] = static_cast<std::uint8_t>(c);
        }
    }

    /** @brief Returns every slab to the root, live blocks or not: the pool's blocks die with it. */
    ~slab_pool_t() {
        for (std::size_t i = 0; i < N; ++i) {
            while (slab_t* s = cls_[i].all) {
                unlink_all(cls_[i], s);
                free_slab(s, i);
            }
        }
    }

    slab_pool_t(const slab_pool_t&) = delete;
    slab_pool_t& operator=(const slab_pool_t&) = delete;

    /**
     * @brief The class that serves @p bytes at @p align: the smallest row that holds it and
     *        keeps the alignment, or @ref kNoClass.
     *
     * A function of the two arguments alone, so `release` finds the class `try_alloc` chose.
     */
    [[nodiscard]] std::size_t class_of(std::size_t bytes, std::size_t align) const noexcept {
        if (align > kMinAlign) {
            if (align > kHeaderBytes) return kNoClass;
            bytes = pad_to(bytes, align);
        }
        // Past the last row first: an oversize request pays one compare, not a search.
        if (bytes > bytes_[N - 1]) return kNoClass;
        std::size_t i =
            bytes <= kLookupBytes
                ? lookup_[(bytes + kMinAlign - 1) / kMinAlign]
                : static_cast<std::size_t>(std::lower_bound(bytes_.begin(), bytes_.end(), bytes) -
                                           bytes_.begin());
        if (align > kMinAlign)
            while (i < N && bytes_[i] % align != 0) ++i;
        return i;
    }

    /** @brief The block size of class @p i. */
    [[nodiscard]] std::size_t class_bytes(std::size_t i) const noexcept { return bytes_[i]; }

    /** @brief A block of the smallest class that holds @p bytes; `nullptr` when the root
     *         refuses a slab. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (!tr::detail::probe_hook_ok(bytes)) return nullptr;  // test-only OOM injection
        const std::size_t i = class_of(bytes, align);
        if (i == kNoClass) return oversize_alloc(bytes, align);
        void* p = nullptr;
        if (take(i, &p, 1, bytes) == 1) count_rounding(i, bytes_[i] - bytes, true);
        return p;
    }

    /** @brief Return a block @ref try_alloc handed out, sized as asked. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        const std::size_t i = class_of(bytes, align);
        if (i == kNoClass) {
            oversize_release(p, bytes, align);
            return;
        }
        give(i, &p, 1);
        count_rounding(i, bytes_[i] - bytes, false);
    }

    /**
     * @brief Take up to @p n blocks of class @p i under one hold of its lock — the refill of a
     *        thread cache, or one `try_alloc`.
     *
     * @param request The caller's request, for the refusal census.
     * @return The blocks written to @p out: @p n, unless the root refused a slab, then fewer
     *         (0 when not even one block could be served).
     */
    [[nodiscard]] std::size_t take(std::size_t i, void** out, std::size_t n,
                                   std::size_t request) noexcept {
        class_t& c = cls_[i];
        c.lock.lock();
        std::size_t got = 0;
        while (got < n) {
            slab_t* s = c.head;
            if (s == nullptr && (s = grow(c, i, request)) == nullptr) break;
            if (s->live == 0) --c.empty;  // a kept free slab is in use again
            void* b;
            if (s->free != nullptr) {
                b = s->free;
                detail::slab_unpoison(b, sizeof(void*));
                s->free = detail::slab_next(b);
            } else {
                b = reinterpret_cast<std::byte*>(s) + kHeaderBytes + s->carved * bytes_[i];
                ++s->carved;
            }
            detail::slab_unpoison(b, bytes_[i]);
            if (++s->live == s->capacity) unlink(c, s);
            out[got++] = b;
        }
        if constexpr (kCounters) c.live += got;
        c.lock.unlock();
        return got;
    }

    /** @brief Return @p n blocks of class @p i under one hold of its lock. */
    void give(std::size_t i, void* const* blocks, std::size_t n) noexcept {
        class_t& c = cls_[i];
        c.lock.lock();
        for (std::size_t k = 0; k < n; ++k) put(c, i, blocks[k]);
        if constexpr (kCounters) c.live -= n;
        c.lock.unlock();
    }

    /**
     * @brief Release every fully free slab to the root, whatever the cap (ADR-0083 Decision 6).
     *
     * The application calls it on its own schedule: after a burst, on a low-memory signal.
     * Blocks a thread cache holds keep their slab.
     */
    void trim() noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            class_t& c = cls_[i];
            c.lock.lock();
            // Fully free slabs are the tail of the list (see `put`), so the walk stops at the
            // first slab with a live block.
            while (c.tail != nullptr && c.tail->live == 0) {
                --c.empty;
                release_slab(c, i, c.tail);
            }
            c.lock.unlock();
        }
    }

    /**
     * @brief This pool's census, in the `core/STYLE.md` §Introspection vocabulary.
     *
     * `in_use` is the SLAB bytes held from the root: what the slabs cost the node, never less
     * than their live blocks. An oversize block (past the last class) is NOT in it — it passes
     * straight through to the root uncounted, so a node holding large values holds more than
     * `in_use` says; its refusals are still counted. `peak` is its
     * high-water mark, `refused` the requests answered `nullptr` because the root refused a slab,
     * and `largest_refused` the biggest of those requests. `capacity` is 0: the pool caps
     * retention, not demand. Sampled with relaxed loads (the snapshot-coherence clause).
     */
    [[nodiscard]] source_stats_t stats() const noexcept override {
        return source_stats_t{0, in_use_.load(std::memory_order_relaxed),
                              peak_.load(std::memory_order_relaxed),
                              refused_.load(std::memory_order_relaxed),
                              largest_refused_.load(std::memory_order_relaxed)};
    }

    /** @brief Class @p i's detail (@ref slab_class_stats_t), read under its lock. */
    [[nodiscard]] slab_class_stats_t class_stats(std::size_t i) noexcept
        requires kCounters
    {
        class_t& c = cls_[i];
        c.lock.lock();
        const slab_class_stats_t s{bytes_[i], c.slabs, c.live, c.released, rounding_[i]};
        c.lock.unlock();
        return s;
    }

    /** @brief Rows in the size-class table. */
    [[nodiscard]] static constexpr std::size_t classes() noexcept { return N; }

    /** @brief The slab size class @p i draws from the root. */
    [[nodiscard]] std::size_t slab_bytes(std::size_t i) const noexcept { return slab_[i]; }

   private:
    /** @brief A slab's header, at its head. Free blocks link through their first word. */
    struct slab_t {
        slab_t* prev = nullptr;     /**< @brief Previous slab on the class's free-block list. */
        slab_t* next = nullptr;     /**< @brief Next slab on that list. */
        slab_t* all_prev = nullptr; /**< @brief Previous slab of the class, listed or not. */
        slab_t* all_next = nullptr; /**< @brief Next slab of the class. */
        void* free = nullptr;       /**< @brief Returned blocks of this slab. */
        std::uint32_t carved = 0;   /**< @brief Blocks carved so far (lazily, in order). */
        std::uint32_t live = 0;     /**< @brief Blocks out of this slab. */
        std::uint32_t capacity = 0; /**< @brief Blocks this slab holds. */
        bool listed = false;        /**< @brief On the free-block list (it has a block to give). */
        bool doubled = false;       /**< @brief Cut from a block twice its size (see `grow`). */
        void* raw = nullptr;        /**< @brief What the root handed out (see `grow`). */
    };
    static_assert(sizeof(slab_t) <= kHeaderBytes, "the slab header fits its reserve");

    /** @brief One size class: its lock, the slabs with a block to give, and its census. */
    struct class_t {
        [[no_unique_address]] Sync lock{}; /**< @brief The class's one lock. */
        slab_t* head = nullptr;            /**< @brief Slabs with a free block; fully free last. */
        slab_t* tail = nullptr;            /**< @brief The list's last slab. */
        slab_t* all = nullptr;             /**< @brief Every slab of the class. */
        std::size_t slabs = 0;             /**< @brief Slabs held. */
        std::size_t empty = 0;    /**< @brief Of those, fully free (the cap counts these). */
        std::size_t live = 0;     /**< @brief Blocks out (only with `kCounters`). */
        std::size_t released = 0; /**< @brief Slabs released (cap or trim). */
    };

    /** @brief The slab block @p p of class @p i was carved from: its address, masked. */
    [[nodiscard]] slab_t* slab_of(void* p, std::size_t i) const noexcept {
        return reinterpret_cast<slab_t*>(reinterpret_cast<std::uintptr_t>(p) & ~(slab_[i] - 1));
    }

    /** @brief Return one block to its slab; release the slab when it empties while its class
     *         already keeps `cap` free ones. */
    void put(class_t& c, std::size_t i, void* p) noexcept {
        slab_t* const s = slab_of(p, i);
        detail::slab_link(p, s->free);
        detail::slab_poison(static_cast<std::byte*>(p) + sizeof(void*), bytes_[i] - sizeof(void*));
        s->free = p;
        if (--s->live != 0) {
            if (!s->listed) push_front(c, s);  // it was full: it has a block to give again
            return;
        }
        if (s->listed) unlink(c, s);
        if (c.empty >= cap_) {
            release_slab(c, i, s, false);
            return;
        }
        ++c.empty;
        push_back(c, s);  // fully free slabs gather at the tail, where `trim` finds them
    }

    /** @brief Draw a fresh slab for class @p i; `nullptr` (counted) when the root refuses. */
    [[nodiscard]] slab_t* grow(class_t& c, std::size_t i, std::size_t request) noexcept {
        // A root that does not honour the slab's alignment (a replaced `operator new` that
        // ignores `align_val_t`, an arena that cannot align so far) would put the header a
        // mask away from where `slab_of` looks for it. Such a draw goes back, and the slab is
        // cut from a block twice its size instead.
        void* raw = root_->try_alloc(slab_[i], slab_[i]);
        const bool doubled =
            raw != nullptr && (reinterpret_cast<std::uintptr_t>(raw) & (slab_[i] - 1)) != 0;
        if (doubled) {
            root_->release(raw, slab_[i], slab_[i]);
            raw = root_->try_alloc(2 * slab_[i], kMinAlign);
        }
        if (raw == nullptr) {
            count_refusal(request);
            return nullptr;
        }
        void* const m = reinterpret_cast<void*>(
            (reinterpret_cast<std::uintptr_t>(raw) + slab_[i] - 1) & ~(slab_[i] - 1));
        auto* const s = new (m) slab_t{};
        s->raw = raw;
        s->doubled = doubled;
        s->capacity = static_cast<std::uint32_t>((slab_[i] - kHeaderBytes) / bytes_[i]);
        detail::slab_poison(static_cast<std::byte*>(m) + kHeaderBytes, slab_[i] - kHeaderBytes);
        s->all_next = c.all;
        if (c.all != nullptr) c.all->all_prev = s;
        c.all = s;
        ++c.slabs;
        ++c.empty;  // `take` counts it back down as it hands out the first block
        push_front(c, s);
        account(slab_[i]);
        return s;
    }

    /** @brief Give slab @p s back to the root; @p listed says whether it is on the list. */
    void release_slab(class_t& c, std::size_t i, slab_t* s, bool listed = true) noexcept {
        if (listed) unlink(c, s);
        unlink_all(c, s);
        --c.slabs;
        ++c.released;
        free_slab(s, i);
        unaccount(slab_[i]);
    }

    /** @brief Hand slab @p s of class @p i back to the root, as `grow` drew it. */
    void free_slab(slab_t* s, std::size_t i) noexcept {
        void* const raw = s->raw;
        const bool doubled = s->doubled;
        detail::slab_unpoison(s, slab_[i]);
        // By the flag, not by `raw == s`: a doubled block the root happened to align to the
        // slab starts where its slab does, and still goes back at the size it was drawn.
        if (doubled)
            root_->release(raw, 2 * slab_[i], kMinAlign);
        else
            root_->release(raw, slab_[i], slab_[i]);
    }

    /** @brief Put @p s at the head of the free-block list. */
    static void push_front(class_t& c, slab_t* s) noexcept {
        s->prev = nullptr;
        s->next = c.head;
        if (c.head != nullptr) c.head->prev = s;
        c.head = s;
        if (c.tail == nullptr) c.tail = s;
        s->listed = true;
    }

    /** @brief Put @p s at the tail of the free-block list. */
    static void push_back(class_t& c, slab_t* s) noexcept {
        s->next = nullptr;
        s->prev = c.tail;
        if (c.tail != nullptr) c.tail->next = s;
        c.tail = s;
        if (c.head == nullptr) c.head = s;
        s->listed = true;
    }

    /** @brief Take @p s off the free-block list. */
    static void unlink(class_t& c, slab_t* s) noexcept {
        (s->prev != nullptr ? s->prev->next : c.head) = s->next;
        (s->next != nullptr ? s->next->prev : c.tail) = s->prev;
        s->listed = false;
    }

    /** @brief Take @p s off the class's list of every slab. */
    static void unlink_all(class_t& c, slab_t* s) noexcept {
        (s->all_prev != nullptr ? s->all_prev->all_next : c.all) = s->all_next;
        if (s->all_next != nullptr) s->all_next->all_prev = s->all_prev;
    }

   public:
    /**
     * @brief An oversize request: its own block from the root, at its own size. Public for a
     *        front end that has already consulted the test probe (the host value cache), so
     *        one request consumes it once.
     *
     * Not rounded up to slabs or pages: rounding a value just past the last class up to
     * whole 64 KiB slabs asked glibc for 128 KiB, its mmap threshold, and a 64 KiB
     * `heap_backend` draw went from 26 ns to 2.5 us on bench-local; whole pages still moved
     * the heap top past its trim threshold on every free. At its own size the block is what the
     * platform allocator saw before the pool existed.
     *
     * Nor is it in `in_use`: it passes straight through to the root, and counting it (one
     * atomic add on the draw, one subtract on the release) cost a 64 KiB draw +8 ns on
     * bench-local (20.9 -> 28.7 ns), more than the platform allocator's own 26 ns before the
     * pool. A refusal is still counted.
     */
    [[nodiscard]] void* oversize_alloc(std::size_t bytes, std::size_t align) noexcept {
        const std::size_t n = pad_to(bytes, kMinAlign);
        void* const p = root_->try_alloc(n, std::max(align, kMinAlign));
        if (p == nullptr) count_refusal(bytes);
        return p;
    }

    /** @brief Return an oversize block, sized as `oversize_alloc` drew it. */
    void oversize_release(void* p, std::size_t bytes, std::size_t align) noexcept {
        const std::size_t n = pad_to(bytes, kMinAlign);
        root_->release(p, n, std::max(align, kMinAlign));
    }

   private:
    /** @brief Count the @p bytes one block of class @p i loses to rounding, as it goes out
     *         (@p out) or comes back. Only with `kCounters`: a build without them keeps the
     *         block path exactly as it was. */
    void count_rounding(std::size_t i, std::size_t bytes, bool out) noexcept {
        if constexpr (kCounters) {
            class_t& c = cls_[i];
            c.lock.lock();
            rounding_[i] = out ? rounding_[i] + bytes : rounding_[i] - bytes;
            c.lock.unlock();
        } else {
            (void)i;
            (void)bytes;
            (void)out;
        }
    }

    /** @brief Add @p n slab bytes to the census and raise the high-water mark. */
    void account(std::size_t n) noexcept {
        const std::size_t now = in_use_.fetch_add(n, std::memory_order_relaxed) + n;
        std::size_t seen = peak_.load(std::memory_order_relaxed);
        while (now > seen && !peak_.compare_exchange_weak(seen, now, std::memory_order_relaxed)) {
        }
    }

    /** @brief Take @p n slab bytes off the census. */
    void unaccount(std::size_t n) noexcept { in_use_.fetch_sub(n, std::memory_order_relaxed); }

    /** @brief Record one refused request (the root refused a slab). Cold arm only. */
    [[gnu::noinline, gnu::cold]] void count_refusal(std::size_t bytes) noexcept {
        refused_.fetch_add(1, std::memory_order_relaxed);
        std::size_t seen = largest_refused_.load(std::memory_order_relaxed);
        while (bytes > seen &&
               !largest_refused_.compare_exchange_weak(seen, bytes, std::memory_order_relaxed)) {
        }
    }

    block_source_t* root_;               /**< @brief Where slabs come from. */
    std::size_t base_;                   /**< @brief The base slab size. */
    std::size_t cap_;                    /**< @brief Fully free slabs a class keeps. */
    std::array<std::size_t, N> bytes_{}; /**< @brief Block size per class. */
    std::array<std::size_t, N> slab_{};  /**< @brief Slab size per class. */
    std::array<std::uint8_t, kLookupBytes / kMinAlign + 1> lookup_{}; /**< @brief 16 B -> class. */
    std::array<class_t, N> cls_{};                                    /**< @brief The classes. */
    /** @brief The empty stand-in for `rounding_` in a build without `kCounters`. */
    struct no_rounding_t {};
    /** @brief Rounding bytes of each class's live blocks; no storage without `kCounters`, so
     *         the pool keeps its size. */
    [[no_unique_address]] std::conditional_t<kCounters, std::array<std::size_t, N>, no_rounding_t>
        rounding_{};
    std::atomic<std::size_t> in_use_{0};          /**< @brief Slab bytes held. */
    std::atomic<std::size_t> peak_{0};            /**< @brief High-water of `in_use_`. */
    std::atomic<std::size_t> refused_{0};         /**< @brief Requests answered `nullptr`. */
    std::atomic<std::size_t> largest_refused_{0}; /**< @brief Bytes of the largest of those. */
};

/** @brief The host slab pool type: the build's guard, its size-class table. */
using host_pool_t = slab_pool_t<graph::guard_t, std::size(graph::config_t::kSizeClasses)>;

/**
 * @brief The VALUE sub-pool of the host root: the shared size classes behind a per-thread cache
 *        (ADR-0083 Decision 10, Q19).
 *
 * A thread keeps a few free blocks of each class it uses and takes from them without a lock,
 * refilling and spilling half a cache at a time under the class lock. The contention evidence is
 * the one ADR-0060 Erratum 1 and ADR-0079 Amendment 2026-08-20 §3 measured: one shared locked
 * pool collapses under many writers, where per-thread lists scale. A thread's cache is returned
 * to the shared classes when the thread exits.
 *
 * There is one of these, inside @ref host_root(): the per-thread cache is the process's, so the
 * object that owns it is never destroyed.
 */
class host_values_t final : public block_source_t {
   public:
    /** @brief A cached block of the class that holds @p bytes. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override;
    /** @brief Return a block to this thread's cache. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override;
    /** @brief The shared classes' census (@ref slab_pool_t::stats). */
    [[nodiscard]] source_stats_t stats() const noexcept override { return pool_.stats(); }
    /** @brief Return this thread's cache to the shared classes, then @ref slab_pool_t::trim. */
    void trim() noexcept;
    /** @brief The shared classes under the cache. */
    [[nodiscard]] host_pool_t& shared() noexcept { return pool_; }

   private:
    friend class host_root_t;
    /** @brief Built only as a member of @ref host_root_t. */
    constexpr host_values_t(std::span<const std::size_t, host_pool_t::classes()> classes,
                            block_source_t& root) noexcept
        : block_source_t("values"), pool_("values", classes, root) {}
    host_pool_t pool_; /**< @brief The shared size classes. */
};

/**
 * @brief The host default root (ADR-0083 Decisions 3 and 4, #1777): one root with the value,
 *        table and net sub-pools derived from it.
 *
 * Each sub-pool is a @ref slab_pool_t that draws whole slabs from the platform heap, so the host
 * allocator sees slab-sized requests only. A `graph_t` constructed without a source uses this
 * root (`tr::mem::default_root()`): its values come from @ref values, its registration and
 * container blocks from @ref tables, and the router and transport defaults from @ref net when
 * the application injects nothing for them (Q21).
 *
 * As a source of its own the root serves from @ref tables, and its census is the three
 * sub-pools' together: what the node holds from the platform heap.
 *
 * Process-wide and never destroyed, as the platform heap it replaces is: a value released after
 * every static destructor has run still has somewhere to go.
 */
class host_root_t final : public block_source_t {
    static_assert(slab_classes_valid(std::span<const std::size_t>(graph::config_t::kSizeClasses)),
                  "config_t::kSizeClasses must be non-empty, strictly ascending, at most 255 rows, "
                  "and every row a multiple of alignof(std::max_align_t)");

   public:
    /** @brief The value sub-pool (`:stats.mem.values`). */
    [[nodiscard]] host_values_t& values() noexcept { return values_; }
    /** @brief The table sub-pool (`:stats.mem.tables`). */
    [[nodiscard]] host_pool_t& tables() noexcept { return tables_; }
    /** @brief The net sub-pool (`:stats.mem.net`). */
    [[nodiscard]] host_pool_t& net() noexcept { return net_; }

    /** @brief A block from the table sub-pool. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return tables_.try_alloc(bytes, align);
    }
    /** @brief Return a block @ref try_alloc handed out. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        tables_.release(p, bytes, align);
    }
    /**
     * @brief The three sub-pools' census, summed: slab bytes held from the platform heap.
     *
     * `peak` is the SUM of the sub-pools' own peaks, an upper bound on the root's true
     * high-water mark: the three need not have peaked at once. `largest_refused` is the
     * largest of theirs.
     */
    [[nodiscard]] source_stats_t stats() const noexcept override;
    /** @brief Release every fully free slab of all three sub-pools (this thread's value cache
     *         first). */
    void trim() noexcept;

   private:
    template <bool>
    friend struct detail::host_storage_t;
    /** @brief Built only by @ref host_root(). */
    constexpr host_root_t() noexcept
        : block_source_t("host_root"),
          values_(
              std::span<const std::size_t, host_pool_t::classes()>(graph::config_t::kSizeClasses),
              heap_source_),
          tables_(
              "tables",
              std::span<const std::size_t, host_pool_t::classes()>(graph::config_t::kSizeClasses),
              heap_source_),
          net_("net",
               std::span<const std::size_t, host_pool_t::classes()>(graph::config_t::kSizeClasses),
               heap_source_) {}

    heap_source_t heap_source_; /**< @brief The platform heap, which serves the slabs. */
    host_values_t values_;      /**< @brief The value sub-pool. */
    host_pool_t tables_;        /**< @brief The table sub-pool. */
    host_pool_t net_;           /**< @brief The net sub-pool. */
};

/**
 * @brief The process-wide host default root (@ref host_root_t).
 *
 * Constant-initialized storage that is never destroyed. On a build whose `kSlabPool` is
 * `false` nothing in the library uses it.
 */
[[nodiscard]] host_root_t& host_root() noexcept;

}  // namespace tr::mem
