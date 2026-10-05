/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief The process-default per-value backend, the host default root (#1777) with the value
 *        sub-pool's per-thread cache, and the L1 allocation helpers over them.
 */

#include "libtracer/mem_heap.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <span>
#include <type_traits>

#include "libtracer/mem_slab_pool.hpp"
#include "libtracer/mem_source_backend.hpp"

namespace tr::mem {

/**
 * @brief heap_backend_t is defined in the header (mem_heap.hpp) so the module-set destroy dispatch
 *        (backend_set.cpp, ADR-0047 §2) can see the concrete type.
 */
mem_backend_t& heap_backend() noexcept {
    static heap_backend_t backend;
    return backend;
}

namespace detail {

/** @brief No host root: a build whose `kSlabPool` is `false` (nothing calls @ref root). */
template <bool kOn>
struct host_storage_t {
    /** @brief Unreachable on this build. */
    [[noreturn]] host_root_t& root() noexcept { std::abort(); }
};

/**
 * @brief Storage for the one @ref host_root_t: constant-initialized, and never destroyed, so a
 *        block released after every static destructor has run still has a pool to go to.
 */
template <>
struct host_storage_t<true> {
    union {
        host_root_t root_; /**< @brief The root. */
    };
    /** @brief Constant-initializes the root. */
    constexpr host_storage_t() noexcept : root_() {}
    /** @brief Deliberately does not destroy the root. */
    ~host_storage_t() {}
    host_storage_t(const host_storage_t&) = delete;
    host_storage_t& operator=(const host_storage_t&) = delete;
    /** @brief The root. */
    host_root_t& root() noexcept { return root_; }
};

}  // namespace detail

namespace {

/** @brief The host root, where the build has one. */
constinit detail::host_storage_t<kSlabPool> g_host{};

/** @brief Rows of the host size-class table. */
constexpr std::size_t kClasses = host_pool_t::classes();

/** @brief Bytes a thread keeps cached per class, before the count bounds below. */
constexpr std::size_t kCacheBytes = 16384;

/** @brief The most blocks a thread caches per class; also the refill array's size. */
constexpr std::uint32_t kCacheMax = 32;

/** @brief Blocks a thread caches per class before it spills half of them: 16 KiB worth, from 4
 *         blocks to @ref kCacheMax. */
constexpr auto kCacheLimit = [] {
    std::array<std::uint32_t, kClasses> l{};
    for (std::size_t i = 0; i < kClasses; ++i)
        l[i] = static_cast<std::uint32_t>(
            std::clamp<std::size_t>(kCacheBytes / graph::config_t::kSizeClasses[i], 4, kCacheMax));
    return l;
}();

/** @brief Where a thread's value cache is in its life. */
enum class cache_state_t : std::uint8_t {
    FRESH, /**< @brief Never used: the thread-exit flush is not registered yet. */
    LIVE,  /**< @brief In use, and flushed when the thread exits. */
    DEAD,  /**< @brief Flushed at thread exit: blocks go straight to the shared classes. */
};

/** @brief Rows a thread's cache keeps: one per class, and none in a build without the pool, so
 *         such a build's threads carry no cache in their TLS. */
constexpr std::size_t kCacheRows = kSlabPool ? kClasses : 0;

/** @brief One thread's cache of value-sub-pool blocks: a free list and a count per class.
 *         Trivially destructible and constant-initialized, so the hot path reads it with no
 *         TLS guard. */
struct value_cache_t {
    std::array<void*, kCacheRows> head{};          /**< @brief Cached blocks, linked in place. */
    std::array<std::uint32_t, kCacheRows> count{}; /**< @brief Blocks cached per class. */
    cache_state_t state = cache_state_t::FRESH;    /**< @brief See @ref cache_state_t. */
};

/** @brief This thread's value cache. */
thread_local constinit value_cache_t t_cache{};

/** @brief The value sub-pool's shared classes. */
[[nodiscard]] host_pool_t& shared_values() noexcept { return g_host.root().values().shared(); }

/** @brief Cache block @p p of class @p i on this thread. */
void cache_push(value_cache_t& c, std::size_t i, void* p) noexcept {
    detail::slab_link(p, c.head[i]);
    detail::slab_poison(static_cast<std::byte*>(p) + sizeof(void*),
                        graph::config_t::kSizeClasses[i] - sizeof(void*));
    c.head[i] = p;
    ++c.count[i];
}

/** @brief Take one cached block of class @p i (the cache holds one). */
[[nodiscard]] void* cache_pop(value_cache_t& c, std::size_t i) noexcept {
    void* const p = c.head[i];
    c.head[i] = detail::slab_next(p);
    --c.count[i];
    detail::slab_unpoison(p, graph::config_t::kSizeClasses[i]);
    return p;
}

/** @brief Hand up to @p n cached blocks of class @p i back to the shared classes. */
void cache_spill(value_cache_t& c, std::size_t i, std::uint32_t n) noexcept {
    std::array<void*, kCacheMax> out;
    while (n != 0 && c.count[i] != 0) {
        std::uint32_t k = 0;
        while (k < n && k < kCacheMax && c.count[i] != 0) out[k++] = cache_pop(c, i);
        shared_values().give(i, out.data(), k);
        n -= k;
    }
}

/** @brief Hand this thread's whole cache back to the shared classes. */
void cache_flush(value_cache_t& c) noexcept {
    for (std::size_t i = 0; i < kCacheRows; ++i) cache_spill(c, i, c.count[i]);
}

/** @brief Flushes this thread's cache when the thread exits, and retires it. */
struct value_cache_owner_t {
    bool armed = false; /**< @brief Touched once, to register the exit flush. */
    /** @brief Flush, then mark the cache DEAD so later releases bypass it. */
    ~value_cache_owner_t() {
        if constexpr (kSlabPool) {
            cache_flush(t_cache);
            t_cache.state = cache_state_t::DEAD;
        }
    }
};

/** @brief The registration of this thread's exit flush; touched only off the hot path. */
thread_local value_cache_owner_t t_owner;

/** @brief Make this thread's cache LIVE: register its exit flush. */
[[gnu::noinline]] void cache_arm(value_cache_t& c) noexcept {
    t_owner.armed = true;
    c.state = cache_state_t::LIVE;
}

/** @brief The cache is empty for class @p i: refill half a cache under the class lock. */
[[gnu::noinline]] void* cache_refill(value_cache_t& c, std::size_t i, std::size_t bytes) noexcept {
    host_pool_t& pool = shared_values();
    std::array<void*, kCacheMax> got;
    if (c.state == cache_state_t::DEAD) {
        return pool.take(i, got.data(), 1, bytes) == 1 ? got[0] : nullptr;
    }
    if (c.state == cache_state_t::FRESH) cache_arm(c);
    const std::size_t n =
        pool.take(i, got.data(), std::max<std::uint32_t>(1, kCacheLimit[i] / 2), bytes);
    for (std::size_t k = 1; k < n; ++k) cache_push(c, i, got[k]);
    return n != 0 ? got[0] : nullptr;
}

/** @brief The cold arms of a release: a FRESH cache arms, a DEAD one gives straight back, a full
 *         one spills half. */
[[gnu::noinline]] void cache_release_slow(value_cache_t& c, std::size_t i, void* p) noexcept {
    if (c.state == cache_state_t::DEAD) {
        shared_values().give(i, &p, 1);
        return;
    }
    if (c.state == cache_state_t::FRESH) cache_arm(c);
    cache_push(c, i, p);
    if (c.count[i] > kCacheLimit[i]) cache_spill(c, i, kCacheLimit[i] / 2);
}

}  // namespace

namespace detail {

void* host_value_alloc(std::size_t bytes, std::size_t align) noexcept {
    if constexpr (kSlabPool) {
        host_pool_t& pool = shared_values();
        const std::size_t i = pool.class_of(bytes, align);
        if (i == host_pool_t::kNoClass) return pool.try_alloc(bytes, align);
        value_cache_t& c = t_cache;
        if (c.head[i] != nullptr) return cache_pop(c, i);
        return cache_refill(c, i, bytes);
    } else {
        return heap_source_t::acquire(bytes, align);
    }
}

void host_value_release(void* p, std::size_t bytes, std::size_t align) noexcept {
    if constexpr (kSlabPool) {
        host_pool_t& pool = shared_values();
        const std::size_t i = pool.class_of(bytes, align);
        if (i == host_pool_t::kNoClass) {
            pool.release(p, bytes, align);
            return;
        }
        value_cache_t& c = t_cache;
        if (c.state != cache_state_t::LIVE || c.count[i] >= kCacheLimit[i]) [[unlikely]] {
            cache_release_slow(c, i, p);
            return;
        }
        cache_push(c, i, p);
    } else {
        heap_source_t::reclaim(p, bytes, align);
    }
}

}  // namespace detail

void* host_values_t::try_alloc(std::size_t bytes, std::size_t align) noexcept {
    if (!tr::detail::probe_hook_ok(bytes)) return nullptr;  // test-only OOM injection
    return detail::host_value_alloc(bytes, align);
}

void host_values_t::release(void* p, std::size_t bytes, std::size_t align) noexcept {
    detail::host_value_release(p, bytes, align);
}

void host_values_t::trim() noexcept {
    if constexpr (kSlabPool) cache_flush(t_cache);
    pool_.trim();
}

source_stats_t host_root_t::stats() const noexcept {
    source_stats_t sum{};
    for (const source_stats_t s : {values_.stats(), tables_.stats(), net_.stats()}) {
        sum.in_use += s.in_use;
        sum.peak += s.peak;
        sum.refused += s.refused;
        sum.largest_refused = std::max(sum.largest_refused, s.largest_refused);
    }
    return sum;
}

void host_root_t::trim() noexcept {
    values_.trim();
    tables_.trim();
    net_.trim();
}

host_root_t& host_root() noexcept { return g_host.root(); }

block_source_t& default_root() noexcept {
    if constexpr (kSlabPool) {
        return g_host.root();
    } else {
        return heap_source();
    }
}

block_source_t& value_source() noexcept {
    if constexpr (kSlabPool) {
        return g_host.root().values();
    } else {
        return heap_source();
    }
}

block_source_t& table_source() noexcept {
    if constexpr (kSlabPool) {
        return g_host.root().tables();
    } else {
        return heap_source();
    }
}

block_source_t& net_source() noexcept {
    if constexpr (kSlabPool) {
        return g_host.root().net();
    } else {
        return heap_source();
    }
}

mem_backend_t& net_backend() noexcept {
    if constexpr (kSlabPool) {
        // Never destroyed, for the reason the root is not: a segment can be released after
        // every static destructor has run.
        alignas(source_backend_t) static std::byte storage[sizeof(source_backend_t)];
        static source_backend_t* const backend =
            new (storage) source_backend_t(g_host.root().net());
        return *backend;
    } else {
        return heap_backend();
    }
}

}  // namespace tr::mem

namespace tr::view {

/**
 * @brief The one locus of "adopt a fresh owned segment from a backend" (#793) — @ref
 *        heap_alloc is this over @ref mem::heap_backend.
 */
segment_ptr_t segment_alloc(mem::mem_backend_t& backend, std::size_t size) {
    return segment_ptr_t::adopt(backend.alloc(size, mem::alloc_hint_t::NONE));
}

rx_block_t alloc_rx(mem::mem_backend_t& backend, std::size_t len, std::size_t loan_min) noexcept {
    // The loan pays only where the value would be SHARED, and only on memory the graph can
    // lay a record in: host bytes, aligned for the record's words, with room for both.
    const bool loan = len >= loan_min && backend.space() == mem::mem_space_t::HOST &&
                      mem::rx_loan_fits(len, backend.max_segment_size(), backend.alignment());
    if (!loan) return {segment_ptr_t::adopt(backend.alloc(len)), 0};
    // ONE request either way: a backend that refuses the reserved block is exhausted, and a
    // second, smaller ask would spend its refusal twice on one frame. A backend whose blocks
    // simply cannot hold the reserve said so through `max_segment_size` above.
    segment_ptr_t seg = segment_ptr_t::adopt(backend.alloc(len + mem::kRxLoanBytes));
    if (!seg) return {};
    new (seg->bytes.data()) rx_loan_word_t(0);  // unclaimed
    seg->rx_loan = 1;
    return {std::move(seg), mem::kRxLoanBytes};
}

// NOT written as `segment_alloc(mem::heap_backend(), size)`: this is the arm every
// pre-#793 call site takes, and the whole latency claim for #793 is that those call
// sites are BYTE-identical. Delegating would put one extra call frame on it. One
// duplicated line is the price of an object-file `cmp` being the proof.
segment_ptr_t heap_alloc(std::size_t size) {
    return segment_ptr_t::adopt(mem::heap_backend().alloc(size, mem::alloc_hint_t::NONE));
}

}  // namespace tr::view
