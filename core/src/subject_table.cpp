/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The interned-subject table behind `subject_id_t` (RFC-0028 §6.8, #1622). Its own TU so the
 * graph's pinned dispatch legs share no inline budget with it.
 */
#include "libtracer/subject_table.hpp"

#include <cstring>
#include <mutex>
#include <new>

/**
 * @file
 * @brief The mutation half of the interned-subject table: intern, release, and the dedup
 *        index. The lock-free read half is `tr::graph::subject_names` in the header.
 */

namespace tr::graph {
namespace {

/** @brief The largest table index an id can carry (30 bits; `0` is "none"). */
constexpr std::uint32_t kMaxSubjectIndex = subject_id_t::kIndexMask;

/** @brief FNV-1a over @p s, continuing from @p h. */
std::uint32_t fnv1a(std::uint32_t h, std::string_view s) noexcept {
    for (const char c : s) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h;
}

/** @brief The pair's hash. The separator byte keeps ("ab","c") and ("a","bc") apart. */
std::uint32_t pair_hash(std::string_view link, std::string_view caller) noexcept {
    std::uint32_t h = fnv1a(2166136261u, link);
    h ^= 0xFFu;
    h *= 16777619u;
    return fnv1a(h, caller);
}

/**
 * @brief The table's mutable state: the dedup index, the free list, the high-water index.
 *
 * All of it guarded by @ref m. The entries themselves live in `detail::subject_chunks`, which
 * the header exposes so the read half can stay inline. Trivially destructible on purpose (no
 * destructor runs at exit), for the same reason the directory is: an edge record owned by a
 * static object may release its subject after every static destructor has run.
 */
struct subject_table_t {
    std::mutex m;                   /**< @brief Guards everything below and every entry's
                                     *          `refs` / `next_free`. A LEAF lock. */
    std::uint32_t* index = nullptr; /**< @brief Open-addressed dedup index of entry indices
                                     *          (`0` ⇒ empty bucket), linear probing. */
    std::uint32_t capacity = 0;     /**< @brief Buckets in @ref index (a power of two or 0). */
    std::uint32_t live = 0;         /**< @brief Live entries. */
    std::uint32_t high_water = 0;   /**< @brief The largest index ever minted (`0` is chunk
                                     *          0's reserved empty entry, never minted). */
    std::uint32_t free_head = 0;    /**< @brief First free index below @ref high_water. */
};

/** @brief The one process-wide table. Never destroyed (see @ref subject_table_t). */
subject_table_t& table() noexcept {
    alignas(subject_table_t) static unsigned char storage[sizeof(subject_table_t)];
    static subject_table_t* const t = new (storage) subject_table_t{};
    return *t;
}

/** @brief The entry at @p index, whose chunk is known to exist. */
detail::subject_entry_t& entry(std::uint32_t index) noexcept {
    const detail::subject_slot_t at = detail::subject_slot(index);
    return detail::subject_chunks[at.chunk].load(std::memory_order_relaxed)[at.offset];
}

/** @brief True iff entry @p e spells exactly (@p link, @p caller). */
bool entry_is(const detail::subject_entry_t& e, std::uint32_t h, std::string_view link,
              std::string_view caller) noexcept {
    return e.hash == h && std::string_view(e.link, e.link_len) == link &&
           std::string_view(e.caller, e.caller_len) == caller;
}

/** @brief Place @p index into @p buckets (capacity @p cap) by its entry's hash. */
void index_insert(std::uint32_t* buckets, std::uint32_t cap, std::uint32_t index) noexcept {
    const std::uint32_t mask = cap - 1;
    std::uint32_t i = entry(index).hash & mask;
    while (buckets[i] != 0) i = (i + 1) & mask;
    buckets[i] = index;
}

/** @brief Grow the dedup index so one more entry keeps the load at or below 1/2.
 *  @return false on allocation failure (the index is unchanged). */
bool index_reserve_one(subject_table_t& t) noexcept {
    if ((t.live + 1) * 2 <= t.capacity) return true;
    const std::uint32_t cap = t.capacity == 0 ? 16 : t.capacity * 2;
    auto* const fresh = new (std::nothrow) std::uint32_t[cap]();
    if (fresh == nullptr) return false;
    for (std::uint32_t i = 0; i < t.capacity; ++i)
        if (t.index[i] != 0) index_insert(fresh, cap, t.index[i]);
    delete[] t.index;
    t.index = fresh;
    t.capacity = cap;
    return true;
}

/** @brief Remove @p index from the dedup index — backward-shift deletion, no tombstones. */
void index_erase(subject_table_t& t, std::uint32_t index) noexcept {
    const std::uint32_t mask = t.capacity - 1;
    std::uint32_t i = entry(index).hash & mask;
    while (t.index[i] != index) i = (i + 1) & mask;
    for (std::uint32_t j = (i + 1) & mask; t.index[j] != 0; j = (j + 1) & mask) {
        const std::uint32_t home = entry(t.index[j]).hash & mask;
        // Move j's occupant into the hole at i iff its home does not lie cyclically in (i, j].
        const bool in_range = i <= j ? (home > i && home <= j) : (home > i || home <= j);
        if (in_range) continue;
        t.index[i] = t.index[j];
        i = j;
    }
    t.index[i] = 0;
}

/** @brief A free index with its chunk allocated, or `0` when none can be had. */
std::uint32_t take_index(subject_table_t& t) noexcept {
    if (t.free_head != 0) {
        const std::uint32_t i = t.free_head;
        t.free_head = entry(i).next_free;
        return i;
    }
    if (t.high_water == kMaxSubjectIndex) return 0;
    const std::uint32_t i = t.high_water + 1;
    const detail::subject_slot_t at = detail::subject_slot(i);
    if (at.offset == 0 && at.chunk != 0) {  // the first index of a chunk not yet allocated
        auto* const chunk =
            new (std::nothrow) detail::subject_entry_t[detail::kSubjectChunkBase << at.chunk]();
        if (chunk == nullptr) return 0;
        detail::subject_chunks[at.chunk].store(chunk, std::memory_order_release);
    }
    t.high_water = i;
    return i;
}

}  // namespace

std::optional<subject_id_t> intern_subject(std::string_view link,
                                           std::string_view caller) noexcept {
    if (link.empty() && caller.empty()) return subject_id_t{};
    const std::uint32_t link_bit = link.empty() ? 0 : subject_id_t::kLinkBit;
    const std::uint32_t h = pair_hash(link, caller);
    subject_table_t& t = table();
    const std::lock_guard lock(t.m);
    if (t.capacity != 0) {
        const std::uint32_t mask = t.capacity - 1;
        for (std::uint32_t i = h & mask; t.index[i] != 0; i = (i + 1) & mask) {
            detail::subject_entry_t& e = entry(t.index[i]);
            if (!entry_is(e, h, link, caller)) continue;
            ++e.refs;
            return subject_id_t{t.index[i] | link_bit};
        }
    }
    // A new pair. One run of characters holds both spellings — the default subject
    // (caller == link) stores its characters once — inside the entry when it fits.
    const bool shared = caller == link;
    if (link.size() > UINT32_MAX || caller.size() > UINT32_MAX) return std::nullopt;
    const std::size_t bytes = link.size() + (shared ? 0 : caller.size());
    char* heap = nullptr;
    if (bytes > sizeof(detail::subject_entry_t::inline_chars)) {
        heap = static_cast<char*>(::operator new(bytes, std::nothrow));
        if (heap == nullptr) return std::nullopt;
    }
    if (!index_reserve_one(t)) {
        ::operator delete(heap);
        return std::nullopt;
    }
    const std::uint32_t index = take_index(t);
    if (index == 0) {
        ::operator delete(heap);
        return std::nullopt;
    }
    detail::subject_entry_t& e = entry(index);
    char* const text = heap != nullptr ? heap : e.inline_chars;
    if (!link.empty()) std::memcpy(text, link.data(), link.size());
    if (!shared && !caller.empty()) std::memcpy(text + link.size(), caller.data(), caller.size());
    e.link = text;
    e.caller = shared ? text : text + link.size();
    e.link_len = static_cast<std::uint32_t>(link.size());
    e.caller_len = static_cast<std::uint32_t>(caller.size());
    e.refs = 1;
    e.hash = h;
    e.next_free = 0;
    index_insert(t.index, t.capacity, index);
    ++t.live;
    return subject_id_t{index | link_bit};
}

void release_subject(subject_id_t id) noexcept {
    if (!id.valid()) return;
    subject_table_t& t = table();
    const std::lock_guard lock(t.m);
    detail::subject_entry_t& e = entry(id.index());
    if (--e.refs != 0) return;
    index_erase(t, id.index());
    if (e.link != e.inline_chars) ::operator delete(const_cast<char*>(e.link));
    e = detail::subject_entry_t{};
    e.next_free = t.free_head;
    t.free_head = id.index();
    --t.live;
}

std::size_t live_subject_count() noexcept {
    subject_table_t& t = table();
    const std::lock_guard lock(t.m);
    return t.live;
}

}  // namespace tr::graph
