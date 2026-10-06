/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief The hazard domain's node class (#1782, ADR-0083 Decision 5): one fixed-size class of
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

#include "libtracer/lkv_slot.hpp"
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
 * @brief The node class and the platform heap its slabs come from: constant-initialized and
 *        never destroyed.
 *
 * The exit sweep (`final_sweep_t`) and every `thread_local` participant that unwinds after
 * static destruction return nodes here, so the class must outlive them all, as the platform
 * heap it replaces did.
 */
struct node_storage_t {
    mem::heap_source_t heap; /**< @brief Serves whole slabs, never a node. */
    union {
        node_pool_t pool; /**< @brief The node class. */
    };
    /** @brief Constant-initializes the class over @ref heap. */
    constexpr node_storage_t() noexcept
        : heap(),
          pool("lkv_nodes", std::span<const std::size_t, 1>(kNodeClass), heap, kNodeSlabBytes) {}
    /** @brief Deliberately does not destroy the class. */
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
    const std::size_t got = g_nodes.pool.take(0, b.data(), kNodeBatch, sizeof(node_t));
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
        g_nodes.pool.give(0, b.data(), k);
        l.freelist_n -= k;
        n -= k;
    }
}

void free_node(node_t* n) noexcept {
    void* b = n;
    g_nodes.pool.give(0, &b, 1);
}

}  // namespace tr::graph::detail_hp
