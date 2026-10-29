/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief The hazard domain's node class (#1782, ADR-0083 Decision 8): one fixed-size class of
 *        `detail_hp::node_t` blocks, cut from slabs, behind each participant's own free list,
 *        which refills and spills `kNodeBatch` nodes per hold of the class lock.
 *
 * Its own translation unit, so a build that does not bind `hazard_slot_t` references nothing
 * here and links none of it.
 */

#include <array>
#include <cstddef>
#include <new>
#include <span>
#include <type_traits>

#include "libtracer/lkv_slot.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_slab_pool.hpp"
#include "libtracer/placement.hpp"

namespace tr::graph::detail_hp {

namespace {

/** @brief The one row of the node class: a node, padded to the pool's block alignment. */
constexpr std::array<std::size_t, 1> kNodeClass{
    mem::pad_to(sizeof(node_t), alignof(std::max_align_t))};

/** @brief The node class's slab: the smallest the pool takes. A node is 16 bytes, so one slab
 *         holds 252 of them, and a process with one hazard slot holds 4 KiB for it, not the
 *         64 KiB of a value slab. */
constexpr std::size_t kNodeSlabBytes = 4096;

/** @brief The node class: one row, its lock the build's guard. */
using node_pool_t = mem::slab_pool_t<guard_t, 1>;

/**
 * @brief Where an MCU build's node slabs come from (#2288): the build's own value draw, the
 *        static arena (or, on the heap-rooted build that asked for it, the platform heap).
 *        Constant-initializable, so the node class stays `constinit`.
 */
class mcu_slab_source_t final : public mem::block_source_t {
   public:
    /** @brief Names the source for `:stats`. */
    constexpr mcu_slab_source_t() noexcept : block_source_t("lkv_nodes") {}
    /** @brief One slab from the build's value draw; `nullptr` when it is spent. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        return mem::detail::mcu_value_alloc(bytes, align);
    }
    /** @brief Return a slab @ref try_alloc handed out. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        mem::detail::mcu_value_release(p, bytes, align);
    }
};

/** @brief The slab source: the platform heap where the build pools on the host, as the host
 *         root's own slabs do; the build's value draw on an MCU, which names no heap. */
using slab_source_t = std::conditional_t<mem::kSlabPool, mem::heap_source_t, mcu_slab_source_t>;

/** @brief The node class and the source its slabs come from, as one object. */
struct node_class_t {
    slab_source_t heap; /**< @brief Serves whole slabs, never a node. */
    node_pool_t pool;   /**< @brief The node class, drawing its slabs from @ref heap. */
    /** @brief Constant-initializes the class over @ref heap. */
    constexpr node_class_t() noexcept
        : heap(),
          pool("lkv_nodes", std::span<const std::size_t, 1>(kNodeClass), heap, kNodeSlabBytes) {}
};

/**
 * @brief Storage for the one @ref node_class_t: constant-initialized and never destroyed.
 *
 * The exit sweep (`final_sweep_t`) and every `thread_local` participant that unwinds after
 * static destruction return nodes here, and a return can release a slab to the heap source
 * (once the class keeps `kSlabClassCap` free slabs), so the pool AND the source it releases
 * through must outlive them all, as the platform heap they replace did. Both sit inside the
 * union: a source kept beside it would be destroyed at static destruction while the pool still
 * pointed at it.
 */
struct node_storage_t {
    union {
        node_class_t cls; /**< @brief The class and its slab source. */
    };
    /** @brief Constant-initializes the class. */
    constexpr node_storage_t() noexcept : cls() {}
    /** @brief Deliberately destroys nothing. */
    ~node_storage_t() {}
    node_storage_t(const node_storage_t&) = delete;
    node_storage_t& operator=(const node_storage_t&) = delete;
};

/** @brief The process's node class. */
constinit node_storage_t g_nodes{};

}  // namespace

std::size_t refill_nodes(lists_t& l) noexcept {
    std::array<void*, kNodeBatch> b;
    if (!tr::detail::probe_hook_ok(sizeof(node_t))) return 0;  // test-only OOM injection
    const std::size_t got = g_nodes.cls.pool.take(0, b.data(), kNodeBatch, sizeof(node_t));
    for (std::size_t i = 0; i < got; ++i) {
        node_t* const n = new (b[i]) node_t;
        n->next = l.freelist;
        l.freelist = n;
    }
    l.freelist_n += got;
    return got;
}

void spill_nodes(lists_t& l, std::size_t n) noexcept {
    std::array<void*, kNodeBatch> b;
    while (n != 0) {
        const std::size_t k = n < kNodeBatch ? n : kNodeBatch;
        for (std::size_t i = 0; i < k; ++i) {
            b[i] = l.freelist;
            l.freelist = l.freelist->next;
        }
        g_nodes.cls.pool.give(0, b.data(), k);
        l.freelist_n -= k;
        n -= k;
    }
}

void free_node(node_t* n) noexcept {
    void* b = n;
    g_nodes.cls.pool.give(0, &b, 1);
}

}  // namespace tr::graph::detail_hp
