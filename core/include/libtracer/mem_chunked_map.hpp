/**
 * @file
 * @brief `tr::mem::chunked_map_t` — a failable sorted map whose entries live in fixed-size
 *        sorted leaves, so an insert or erase moves at most one leaf.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `sorted_map_t` keeps every entry in one block, so an insert or a single-key erase moves the
 * tail: O(n). That is the right trade for a map built at registration time. A map written on
 * the value path is not one: the graph's two propagate-sweep sets take an insert on every
 * `assign` to an observed IF_NEWER vertex (#1886). This map splits the same sorted order into
 * leaves of at most @p LeafEntries entries, indexed by a sorted array of leaves. A lookup is two
 * binary searches; an insert moves at most one leaf, plus one index slot per leaf split; and a
 * key range is still one run in key order, walked leaf by leaf through `pos_t`.
 *
 * Every block comes from the injected source. An insert the source refuses changes nothing; an
 * erase never allocates and never fails. A leaf an erase empties goes back to the source, except
 * the last one: a drained map keeps it for the next insert.
 */
#pragma once

#include <cstddef>
#include <functional>
#include <utility>

#include "libtracer/mem_sorted_map.hpp"
#include "libtracer/mem_source.hpp"

namespace tr::mem {

/**
 * @brief A key/value map over sorted leaves of at most @p LeafEntries entries each, with
 *        failable growth (ADR-0083 Decision 2; #1886).
 *
 * Any insert or erase may move entries, within a leaf or into a new one, so a value pointer or
 * a @ref pos_t is invalidated by every insert and erase.
 *
 * @tparam K           The key type; nothrow-movable. Its order must not change in the map.
 * @tparam V           The mapped type; nothrow-movable.
 * @tparam Less        A strict weak order over keys, transparent when lookups use another type.
 * @tparam LeafEntries The most entries one leaf holds; a power of two of at least 8 keeps every
 *                     leaf block one of the vector's own growth sizes.
 */
template <class K, class V, class Less = std::less<>, std::size_t LeafEntries = 64>
class chunked_map_t {
    static_assert(LeafEntries >= 8 && LeafEntries % 2 == 0, "a leaf splits into two halves");

   public:
    /** @brief One entry: the key and its value, side by side (the `sorted_map_t` layout). */
    using entry_t = typename sorted_map_t<K, V, Less>::entry_t;
    /** @brief What @ref try_emplace answers (the `sorted_map_t` shape). */
    using emplace_result_t = typename sorted_map_t<K, V, Less>::emplace_result_t;

    /** @brief A position in key order: a leaf and an entry in it. The end is `{leaves, 0}`. */
    struct pos_t {
        std::size_t leaf; /**< @brief The leaf's index. */
        std::size_t i;    /**< @brief The entry's index in that leaf. */
        /** @brief Positions compare by both members. */
        bool operator==(const pos_t&) const noexcept = default;
    };

    /** @brief An empty map that will draw its storage from @p src. Allocates nothing. */
    explicit chunked_map_t(block_source_t& src, Less less = Less{}) noexcept
        : leaves_(src), less_(std::move(less)) {}

    /** @brief The value under @p key, or null when absent. */
    template <class Q>
    [[nodiscard]] V* find(const Q& key) noexcept {
        const pos_t p = lower_bound(key);
        return p != end_pos() && !less_(key, at(p).key) ? &at(p).value : nullptr;
    }
    /** @brief True when @p key is present. */
    template <class Q>
    [[nodiscard]] bool contains(const Q& key) const noexcept {
        return const_cast<chunked_map_t*>(this)->find(key) != nullptr;
    }

    /**
     * @brief Insert `{key, V(args...)}` unless @p key is present.
     *
     * A full leaf is split first: a new leaf of @p LeafEntries slots takes its upper half. The
     * split's two allocations are taken before anything moves, so a refusal changes nothing and
     * moves nothing out of @p key or @p args.
     *
     * @return `{value, true}` when added; `{existing, false}` when @p key was present;
     *         `{nullptr, false}` when the source refused (BACKPRESSURE).
     */
    template <class KK, class... Args>
    [[nodiscard]] emplace_result_t try_emplace(KK&& key, Args&&... args) noexcept {
        pos_t p = lower_bound(key);
        if (p != end_pos() && !less_(key, at(p).key)) return {&at(p).value, false};
        // Past every key: the insert appends to the last leaf (a first leaf when there is none).
        if (p == end_pos() && !leaves_.empty()) p = {leaves_.size() - 1, leaves_.back().size()};
        // The first leaf's index slot is exactly one, so a small map costs one leaf and one slot.
        if (leaves_.empty() && (!leaves_.reserve(1) || !leaves_.emplace_back(leaves_.source())))
            return {nullptr, false};
        if (leaves_[p.leaf].size() == LeafEntries && !split(p)) return {nullptr, false};
        entry_t* e = leaves_[p.leaf].emplace_at(p.i, std::in_place, std::forward<KK>(key),
                                                std::forward<Args>(args)...);
        if (e == nullptr && size_ == 0) leaves_.clear();  // a first leaf made for this insert
        size_ += e != nullptr;
        return {e != nullptr ? &e->value : nullptr, e != nullptr};
    }

    /** @brief Remove the entry under @p key. @retval false It was absent. */
    template <class Q>
    bool erase(const Q& key) noexcept {
        const pos_t p = lower_bound(key);
        if (p == end_pos() || less_(key, at(p).key)) return false;
        leaves_[p.leaf].erase_at(p.i);
        // An emptied leaf goes back, unless it is the one a drained map keeps.
        if (--size_ != 0 && leaves_[p.leaf].empty()) leaves_.erase_at(p.leaf);
        return true;
    }

    /**
     * @brief Remove every entry in `[first, last)` that @p drop selects, keeping the rest in key
     *        order, and answer how many went. One pass over the run; a leaf left empty is
     *        given back to the source.
     */
    template <class Drop>
    std::size_t erase_if(pos_t first, pos_t last, Drop drop) noexcept {
        // The leaves the run touches: `last`'s own leaf only when the run ends inside it.
        const std::size_t stop = last.i == 0 ? last.leaf : last.leaf + 1;
        std::size_t gone = 0;
        for (std::size_t l = first.leaf, i = first.i; l < stop; ++l, i = 0) {
            block_array_t<entry_t>& leaf = leaves_[l];
            const std::size_t end = l == last.leaf ? last.i : leaf.size();
            std::size_t kept = i;
            for (std::size_t r = i; r < end; ++r) {
                if (drop(leaf[r])) continue;
                if (kept != r) std::swap(leaf[kept], leaf[r]);
                ++kept;
            }
            leaf.erase_at(kept, end - kept);
            gone += end - kept;
        }
        // Give back the leaves the pass emptied: only those in [first.leaf, stop) can be.
        std::size_t w = first.leaf;
        for (std::size_t r = first.leaf; r < stop; ++r) {
            if (leaves_[r].empty()) continue;
            if (w != r) std::swap(leaves_[w], leaves_[r]);
            ++w;
        }
        // A map drained to nothing keeps its first leaf's block, empty, so the next insert
        // takes no allocation: the drain-then-mark cycle of a propagate sweep is this map's
        // common case. It is the one empty leaf the map ever holds.
        size_ -= gone;
        if (size_ == 0 && w == 0 && stop > 0) w = 1;
        leaves_.erase_at(w, stop - w);
        return gone;
    }

    /** @brief Entry count. */
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    /** @brief True when no entries are held. */
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    /** @brief The first position whose key is not less than @p key, or @ref end_pos. */
    template <class Q>
    [[nodiscard]] pos_t lower_bound(const Q& key) const noexcept {
        if (size_ == 0) return end_pos();  // no keys, perhaps one empty leaf kept for reuse
        // The first leaf whose LAST key is not less than @p key holds the answer, and the last
        // leaf takes every key past the others, so it is never compared: a map of one leaf, the
        // common case, pays only the in-leaf search.
        std::size_t lo = 0;
        std::size_t n = leaves_.size() - 1;
        while (n > 0) {
            const std::size_t half = n / 2;
            if (less_(leaves_[lo + half].back().key, key)) {
                lo += half + 1;
                n -= half + 1;
            } else {
                n = half;
            }
        }
        const block_array_t<entry_t>& leaf = leaves_[lo];
        std::size_t i = 0;
        n = leaf.size();
        while (n > 0) {
            const std::size_t half = n / 2;
            if (less_(leaf[i + half].key, key)) {
                i += half + 1;
                n -= half + 1;
            } else {
                n = half;
            }
        }
        return i == leaf.size() ? end_pos() : pos_t{lo, i};
    }
    /** @brief The past-the-end position. */
    [[nodiscard]] pos_t end_pos() const noexcept { return {leaves_.size(), 0}; }
    /** @brief The position after @p p (`p != end_pos()`). */
    [[nodiscard]] pos_t next(pos_t p) const noexcept {
        return p.i + 1 < leaves_[p.leaf].size() ? pos_t{p.leaf, p.i + 1} : pos_t{p.leaf + 1, 0};
    }
    /** @brief The entry at @p p, unchecked. */
    [[nodiscard]] entry_t& at(pos_t p) noexcept { return leaves_[p.leaf][p.i]; }
    /** @brief The entry at @p p, unchecked (read-only). */
    [[nodiscard]] const entry_t& at(pos_t p) const noexcept { return leaves_[p.leaf][p.i]; }

   private:
    /**
     * @brief Split the full leaf at @p p.leaf into it and a new leaf after it, and re-aim @p p
     *        at the leaf the insert belongs in.
     *
     * An insert at either end of the leaf moves the whole leaf to that side, leaving the
     * inserting side empty: keys that arrive in key order, or in reverse, then fill each leaf
     * before the next split instead of leaving a trail of half-full ones. Any other insert
     * splits the leaf in halves. The index grows geometrically, so a split moves the index's
     * tail (one leaf handle each) and reallocates it only now and then.
     *
     * @retval false The source refused the new leaf or the index slot — nothing moved.
     */
    [[nodiscard]] bool split(pos_t& p) noexcept {
        block_array_t<entry_t> upper(leaves_.source());
        const std::size_t n = leaves_.size();
        if (!upper.reserve(LeafEntries) || (n == leaves_.capacity() && !leaves_.reserve(2 * n)))
            return false;
        block_array_t<entry_t>& lower = leaves_[p.leaf];
        const std::size_t k = p.i == 0 || p.i == LeafEntries ? p.i : LeafEntries / 2;
        for (std::size_t r = k; r < LeafEntries; ++r)
            (void)upper.emplace_back(std::move(lower[r]));  // reserved: cannot fail
        lower.erase_at(k, LeafEntries - k);
        (void)leaves_.emplace_at(p.leaf + 1, std::move(upper));  // reserved: cannot fail
        if (k != 0 && p.i >= k) p = {p.leaf + 1, p.i - k};
        return true;
    }

    block_array_t<block_array_t<entry_t>> leaves_; /**< @brief The leaves, in key order. */
    std::size_t size_ = 0;                         /**< @brief Entries over every leaf. */
    [[no_unique_address]] Less less_;              /**< @brief The key order. */
};

}  // namespace tr::mem
