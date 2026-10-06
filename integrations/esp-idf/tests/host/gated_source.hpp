/**
 * @file
 * @brief `gated_source_t` — a host-test `block_source_t` that can be told to refuse, and
 *        counts what it served and what came back (#1880).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The ESP-IDF links draw their connection state and their egress from the stores in their
 * `link_memory_t`, so the soft-fail suites inject one of these as `memory.state` or
 * `memory.io` and refuse at the instant a case wants a growth site to meet exhaustion.
 * Served blocks come from the process heap. The counters are atomic because a condemned
 * dial returns its slot from a detached recv thread.
 */
#pragma once

#include <atomic>
#include <cstddef>

#include "libtracer/mem_source.hpp"

/** @brief A heap-backed source that refuses while @ref refusing is set. */
class gated_source_t final : public tr::mem::block_source_t {
   public:
    gated_source_t() noexcept : block_source_t("gated") {}

    /** @brief Serve from the heap, or refuse (counted) while @ref refusing is set. */
    [[nodiscard]] void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refusing.load(std::memory_order_relaxed)) {
            refused.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
        void* const p = tr::mem::heap_source().try_alloc(bytes, align);
        if (p != nullptr) live.fetch_add(1, std::memory_order_relaxed);
        return p;
    }
    /** @brief Return a block to the heap. */
    void release(void* p, std::size_t bytes, std::size_t align) noexcept override {
        live.fetch_sub(1, std::memory_order_relaxed);
        tr::mem::heap_source().release(p, bytes, align);
    }

    std::atomic<bool> refusing{false};   /**< @brief Refuse every draw while set. */
    std::atomic<std::size_t> refused{0}; /**< @brief Draws refused so far. */
    std::atomic<std::ptrdiff_t> live{0}; /**< @brief Blocks served and not yet returned. */
};
