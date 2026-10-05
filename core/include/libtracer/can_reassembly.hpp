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
 * Lives in `tr::net`, beside `can_transport_t` — the reassembly buffer is a
 * transport-plane concern, and the earlier `tr::mem::mem_can_reassembly_t`
 * naming was a self-admitted L0→L1 layer inversion (an L0 `tr::mem` type
 * referencing the L1 `rope_t` it assembles). Resolved by the rehome (ADR-0048
 * round 2): the reassembly is a `tr::net` component that composes L1 views into a
 * rope, exactly as any transport does.
 *
 * Storage is drawn from an injected `tr::mem::block_source_t` (the one allocation seam,
 * ADR-0083 — #1780) and the group count is bounded by config, so on a constrained node
 * exhaustion is a bounded drop (evict-oldest + a `dropped_groups` counter, or a refused
 * slice the caller discards), never an OOM (the no-synthetic-limits doctrine: bounds are
 * injected resources / config, never a hardcoded magic number). The defaults — the process
 * net sub-pool, unbounded — preserve the pre-rehome behavior; a target injects a bounded
 * source + `max_groups` to bound it (the per-connection `:settings` path).
 *
 * A count bound alone does not reclaim a group that will NEVER complete — the
 * exact residue a lost advertise or a lost data slice leaves behind, since
 * `erase` is reached only after `is_complete` (#912). So the buffer also ages
 * out: the caller stamps it with its own monotonic clock (`set_now`) and sweeps
 * (`sweep_stale`) on a cadence it chooses. The stamp is opaque here — the buffer
 * stays a pure framing primitive with NO clock of its own, exactly as it has no
 * allocator of its own.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "libtracer/mem_sorted_map.hpp"
#include "libtracer/mem_source.hpp"
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
 * Held as raw bytes so the reassembly buffer stays a self-contained framing
 * primitive; `can_transport_t` fills it from the CAN id.
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

    /** @brief Total ordering, so the key works as a sorted-map key (value type). */
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
 * Structure and slices are drawn from the injected block source; when
 * `max_groups` is non-zero and a new group would exceed it, the oldest group
 * is evicted (its buffered slices freed) and @ref dropped_groups is incremented —
 * a bounded drop rather than unbounded growth. Independently of that count bound,
 * @ref sweep_stale reclaims groups that stopped making progress, which is the only
 * thing that frees a group a lost slice left permanently incomplete.
 */
class can_reassembly_t {
   public:
    /**
     * @brief Construct over @p src, bounding the live group count at @p max_groups.
     * @param src        Where the group/slice structure is drawn from
     *                   (default: the process net sub-pool).
     * @param max_groups Live-group ceiling; `0` means unbounded (the default, and
     *                   the pre-rehome behavior).
     */
    explicit can_reassembly_t(mem::block_source_t& src = mem::net_source(),
                              std::size_t max_groups = 0) noexcept
        : max_groups_(max_groups), groups_(src), slices_(src) {}

    /**
     * @brief Add (or replace) slice @p index of group @p key.
     * @param key   The `(origin, ts)` group identity.
     * @param index The zero-based slice position.
     * @param slice The slice's bytes (one CAN data field), borrowed zero-copy.
     * @retval false The source refused the group or the slice (BACKPRESSURE): the slice was
     *               not taken, and the caller abandons the group (@ref discard).
     */
    [[nodiscard]] bool add_slice(const reassembly_key_t& key, std::uint32_t index,
                                 tr::view::view_t slice) noexcept {
        if (touch_group(key) == nullptr) return false;
        const slice_id_t id{key, index};
        if (tr::view::view_t* const held = slices_.find(id)) {
            *held = std::move(slice);
            return true;
        }
        return slices_.try_emplace(id, std::move(slice)).value != nullptr;
    }

    /**
     * @brief Declare the expected slice count of group @p key (totality opt-in).
     * @param key   The group identity.
     * @param count The number of slices the complete group contains.
     * @retval false The source refused a new group's entry (BACKPRESSURE): nothing was
     *               recorded.
     */
    [[nodiscard]] bool set_expected_count(const reassembly_key_t& key,
                                          std::uint32_t count) noexcept {
        group_meta_t* const g = touch_group(key);
        if (g == nullptr) return false;
        g->expected = count;
        return true;
    }

    /** @brief True when group @p key is being tracked (has a slice or an expected count). */
    [[nodiscard]] bool contains(const reassembly_key_t& key) const noexcept {
        return groups_.contains(key);
    }

    /** @brief Number of slices currently buffered for group @p key (0 if unknown). */
    [[nodiscard]] std::size_t slice_count(const reassembly_key_t& key) const noexcept {
        std::size_t n = 0;
        for (std::size_t i = first_slice(key); in_group(i, key); ++i) ++n;
        return n;
    }

    /**
     * @brief True when an interior slice is missing (a hole below the highest index).
     *
     * Detects a dropped *interior* slice even before the count is known; a missing
     * *trailing* slice is not an interior gap (ADR-0011).
     */
    [[nodiscard]] bool has_interior_gap(const reassembly_key_t& key) const noexcept {
        std::size_t n = 0;
        std::uint32_t highest = 0;
        for (std::size_t i = first_slice(key); in_group(i, key); ++i) {
            ++n;
            highest = slices_.at(i).key.index;  // ordered by index — the last is the highest
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
    [[nodiscard]] bool is_complete(const reassembly_key_t& key) const noexcept {
        const group_meta_t* const g = groups_.find(key);
        if (g == nullptr || !g->expected) return false;
        const std::uint32_t expected = *g->expected;
        std::size_t n = 0;
        std::uint32_t highest = 0;
        for (std::size_t i = first_slice(key); in_group(i, key); ++i) {
            ++n;
            highest = slices_.at(i).key.index;
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
        for (std::size_t i = first_slice(key); in_group(i, key); ++i) r.append(slices_.at(i).value);
        return r;
    }

    /** @brief Drop all buffered state for group @p key (after assembly or timeout). */
    void erase(const reassembly_key_t& key) noexcept { drop_group(key); }

    /**
     * @brief Abandon group @p key as one that will NEVER complete — erase it AND count it.
     *
     * The counted twin of @ref erase. `erase` is the post-delivery release: the group's
     * bytes already reached the receiver, so nothing was lost and nothing is counted.
     * This is the caller-side abandon — the ingress path could not own a slice's bytes
     * (allocation refusal), so the group is dead and its buffered slices are reclaimed
     * BEFORE delivery. That is exactly what @ref dropped_groups counts, whatever forced
     * it: a `max_groups` eviction, a @ref sweep_stale age-out, or this.
     *
     * Silence is the alternative this exists to remove: without it a caller either
     * fabricates a placeholder slice (delivering a byte-wrong short frame as valid) or
     * calls @ref erase and loses a whole group with no counter moving (#911).
     *
     * @retval false Nothing was tracked under @p key — no group, so no drop to count.
     */
    bool discard(const reassembly_key_t& key) noexcept {
        if (!groups_.contains(key)) return false;
        drop_group(key);
        ++dropped_groups_;
        return true;
    }

    /**
     * @brief Set the monotonic stamp that subsequent touches mark a group with.
     *
     * The buffer holds no clock — the caller feeds one (the CAN binding stamps it
     * once per inbound frame with `steady_clock` milliseconds). The unit is
     * whatever the caller uses, and @ref sweep_stale's age is in the same unit.
     */
    void set_now(std::uint64_t now) noexcept { now_ = now; }

    /**
     * @brief Erase every group untouched for longer than @p max_age (the stale sweep).
     *
     * A group is "touched" when a slice is added or its expected count is set, so
     * one that stopped making progress — a lost data slice, or an advertise whose
     * group never materialized — ages out here. Without this, such a group is never
     * @ref is_complete, so @ref erase is never reached and its buffered slices are
     * pinned for the process's life (#912). Each erased group ticks @ref
     * dropped_groups, exactly as a `max_groups` eviction does: one counter for
     * "a group's buffered slices were reclaimed before delivery", whatever forced it.
     *
     * @return How many groups were erased.
     * @note Ages against the stamp last given to @ref set_now; a caller that never
     *       calls it sweeps nothing (every group reads as age 0).
     */
    std::size_t sweep_stale(std::uint64_t max_age) noexcept {
        std::size_t swept = 0;
        for (std::size_t i = 0; i < groups_.size();) {
            // Guard a non-monotonic stamp: an entry from "the future" is not stale.
            const std::uint64_t touched = groups_.at(i).value.last_touch;
            if (now_ < touched || now_ - touched <= max_age) {
                ++i;
                continue;
            }
            const reassembly_key_t key = groups_.at(i).key;  // index `i` now holds the next group
            drop_group(key);
            ++dropped_groups_;
            ++swept;
        }
        return swept;
    }

    /**
     * @brief Count of groups reclaimed before delivery — a `max_groups` eviction
     *        or a @ref sweep_stale age-out (never an OOM).
     */
    [[nodiscard]] std::uint64_t dropped_groups() const noexcept { return dropped_groups_; }

   private:
    // A whole-buffer slice identity: `(group, index)`, so a group's slices are a
    // contiguous, index-ordered run in one flat map (no nested container).
    struct slice_id_t {
        reassembly_key_t group{};
        std::uint32_t index = 0;
        [[nodiscard]] auto operator<=>(const slice_id_t&) const = default;
    };

    struct group_meta_t {
        std::optional<std::uint32_t> expected;  // totality opt-in.
        std::uint64_t seq = 0;                  // insertion order, for evict-oldest.
        std::uint64_t last_touch = 0;           // caller's stamp at the last progress.
    };

    // Track a group, creating it (bound-enforced) if new; returns its metadata.
    // Every touch restamps it, so a group still receiving slices never ages out
    // and one that stopped making progress does.
    // Null when the source refused a new group's entry.
    group_meta_t* touch_group(const reassembly_key_t& key) noexcept {
        if (group_meta_t* const g = groups_.find(key)) {
            g->last_touch = now_;
            return g;
        }
        if (max_groups_ != 0 && groups_.size() >= max_groups_) evict_oldest();
        return groups_
            .try_emplace(
                key, group_meta_t{.expected = std::nullopt, .seq = next_seq_++, .last_touch = now_})
            .value;
    }

    // The index of the first slice of group @p key (or of the first entry after it).
    [[nodiscard]] std::size_t first_slice(const reassembly_key_t& key) const noexcept {
        return slices_.lower_bound(slice_id_t{key, 0});
    }
    // True when slice entry @p i exists and belongs to group @p key.
    [[nodiscard]] bool in_group(std::size_t i, const reassembly_key_t& key) const noexcept {
        return i < slices_.size() && slices_.at(i).key.group == key;
    }

    // Erase a group's metadata and its slice run (the flat-map range for the group).
    void drop_group(const reassembly_key_t& key) noexcept {
        (void)groups_.erase(key);
        const std::size_t lo = first_slice(key);
        while (in_group(lo, key)) slices_.erase_at(lo);
    }

    // Evict the oldest-inserted group to make room (bounded drop, counted).
    void evict_oldest() noexcept {
        if (groups_.empty()) return;
        std::size_t oldest = 0;
        for (std::size_t i = 1; i < groups_.size(); ++i) {
            if (groups_.at(i).value.seq < groups_.at(oldest).value.seq) oldest = i;
        }
        const reassembly_key_t key = groups_.at(oldest).key;
        drop_group(key);
        ++dropped_groups_;
    }

    std::size_t max_groups_ = 0;
    std::uint64_t next_seq_ = 0;
    std::uint64_t dropped_groups_ = 0;
    std::uint64_t now_ = 0;  // the caller's latest stamp (set_now); no clock here.
    mem::sorted_map_t<reassembly_key_t, group_meta_t> groups_;
    mem::sorted_map_t<slice_id_t, tr::view::view_t> slices_;
};

}  // namespace tr::net
