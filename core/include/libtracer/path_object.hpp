/**
 * @file
 * @brief The path object (RFC-0030 §5): the graph-owned name an application addresses a vertex
 *        through — the canonical bytes, the local pair it resolved to, and a soft chain cache.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A leaf: it includes no graph header, so code that only holds a path object does not parse
 * `graph_t`. `graph.hpp` includes it back for the host-API doors that take one.
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "libtracer/config.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/vertex_handle.hpp"

namespace tr::graph {

/**
 * @brief The chain cache's capacity in bytes for this build: @ref default_config_t::kPathChainBytes
 *        rounded up to whole 4-byte words, the unit the cache is copied in.
 */
inline constexpr std::size_t kPathChainCapacity = (config_t::kPathChainBytes + 3) / 4 * 4;

/**
 * @brief A graph-owned path object (RFC-0030 §5): what the host API addresses a vertex through.
 *
 * Made by @ref graph_t::make_path from a canonical path string (parsed and validated once) or
 * from a pre-encoded canonical `PATH` payload in read-only memory, which it borrows and does not
 * copy (v1 §3.1.2). It holds three things:
 *
 * 1. the **canonical bytes**, for its whole life. They are the durable truth and the only form
 *    an application may persist (§5.1 item 2);
 * 2. **element 0**, the local pair (slot index and generation) the canonical bytes last resolved
 *    to. A door that takes the object dereferences it first (one bounds check and one compare,
 *    RFC-0024 §5.1). When the target was retired the dereference refuses, and the door resolves
 *    the canonical bytes again and re-learns it, with the same answer the canonical form gives
 *    (§5.2 Fallback);
 * 3. the **chain cache**: the elements beyond the first door, learned from a matched reply
 *    (§5.2 Learning). It is soft: losing or refusing it never changes an answer. Its capacity is
 *    the compile-time @ref kPathChainCapacity, stored inline, so learning never allocates; a
 *    chain that does not fit is not cached (§5.4).
 *
 * A learn rewrites the inline chain under a sequence counter. A reader that races one retries a
 * few times and then reads no chain, which is the canonical form; nothing is freed while a
 * reader holds it, so the cache adds no reclamation problem (§5.4).
 *
 * Move-only. The object's block comes from the graph's table source and goes back to it when the
 * object is destroyed, so it MUST NOT outlive the graph that made it. A default-constructed or
 * moved-from object names nothing, and every door answers it `NOT_FOUND`.
 *
 * It holds no route-table entry and registers nothing anywhere (§5.5): the pair it caches is the
 * vertex index's own, and the graph never learns that a path object exists.
 */
class path_object_t {
   public:
    /** @brief An object that names nothing. */
    path_object_t() noexcept = default;
    path_object_t(const path_object_t&) = delete;
    path_object_t& operator=(const path_object_t&) = delete;
    /** @brief Take @p o's block; @p o names nothing afterwards. */
    path_object_t(path_object_t&& o) noexcept : b_(o.b_) { o.b_ = nullptr; }
    /** @brief Release this object's block, then take @p o's. */
    path_object_t& operator=(path_object_t&& o) noexcept {
        if (this != &o) {
            release();
            b_ = o.b_;
            o.b_ = nullptr;
        }
        return *this;
    }
    /** @brief Return the block to the source that served it. */
    ~path_object_t() { release(); }

    /** @brief True when this object names nothing (default-constructed or moved from). */
    [[nodiscard]] bool empty() const noexcept { return b_ == nullptr; }

    /** @brief The canonical `PATH` payload bytes (packed records); empty for an empty object. */
    [[nodiscard]] std::span<const std::byte> key() const noexcept {
        return b_ == nullptr ? std::span<const std::byte>{}
                             : std::span<const std::byte>{b_->key, b_->key_len};
    }

    /**
     * @brief The local pair the canonical bytes last resolved to, if one is learned.
     *
     * Learned by the first door that resolves the object, and replaced when a later door finds
     * it stale. It is a cache: a match still authorizes nothing (RFC-0024 §6.2).
     */
    [[nodiscard]] std::optional<vertex_slot_t> element0() const noexcept;

    /**
     * @brief Copy the cached chain (the elements beyond the first door) into @p out.
     * @return The chain's length in bytes, or 0 when there is none, it does not fit @p out, or a
     *         learn kept racing the copy: in every such case the caller uses the canonical form.
     */
    [[nodiscard]] std::size_t chain(std::span<std::byte> out) const noexcept;

    /**
     * @brief Cache @p chain, a `PATH` body of the elements beyond the first door.
     * A chain larger than @ref kPathChainCapacity is not cached and drops the chain held, which
     * it supersedes, so the object goes back to the canonical form (§5.4).
     *
     * @return false, caching nothing, when @p chain is empty or too large, when this object
     *         names nothing, or when another learn is writing at the same moment (the cache is
     *         soft, so one of two racing learns is simply dropped).
     */
    bool learn(std::span<const std::byte> chain) noexcept;

    /** @brief Drop the cached chain (a chain-spelled request was answered `NOT_FOUND`, §5.2). */
    void clear_chain() noexcept;

   private:
    friend class graph_t;

    static constexpr std::size_t kChainWords = kPathChainCapacity / 4;

    // Every shared field is a 32-bit atomic, lock-free on every target (a 64-bit atomic is a
    // libcall critical section on a 32-bit MCU, which an ISR door could deadlock on). Element 0
    // and the chain are written under `seq`, so a reader takes both words of the pair from one
    // write.
    struct body_t {
        mem::block_source_t* src;              // who serves this block
        std::size_t block_bytes;               // its size, for the sized release
        const std::byte* key;                  // the canonical bytes: this block's tail, or ROM
        std::uint32_t key_len;                 // at most kMaxPathBytes
        std::atomic<std::uint32_t> seq;        // even: stable; odd: a writer holds it
        std::atomic<std::uint32_t> e0_index;   // element 0's slot index
        std::atomic<std::uint32_t> e0_gen;     // its generation; kGenerationSaturated: none
        std::atomic<std::uint32_t> chain_len;  // 0: no chain
        std::array<std::atomic<std::uint32_t>, kChainWords> chain;  // inline chain cache
    };
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "a path object's door reads only lock-free atomics");

    // The writer half of the seqlock: take `seq` odd, or answer false when another writer holds
    // it (the cache is soft, so the losing write is dropped). `end_write` publishes.
    [[nodiscard]] bool begin_write(std::uint32_t& s) const noexcept;
    void end_write(std::uint32_t s) const noexcept;

    explicit path_object_t(body_t* b) noexcept : b_(b) {}
    void set_element0(std::optional<vertex_slot_t> e) const noexcept;
    void release() noexcept;

    body_t* b_ = nullptr;
};

}  // namespace tr::graph
