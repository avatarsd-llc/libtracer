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

path_label_table_t::path_label_table_t(mem::block_source_t* src, std::size_t capacity,
                                       std::size_t max_per_peer, std::size_t max_peers)
    : capacity_(std::min(capacity, wire::kPathLabelSlotSpace)),
      max_per_peer_(max_per_peer),
      max_peers_(std::min(max_peers == kPeersFollowCapacity ? capacity_ : max_peers, capacity_)),
      slots_(src != nullptr ? *src : mem::heap_source()),
      peers_(src != nullptr ? *src : mem::heap_source()) {
    // The SLOT array is charged HERE, once and in full, so the peer-provoked mint path
    // allocates nothing: a mint takes a slot out of storage the table already owns and can
    // never be the operation that finds the source empty. A source that cannot serve it does
    // not throw and does not abort — it leaves this host at kMintsNothing, which §6.3 already
    // defines as a conformant node: every part travels as a string and nothing on the route
    // notices. capacity() reports the 0, so a deployment that sized a table and got none can
    // see that it did.
    //
    // The CENSUS is charged differently, and the asymmetry is deliberate. Its ceiling is
    // max_peers_ — derived from capacity_, because a peer holds an entry only while it holds
    // a live label — but reserving that many entries up front would charge a 65 536-slot
    // table a megabyte for peers a node will never have (a node carries FEW peers; that is
    // the premise the flat scan is built on). So it takes a small floor here and grows on
    // demand up to its ceiling, and every growth answer is checked: exhaustion is a refusal
    // by value, counted, exactly like reaching the ceiling itself.
    if (!slots_.reserve(capacity_)) {
        capacity_ = 0;
        max_peers_ = 0;
        return;
    }
    (void)peers_.reserve(std::min(max_peers_, kPeerPrereserve));
}

path_label_table_t::peer_census_t* path_label_table_t::find_census(std::uint64_t key) noexcept {
    // A flat linear scan, and it stays one for the reason the member's declaration gives: a
    // node carries few peers and an entry dies the moment its count reaches zero.
    for (std::size_t i = 0; i < peers_.size(); ++i)
        if (peers_[i].peer == key) return &peers_[i];
    return nullptr;
}

void path_label_table_t::drop_census(peer_census_t* c) noexcept {
    // Swap with the back and shrink. The census is unordered by construction — it is scanned,
    // never indexed into by anything that outlives the scan — so moving one entry costs
    // nothing a shift would have saved, and the array never gives its block back mid-life.
    *c = peers_[peers_.size() - 1];
    peers_.pop_back();
}

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
    peer_census_t* census = find_census(key);
    if (max_per_peer_ != kNoPeerCeiling && census != nullptr && census->live >= max_per_peer_) {
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
    } else if (slots_.size() < capacity_ && slots_.push_back(slot_t{})) {
        index = static_cast<std::uint32_t>(slots_.size() - 1);
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

    if (census == nullptr) {
        // A first label for this peer needs a census entry, and the entry is what the §8.3
        // ceiling is charged against — so if there is no room for one there is no way to
        // hold this peer to its share, and the mint is refused rather than run uncounted.
        // Reserved to max_peers_ at construction, so this cannot fail before the ceiling
        // does; the `push_back` answer is checked anyway, because a bound that is only
        // enforced by an invariant elsewhere in the file is a bound one edit away from gone.
        if (peers_.size() >= max_peers_ ||
            !peers_.push_back(peer_census_t{.peer = key, .live = 1})) {
            // Give the slot straight back: nothing was published, so the free list and the
            // live count must not remember it.
            s.live = false;
            --live_;
            s.next_free = free_head_;
            free_head_ = index;
            ++refused_;
            return std::nullopt;
        }
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

    peer_census_t* census = find_census(s.owner.bits());
    if (census != nullptr && --census->live == 0) drop_census(census);

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
    for (std::size_t i = 0; i < peers_.size(); ++i)
        if (peers_[i].peer == key) return peers_[i].live;
    return 0;
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
