/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * can_reassembly (#55): multi-frame CAN payload reassembly via libtracer's own
 * address-shift slicing / advertise+id-match — NOT ISO-TP (ADR-0030). Each CAN
 * frame is a slice; `(origin, ts) + index` chains slices into a rope. The same
 * reassembly model that "spans a 9-byte elided CAN sample → a GB advertised rope
 * group" (CONTEXT.md *Advertise + id-match*), reused so CAN stays uniform with
 * UDP/QUIC scatter-gather.
 *
 * Lives in `tr::net`, beside `transport_can` — the reassembly buffer is a
 * transport-plane concern, and the earlier `tr::mem::mem_can_reassembly_t`
 * naming was a self-admitted L0→L1 layer inversion (an L0 `tr::mem` type
 * referencing the L1 `rope_t` it assembles). Resolved by the rehome (ADR-0048
 * round 2): the reassembly is a `tr::net` component that composes L1 views into a
 * rope, exactly as any transport does.
 *
 * Storage is drawn from an injected `std::pmr::memory_resource` and the group
 * count is bounded by config, so on a constrained node exhaustion is a bounded
 * drop (evict-oldest + a `dropped_groups` counter), never an OOM (the
 * no-synthetic-limits doctrine: bounds are injected resources / config, never a
 * hardcoded magic number). The defaults — the process heap, unbounded — preserve
 * the pre-rehome behavior; a target injects a stack resource + `max_groups` to
 * bound it (the per-connection `:settings` path).
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory_resource>
#include <optional>

#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief `tr::net` multi-frame CAN reassembly buffer: `can_reassembly_t`.
 */

namespace tr::net {

/**
 * @brief A 16-byte node/peer identity — mirrors the ROUTER `origin_peer_id`.
 *
 * Held as raw bytes (not `tr::net::peer_id_t`) so the reassembly buffer stays a
 * self-contained framing primitive; `transport_can` fills it from the CAN id.
 */
using can_origin_id_t = std::array<std::uint8_t, 16>;

/**
 * @brief The in-flight identity of one address-shift group: `(origin, ts)`.
 *
 * The collision-free `(origin_peer_id, ts)` identity used by cycle-dedup and
 * slice-grouping (CONTEXT.md *Address-shift slicing*). Each slice's `index` gives
 * its position within the group.
 */
struct reassembly_key_t {
    can_origin_id_t origin{}; /**< @brief The originating node id (16 bytes). */
    std::uint64_t ts = 0;     /**< @brief The group's per-producer monotonic timestamp. */

    /** @brief Total ordering, so the key works as a `std::map` key (value type). */
    [[nodiscard]] auto operator<=>(const reassembly_key_t&) const = default;
    /** @brief Field-wise equality (value type). */
    [[nodiscard]] bool operator==(const reassembly_key_t&) const = default;
};

/**
 * @brief Reassembles multi-frame CAN payloads from out-of-order slices.
 *
 * A slice (one CAN data field, as a @ref tr::view::view_t) is added under its
 * group key and index; slices may arrive in any order. Totality is **opt-in**
 * (@ref set_expected_count, the advertise manifest's slice count): with it set, a
 * dropped *interior* slice is detectable (@ref has_interior_gap) and the group is
 * @ref is_complete only when every index `0..count-1` is present; a dropped
 * *trailing* slice is undetectable without it (ADR-0011 totality-opt-in). @ref
 * assemble chains the slices, in index order, into a @ref tr::view::rope_t with
 * zero copies.
 *
 * Structure and slices are drawn from the injected memory resource; when
 * `max_groups` is non-zero and a new group would exceed it, the oldest group
 * is evicted (its buffered slices freed) and @ref dropped_groups is incremented —
 * a bounded drop rather than unbounded growth.
 */
class can_reassembly_t {
   public:
    /**
     * @brief Construct over @p mr, bounding the live group count at @p max_groups.
     * @param mr         Where the group/slice structure is allocated
     *                   (default: the process heap).
     * @param max_groups Live-group ceiling; `0` means unbounded (the default, and
     *                   the pre-rehome behavior).
     */
    explicit can_reassembly_t(std::pmr::memory_resource* mr = std::pmr::new_delete_resource(),
                              std::size_t max_groups = 0)
        : max_groups_(max_groups), groups_(mr), slices_(mr) {}

    /**
     * @brief Add (or replace) slice @p index of group @p key.
     * @param key   The `(origin, ts)` group identity.
     * @param index The zero-based slice position.
     * @param slice The slice's bytes (one CAN data field), borrowed zero-copy.
     */
    void add_slice(const reassembly_key_t& key, std::uint32_t index, tr::view::view_t slice) {
        touch_group(key);
        slices_.insert_or_assign(slice_id_t{key, index}, std::move(slice));
    }

    /**
     * @brief Declare the expected slice count of group @p key (totality opt-in).
     * @param key   The group identity.
     * @param count The number of slices the complete group contains.
     */
    void set_expected_count(const reassembly_key_t& key, std::uint32_t count) {
        touch_group(key).expected = count;
    }

    /** @brief True when group @p key is being tracked (has a slice or an expected count). */
    [[nodiscard]] bool contains(const reassembly_key_t& key) const {
        return groups_.find(key) != groups_.end();
    }

    /** @brief Number of slices currently buffered for group @p key (0 if unknown). */
    [[nodiscard]] std::size_t slice_count(const reassembly_key_t& key) const {
        std::size_t n = 0;
        for (auto it = slices_.lower_bound(slice_id_t{key, 0});
             it != slices_.end() && it->first.group == key; ++it) {
            ++n;
        }
        return n;
    }

    /**
     * @brief True when an interior slice is missing (a hole below the highest index).
     *
     * Detects a dropped *interior* slice even before the count is known; a missing
     * *trailing* slice is not an interior gap (ADR-0011).
     */
    [[nodiscard]] bool has_interior_gap(const reassembly_key_t& key) const {
        std::size_t n = 0;
        std::uint32_t highest = 0;
        for (auto it = slices_.lower_bound(slice_id_t{key, 0});
             it != slices_.end() && it->first.group == key; ++it) {
            ++n;
            highest = it->first.index;  // ordered by index — the last seen is the highest
        }
        if (n == 0) return false;
        return n != static_cast<std::size_t>(highest) + 1;
    }

    /**
     * @brief True when @p key has its expected count set and every index is present.
     *
     * Requires @ref set_expected_count (totality opt-in); without it, completeness
     * is undecidable (a trailing drop is invisible) and this returns false.
     */
    [[nodiscard]] bool is_complete(const reassembly_key_t& key) const {
        const auto it = groups_.find(key);
        if (it == groups_.end() || !it->second.expected) return false;
        const std::uint32_t expected = *it->second.expected;
        std::size_t n = 0;
        std::uint32_t highest = 0;
        for (auto s = slices_.lower_bound(slice_id_t{key, 0});
             s != slices_.end() && s->first.group == key; ++s) {
            ++n;
            highest = s->first.index;
        }
        if (n != static_cast<std::size_t>(expected)) return false;
        return expected == 0 || highest == expected - 1;
    }

    /**
     * @brief Chain the complete group's slices, in index order, into one rope.
     * @param key The group identity.
     * @return The reassembled @ref tr::view::rope_t, or `std::nullopt` unless the
     *         group @ref is_complete (totality must be satisfied first).
     */
    [[nodiscard]] std::optional<tr::view::rope_t> assemble(const reassembly_key_t& key) const {
        if (!is_complete(key)) return std::nullopt;
        tr::view::rope_t r;
        for (auto it = slices_.lower_bound(slice_id_t{key, 0});
             it != slices_.end() && it->first.group == key; ++it) {
            r.append(it->second);
        }
        return r;
    }

    /** @brief Drop all buffered state for group @p key (after assembly or timeout). */
    void erase(const reassembly_key_t& key) { drop_group(key); }

    /** @brief Count of groups evicted because `max_groups` was reached (never OOM). */
    [[nodiscard]] std::uint64_t dropped_groups() const noexcept { return dropped_groups_; }

   private:
    // A whole-buffer slice identity: `(group, index)`, so a group's slices are a
    // contiguous, index-ordered run in one flat map (no nested pmr container).
    struct slice_id_t {
        reassembly_key_t group{};
        std::uint32_t index = 0;
        [[nodiscard]] auto operator<=>(const slice_id_t&) const = default;
    };

    struct group_meta_t {
        std::optional<std::uint32_t> expected;  // totality opt-in.
        std::uint64_t seq = 0;                  // insertion order, for evict-oldest.
    };

    // Track a group, creating it (bound-enforced) if new; returns its metadata.
    group_meta_t& touch_group(const reassembly_key_t& key) {
        const auto it = groups_.find(key);
        if (it != groups_.end()) return it->second;
        if (max_groups_ != 0 && groups_.size() >= max_groups_) evict_oldest();
        return groups_.emplace(key, group_meta_t{.expected = std::nullopt, .seq = next_seq_++})
            .first->second;
    }

    // Erase a group's metadata and its slice run (the flat-map range for the group).
    void drop_group(const reassembly_key_t& key) {
        groups_.erase(key);
        const auto lo = slices_.lower_bound(slice_id_t{key, 0});
        auto hi = lo;
        while (hi != slices_.end() && hi->first.group == key) ++hi;
        slices_.erase(lo, hi);
    }

    // Evict the oldest-inserted group to make room (bounded drop, counted).
    void evict_oldest() {
        auto oldest = groups_.begin();
        for (auto it = groups_.begin(); it != groups_.end(); ++it) {
            if (it->second.seq < oldest->second.seq) oldest = it;
        }
        if (oldest == groups_.end()) return;
        const reassembly_key_t key = oldest->first;
        drop_group(key);
        ++dropped_groups_;
    }

    std::size_t max_groups_ = 0;
    std::uint64_t next_seq_ = 0;
    std::uint64_t dropped_groups_ = 0;
    std::pmr::map<reassembly_key_t, group_meta_t> groups_;
    std::pmr::map<slice_id_t, tr::view::view_t> slices_;
};

}  // namespace tr::net
