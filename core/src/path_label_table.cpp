/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * RFC-0027 §§4.3.1, 7, 8.3 — the path-label mint table's body. Three mechanisms and
 * nothing else: a free list over slots grown on demand from the injected store, a
 * flat per-peer census the §8.3 ceiling is charged against, and the saturate-and-
 * retire generation rule, which is the one place in this file where doing the
 * obvious thing (wrapping) would be a mis-delivery rather than a bug.
 */

#include "libtracer/path_label_table.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "libtracer/path_label.hpp"

/**
 * @file
 * @brief The mint / lookup / release body of @ref tr::net::path_label_table_t.
 */

namespace tr::net {

path_label_table_t::path_label_table_t(std::pmr::memory_resource* mr, std::size_t capacity,
                                       std::size_t max_per_peer)
    : capacity_(std::min(capacity, wire::kPathLabelSlotSpace)),
      max_per_peer_(max_per_peer),
      slots_(mr),
      peers_(mr) {}

std::optional<wire::path_label_t> path_label_table_t::mint(peer_handle_t peer,
                                                           path_label_target_t target) {
    // An unowned label is one anyone could present, which would give away lookup's owner check
    // — half of §4.1's node-scope rule. Refused before anything is spent.
    if (!peer.valid()) {
        const std::lock_guard<std::mutex> g(m_);
        ++refused_;
        return std::nullopt;
    }

    const std::lock_guard<std::mutex> g(m_);

    // The §8.3 per-peer ceiling, charged against the seam's own peer identity (§10). Checked
    // before a slot is taken so a refused peer costs the table nothing at all.
    const std::uint64_t key = peer.bits();
    auto census = std::find_if(peers_.begin(), peers_.end(),
                               [key](const peer_census_t& c) { return c.peer == key; });
    if (max_per_peer_ != kNoPeerCeiling && census != peers_.end() &&
        census->live >= max_per_peer_) {
        ++refused_;
        return std::nullopt;
    }

    // A free slot first, a fresh one second. Retired slots (§4.3.1) never reach the free list,
    // so "the list is empty and the vector is at capacity" is the whole exhaustion condition —
    // there is no scan that could accidentally revive one.
    std::uint32_t index = kNoSlot;
    if (free_head_ != kNoSlot) {
        index = free_head_;
        free_head_ = slots_[index].next_free;
    } else if (slots_.size() < capacity_) {
        index = static_cast<std::uint32_t>(slots_.size());
        slots_.push_back(slot_t{});
    } else {
        // Exhausted: REFUSE, never evict (§8.3). A live label is not a cache entry, and
        // reclaiming one would turn a bounded-resource condition into avoidable NOT_FOUND
        // round trips on flows that were working. The caller leaves the part a string.
        ++refused_;
        return std::nullopt;
    }

    slot_t& s = slots_[index];
    s.owner = peer;
    s.target = target;
    s.live = true;
    s.next_free = kNoSlot;
    ++live_;

    if (census == peers_.end()) {
        peers_.push_back(peer_census_t{.peer = key, .live = 1});
    } else {
        ++census->live;
    }

    return wire::path_label_t{.index = static_cast<std::uint16_t>(index),
                              .generation = s.generation};
}

std::optional<path_label_target_t> path_label_table_t::lookup(peer_handle_t peer,
                                                              wire::path_label_t label) const {
    const std::lock_guard<std::mutex> g(m_);
    if (label.index >= slots_.size()) return std::nullopt;
    const slot_t& s = slots_[label.index];
    // All four refusals are one answer (§7.2): no live label, a stale generation, or a label
    // minted for someone else. Nothing is repaired here and nothing is guessed — the caller
    // answers NOT_FOUND and the peer's own string original is the recovery.
    if (!s.live || s.generation != label.generation || !(s.owner == peer)) return std::nullopt;
    return s.target;
}

void path_label_table_t::release_locked(slot_t& s, std::uint32_t index) {
    s.live = false;
    --live_;

    const std::uint64_t key = s.owner.bits();
    auto census = std::find_if(peers_.begin(), peers_.end(),
                               [key](const peer_census_t& c) { return c.peer == key; });
    if (census != peers_.end() && --census->live == 0) peers_.erase(census);

    // §4.3.1, the load-bearing four lines. The generation advances so the label the peer still
    // holds compares unequal on its next frame; when the advance would reach the top it STOPS
    // there and the slot is retired permanently — never returned to the free list, not after
    // reclamation, not after the peer departs, not after the table empties. A wrap would let a
    // stale label validate against an unrelated occupant, which is #603's misroute rather than
    // a drop, and the doc set closes that class by construction in both of its (slot,
    // generation) fields rather than pricing its probability.
    if (s.generation >= wire::kPathLabelMaxGeneration - 1) {
        s.generation = wire::kPathLabelMaxGeneration;
        ++retired_;
        return;
    }
    ++s.generation;
    s.next_free = free_head_;
    free_head_ = index;
}

bool path_label_table_t::release(wire::path_label_t label) {
    const std::lock_guard<std::mutex> g(m_);
    if (label.index >= slots_.size()) return false;
    slot_t& s = slots_[label.index];
    // A stale release is ignored rather than applied: the slot has already moved on, and
    // honouring a late departure would retire the successor's live label.
    if (!s.live || s.generation != label.generation) return false;
    release_locked(s, label.index);
    return true;
}

std::size_t path_label_table_t::release_peer(peer_handle_t peer) {
    const std::lock_guard<std::mutex> g(m_);
    std::size_t released = 0;
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        slot_t& s = slots_[i];
        if (!s.live || !(s.owner == peer)) continue;
        release_locked(s, static_cast<std::uint32_t>(i));
        ++released;
    }
    return released;
}

std::size_t path_label_table_t::live_count() const {
    const std::lock_guard<std::mutex> g(m_);
    return live_;
}

std::size_t path_label_table_t::live_count_for(peer_handle_t peer) const {
    const std::lock_guard<std::mutex> g(m_);
    const std::uint64_t key = peer.bits();
    const auto census = std::find_if(peers_.begin(), peers_.end(),
                                     [key](const peer_census_t& c) { return c.peer == key; });
    return census == peers_.end() ? 0 : census->live;
}

std::size_t path_label_table_t::retired_slots() const {
    const std::lock_guard<std::mutex> g(m_);
    return retired_;
}

std::size_t path_label_table_t::refused_mints() const {
    const std::lock_guard<std::mutex> g(m_);
    return refused_;
}

}  // namespace tr::net
