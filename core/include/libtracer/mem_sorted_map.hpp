/**
 * @file
 * @brief `tr::mem::sorted_map_t` — the core's failable map: key/value entries kept sorted in
 *        one `block_array_t`, searched by binary search.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * One of the three core containers ADR-0083 Decision 2 names (#1776), next to the vector
 * (`block_array_t`, `mem_source.hpp`) and the name/string store (`mem_string.hpp`). It is the
 * replacement for a core `std::map` or `std::unordered_map`: one contiguous block from the
 * injected source instead of a node per entry from the global heap, and an insert the source
 * refuses comes back as a null value pointer instead of a throw. Nothing migrates onto it in
 * the ticket that adds it; the directory batches do that.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

#include "libtracer/mem_source.hpp"

namespace tr::mem {

/**
 * @brief A key/value map over one sorted @ref block_array_t, with failable growth
 *        (ADR-0083 Decision 2).
 *
 * Lookup is a binary search, O(log n) with no pointer chasing; insert and erase shift the tail,
 * O(n). That is the right trade for the core's maps, which are built at registration time and
 * read on every frame. A refused insert leaves the map and the caller's arguments untouched.
 *
 * Lookup is heterogeneous: with the default `std::less<>`, a map keyed by @ref string_t is
 * searched with a `std::string_view` and builds no key. A key is constructed only when an
 * insert actually adds an entry.
 *
 * Any growth may move the entries, so a pointer or iterator into the map is invalidated by
 * every insert and erase.
 *
 * @tparam K    The key type; nothrow-movable. Its order must not change while it is in the map.
 * @tparam V    The mapped type; nothrow-movable.
 * @tparam Less A strict weak order over keys, transparent when lookups use another type.
 */
template <class K, class V, class Less = std::less<>>
class sorted_map_t {
   public:
    /** @brief One entry: the key and its value, stored side by side. */
    struct entry_t {
        /** @brief Build the key from @p k and the value from @p args, in place. */
        template <class KK, class... Args>
        entry_t(std::in_place_t, KK&& k, Args&&... args) noexcept
            : key(std::forward<KK>(k)), value(std::forward<Args>(args)...) {}

        K key;   /**< @brief The key; changing its order while it is in the map is undefined. */
        V value; /**< @brief The mapped value. */
    };

    /** @brief What @ref try_emplace answers. */
    struct emplace_result_t {
        /** @brief The entry's value, or null when the source refused the insert. */
        V* value;
        /** @brief True when this call added the entry; false when the key was present or the
         *         insert was refused. */
        bool inserted;
    };

    /** @brief A mutable entry iterator, in key order (a plain pointer). */
    using iterator = entry_t*;
    /** @brief A read-only entry iterator, in key order. */
    using const_iterator = const entry_t*;

    /** @brief An empty map that will draw its storage from @p src. Allocates nothing. */
    explicit sorted_map_t(block_source_t& src, Less less = Less{}) noexcept
        : entries_(src), less_(std::move(less)) {}

    /** @brief The value under @p key, or null when absent. */
    template <class Q>
    [[nodiscard]] V* find(const Q& key) noexcept {
        entry_t* e = entries_.data() + lower_bound(key);
        return e != entries_.end() && !less_(key, e->key) ? &e->value : nullptr;
    }
    /** @brief The value under @p key (const), or null when absent. */
    template <class Q>
    [[nodiscard]] const V* find(const Q& key) const noexcept {
        return const_cast<sorted_map_t*>(this)->find(key);
    }
    /** @brief True when @p key is present. */
    template <class Q>
    [[nodiscard]] bool contains(const Q& key) const noexcept {
        return find(key) != nullptr;
    }

    /**
     * @brief Insert `{key, V(args...)}` unless @p key is present.
     *
     * The key and value are constructed only when the entry is added. On a refused insert
     * neither @p key nor @p args are moved from, so the caller still owns them.
     *
     * @return `{value, true}` when added; `{existing, false}` when @p key was present;
     *         `{nullptr, false}` when the source refused (BACKPRESSURE).
     */
    template <class KK, class... Args>
    [[nodiscard]] emplace_result_t try_emplace(KK&& key, Args&&... args) noexcept {
        const std::size_t i = lower_bound(key);
        if (i != entries_.size() && !less_(key, entries_[i].key))
            return {&entries_[i].value, false};
        entry_t* e = entries_.emplace_at(i, std::in_place, std::forward<KK>(key),
                                         std::forward<Args>(args)...);
        return {e != nullptr ? &e->value : nullptr, e != nullptr};
    }

    /** @brief Remove the entry under @p key. @retval false It was absent. */
    template <class Q>
    bool erase(const Q& key) noexcept {
        const std::size_t i = lower_bound(key);
        if (i == entries_.size() || less_(key, entries_[i].key)) return false;
        entries_.erase_at(i);
        return true;
    }

    /**
     * @brief Ensure room for @p n entries without growing again.
     * @retval false The source refused — the map is unchanged.
     */
    [[nodiscard]] bool reserve(std::size_t n) noexcept { return entries_.reserve(n); }
    /** @brief Remove every entry; the block is kept for reuse. */
    void clear() noexcept { entries_.clear(); }
    /** @brief Entry count. */
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    /** @brief True when no entries are held. */
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    /** @brief The first entry in key order. */
    [[nodiscard]] iterator begin() noexcept { return entries_.begin(); }
    /** @brief Past the last entry. */
    [[nodiscard]] iterator end() noexcept { return entries_.end(); }
    /** @brief The first entry in key order (read-only). */
    [[nodiscard]] const_iterator begin() const noexcept { return entries_.begin(); }
    /** @brief Past the last entry (read-only). */
    [[nodiscard]] const_iterator end() const noexcept { return entries_.end(); }
    /** @brief Entry @p i in key order, unchecked. */
    [[nodiscard]] entry_t& at(std::size_t i) noexcept { return entries_[i]; }
    /** @brief Entry @p i in key order, unchecked (read-only). */
    [[nodiscard]] const entry_t& at(std::size_t i) const noexcept { return entries_[i]; }
    /** @brief Remove the @p n entries from @p i, shifting the tail down once. Precondition:
     *         `i + n <= size()`. */
    void erase_at(std::size_t i, std::size_t n = 1) noexcept { entries_.erase_at(i, n); }

    /**
     * @brief Index of the first entry whose key is not less than @p key — where a range scan
     *        over a key prefix starts (`size()` when there is none).
     */
    template <class Q>
    [[nodiscard]] std::size_t lower_bound(const Q& key) const noexcept {
        std::size_t lo = 0;
        std::size_t n = entries_.size();
        while (n > 0) {
            const std::size_t half = n / 2;
            if (less_(entries_[lo + half].key, key)) {
                lo += half + 1;
                n -= half + 1;
            } else {
                n = half;
            }
        }
        return lo;
    }

   private:
    block_array_t<entry_t> entries_;  /**< @brief The entries, sorted by key. */
    [[no_unique_address]] Less less_; /**< @brief The key order. */
};

}  // namespace tr::mem
