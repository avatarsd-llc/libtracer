/**
 * @file
 * @brief `link_index_t` — the per-link departure index's slots, carry, and name doors (#1071).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Moved out of `graph.cpp` verbatim (#1710). The whole-link and route-scoped evictions that
 * consume @ref tr::graph::link_index_t::candidates stay in `graph.cpp`: they walk the vertex
 * tree under the graph's map lock, which this unit never takes.
 */

#include "libtracer/link_index.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace tr::graph {

namespace {

/**
 * @brief Re-establish one link's candidate list as SORTED and unique, in place.
 *
 * The list is a sorted prefix plus an unsorted tail (see `link_index_t::index_vertex`); this
 * folds the tail in. The `unique` pass is belt-and-braces now that the insert is idempotent —
 * a duplicate would be a SPACE problem only, never a correctness one, since visiting a vertex
 * twice costs a second `vertex_t::evict_link_edges` that finds the link's edges already gone
 * and reports 0.
 */
void compact_candidates(std::pmr::vector<vertex_t*>& vs) {
    std::sort(vs.begin(), vs.end());
    vs.erase(std::unique(vs.begin(), vs.end()), vs.end());
}

/**
 * @brief Is @p v already a candidate in @p vs, whose first @p sorted entries are sorted?
 *
 * The membership half of `link_index_t::index_vertex`'s idempotent insert: a binary search
 * over the sorted prefix, then a scan of the tail the compaction floor bounds.
 *
 * Written out rather than composed from `std::binary_search` + `std::find`, which is what it
 * plainly is. While this lived in `graph.cpp`, the two extra `<algorithm>` instantiations
 * enlarged that translation unit enough to move GCC's inter-procedural budget, and the budget
 * was spent on the DELIVERY path: `vertex_t::copy_published` +667 B and `graph_t::fan_out`
 * +48 B, for a control-plane change that touches neither. This unit no longer shares that
 * budget (#1710); the hand-written form is kept so the move itself changes no code.
 */
bool candidates_contain(const std::pmr::vector<vertex_t*>& vs, std::size_t sorted,
                        const vertex_t* v) {
    std::size_t lo = 0;
    std::size_t hi = sorted;
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (vs[mid] == v) return true;
        if (vs[mid] < v)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (std::size_t i = sorted; i < vs.size(); ++i)
        if (vs[i] == v) return true;
    return false;
}

}  // namespace

/** @brief Slot @p i's spelling — inline, overflowed, or empty for a dead slot. */
std::string_view link_index_t::slot_name(std::uint32_t i) const {
    const link_slot_t& s = slots_[i];
    if (s.len != kOverflowNameLen) return std::string_view(s.name, s.len);
    for (const link_long_name_t& l : long_names_)
        if (l.slot == i) return l.text;
    return {};
}

/** @brief THE NAME DOOR: a linear scan of the live slots. */
std::uint32_t link_index_t::find_slot(std::string_view name) const {
    // THE NAME DOOR, and it is a scan (see link_index.hpp's "DENSE SLOT VECTOR" section). A dead
    // slot's name is empty and `name` never is here, so the liveness test rides the comparison.
    for (std::uint32_t i = 0; i < slots_.size(); ++i)
        if (slot_name(i) == name) return i;
    return kNoSlot;
}

/** @brief Give slot @p i the spelling @p name, inline when it fits. */
void link_index_t::name_slot(std::uint32_t i, std::string_view name) {
    link_slot_t& s = slots_[i];
    if (name.size() <= kInlineNameChars) {
        std::memcpy(s.name, name.data(), name.size());
        s.len = static_cast<std::uint8_t>(name.size());
        return;
    }
    long_names_.push_back(link_long_name_t{i, std::pmr::string(name, entry_mr_)});
    s.len = kOverflowNameLen;
}

/** @brief Mint-or-find @p name with the lock held. */
link_id_t link_index_t::intern_locked(std::string_view name) {
    if (name.empty()) return {};  // the #1056 empty-key rule — the LOCAL spelling is not a link
    // Mint-or-FIND: the same spelling always answers the same live token, which is the
    // same-NAME redial ordering #1263 pinned. A redial that comes back as `p3` re-enters the
    // slot `p3` already has rather than stranding it behind a second one.
    if (const std::uint32_t i = find_slot(name); i != kNoSlot)
        return link_id_t{i, slots_[i].generation};
    std::uint32_t i = 0;
    if (!free_.empty()) {
        i = free_.back();
        free_.pop_back();
    } else {
        // Grown to EXACTLY what is needed rather than doubled. This runs once per link-up
        // and the figure #1266 is judged on is bytes at rest in the user-pinned arena
        // (#1160's budget on the C6), where a doubling vector of 64-byte slots would hand
        // back up to a fifth of the saving as capacity slack. The copy is a move of a
        // handful of trivially-relocatable records on a control-plane-cold path.
        if (slots_.size() == slots_.capacity()) slots_.reserve(slots_.size() + 1);
        slots_.push_back(
            link_slot_t{.e = link_entry_t{.vs = std::pmr::vector<vertex_t*>(entry_mr_)}});
        i = static_cast<std::uint32_t>(slots_.size() - 1);
    }
    link_slot_t& s = slots_[i];
    if (s.generation == 0) s.generation = 1;  // fresh; a released slot was bumped on release
    name_slot(i, name);
    return link_id_t{i, s.generation};
}

/** @brief The fallback counter, read under the lock. */
std::size_t link_index_t::name_lookups() const {
    const std::lock_guard lock(mutex_);
    return name_lookups_;
}

/** @brief Mint-or-find @p name under the lock. */
link_id_t link_index_t::intern(std::string_view name) {
    const std::lock_guard lock(mutex_);
    return intern_locked(name);
}

/** @brief Mint-or-find @p name, trying the caller's slot hint first. */
link_id_t link_index_t::intern_hinted(std::string_view name, std::uint32_t& hint) {
    if (name.empty()) return {};  // the #1056 empty-key rule, and the hint stays put
    const std::lock_guard lock(mutex_);
    // The whole of the fast path: a bounds check and a name compare, against the SCAN the
    // else-arm would run. The name compare is not an optimisation here, it is the validation
    // — see `graph_t::intern_link_hinted` for why the stamp is deliberately not remembered by
    // the caller.
    if (hint < slots_.size() && slot_name(hint) == name)
        return link_id_t{hint, slots_[hint].generation};
    const link_id_t fresh = intern_locked(name);
    // Written back only on a token, so a name that could not be interned (an exhausted arena)
    // leaves whatever the caller had rather than poisoning the word with a slot that is not
    // this link's. A hint that was already right and simply lost its race is re-derived here
    // at the cost of the scan, which is the cost of not having a hint at all.
    if (fresh.valid()) hint = fresh.slot;
    return fresh;
}

/** @brief Retire @p token's slot, if it is still the live one it names. */
void link_index_t::release(link_id_t token) {
    const std::lock_guard lock(mutex_);
    if (!token.valid() || token.slot >= slots_.size()) return;
    link_slot_t& s = slots_[token.slot];
    if (s.generation != token.generation || s.len == 0) return;  // already released, or stale
    release_slot(token.slot);
}

/** @brief Retire slot @p i with the lock held. */
void link_index_t::release_slot(std::uint32_t i) {
    link_slot_t& s = slots_[i];
    if (s.len == kOverflowNameLen)
        for (std::size_t k = 0; k < long_names_.size(); ++k)
            if (long_names_[k].slot == i) {
                long_names_.erase(long_names_.begin() + static_cast<std::ptrdiff_t>(k));
                break;
            }
    s.len = 0;  // DEAD — a name scan skips it, and no live link can spell itself empty
    // The stamp moves BEFORE the slot can be handed out again, so every token still in flight
    // for the departed link stops validating. That is what makes slot reuse safe rather than
    // merely unlikely: a stale token cannot inherit a successor's candidate list.
    ++s.generation;
    if (s.generation == 0) s.generation = 1;  // wrapped: 0 is reserved for "no token"
    s.e.vs.clear();
    s.e.compacted = 0;
    free_.push_back(i);
}

/** @brief Record @p v under @p link, by the carried token when it validates. */
void link_index_t::index_vertex(std::string_view link, link_id_t token, vertex_t* v) {
    if (link.empty()) return;  // the LOCAL spelling — no link teardown can ever name it
    const std::lock_guard lock(mutex_);
    // THE CARRY (#1417). A valid token whose slot is live AND spells `link` is the entry,
    // reached by subscript: no hash, no find, no key copy. Everything else falls back to
    // interning the name, which is byte-identical in outcome to what every pre-carry caller
    // got — so an absent token, a token released under it, and a token for a DIFFERENT link
    // than the one this admission is keyed by (a mount-routed target rebinds the key to the
    // mount's name; a `field_write` admission keys on its `caller`, #943) all degrade to a
    // lookup instead of mis-indexing. The name compare is what turns "the caller must never
    // carry the wrong token" into "carrying the wrong token cannot lose an edge", and it is a
    // handful of inline bytes against a `std::string_view` already in a register.
    std::uint32_t i = kNoSlot;
    if (token.valid() && token.slot < slots_.size() &&
        slots_[token.slot].generation == token.generation && slot_name(token.slot) == link) {
        i = token.slot;
    } else {
        ++name_lookups_;  // the carry did not reach here — see name_lookups
        const link_id_t fresh = intern_locked(link);
        if (!fresh.valid()) return;
        i = fresh.slot;
    }
    link_entry_t& e = slots_[i].e;
    // IDEMPOTENT — a vertex already listed for this link is not listed again. That is what
    // the declaration has always promised, and what the eviction's bound argument assumes;
    // the code did not do it, and that gap is where the subscribe path's cost actually was
    // (#1266). The predecessor appended unconditionally and squashed duplicates later, so a
    // peer re-subscribing over its own handful of vertices — the steady state, since a
    // subscription is renewed far more often than a new vertex is first subscribed — grew a
    // list oscillating between D and 2D+8 entries: an arena allocation whenever it outgrew
    // its capacity, and an `O(D log D)` sort every D+8 subscribes, forever, for no distinct
    // vertex gained. Measured by ablation, that append-plus-amortized-sort was the larger
    // half of the index's per-subscribe cost.
    //
    // `vs` is `[0, compacted)` SORTED and unique, followed by an unsorted tail the compaction
    // below keeps under `kCompactFloor`. So membership is a binary search over the prefix
    // plus a bounded scan of the tail, with NO memmove — which is what made a fully sorted
    // insert a reject (+19% on this path, #1071, from the N/2-pointer shift the Nth
    // subscription paid). A genuinely new vertex still lands with a bare `push_back`.
    if (candidates_contain(e.vs, e.compacted, v)) return;
    e.vs.push_back(v);
    // With the membership test above the list IS the distinct set, so compaction no longer
    // bounds unbounded growth — nothing can grow it past the vertices this link subscribed
    // on. What it bounds now is the TAIL, i.e. how long the linear half of that test can get:
    // merging every `kCompactFloor` NEW vertices keeps the scan at a handful of pointers, and
    // the sort is paid per new vertex rather than per subscribe.
    if (e.vs.size() - e.compacted >= kCompactFloor) {
        compact_candidates(e.vs);
        e.compacted = e.vs.size();
    }
}

/** @brief The distinct candidate count for @p link, compacting first. */
std::size_t link_index_t::candidate_count(std::string_view link) const {
    if (link.empty()) return 0;
    const std::lock_guard lock(mutex_);
    // The NAME door, unchanged in signature and in answer, and now a scan (#1417). It costs
    // no token and no router: the ESP departure-cost guard calls it on a bare `graph_t`, and
    // that is the property that forced the name into the slot rather than out of the index.
    const std::uint32_t i = find_slot(link);
    if (i == kNoSlot) return 0;
    // Compact before reporting, so the number is the DISTINCT vertex count a caller can
    // reason about rather than an artefact of where the amortized compaction last landed.
    link_entry_t& e = slots_[i].e;
    if (e.vs.size() != e.compacted) {
        compact_candidates(e.vs);
        e.compacted = e.vs.size();
    }
    return e.vs.size();
}

/** @brief The candidate vertices for @p link — copied, or taken with the slot released. */
std::pmr::vector<vertex_t*> link_index_t::candidates(std::string_view link, bool take) {
    const std::lock_guard lock(mutex_);
    const std::uint32_t i = find_slot(link);
    if (i == kNoSlot) return std::pmr::vector<vertex_t*>(entry_mr_);
    // Compact first: a duplicate would cost a second eviction pass over the same vertex.
    link_entry_t& e = slots_[i].e;
    if (e.vs.size() != e.compacted) {
        compact_candidates(e.vs);
        e.compacted = e.vs.size();
    }
    if (!take) return e.vs;  // a copy: the entry outlives this eviction
    std::pmr::vector<vertex_t*> out = std::move(e.vs);
    // RELEASED, not merely emptied — the exact footprint behaviour the erased map entry had,
    // so a node that churns links keeps one slot per link it CURRENTLY holds rather than one
    // per name it has ever seen. The stamp bump inside is what stops a token cached for the
    // departed link from addressing whichever link takes the slot next (#1417); a router that
    // has not yet dropped its cached copy degrades to a name lookup, never to a wrong entry.
    release_slot(i);
    return out;
}

}  // namespace tr::graph
