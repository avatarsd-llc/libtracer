/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_can_reassembly (#55): multi-frame CAN payload reassembly via libtracer's
 * own address-shift slicing / advertise+id-match — NOT ISO-TP (ADR-0030). Each
 * CAN frame is a slice; `(origin, ts) + index` chains slices into a rope. This
 * is the same reassembly model that "spans a 9-byte elided CAN sample → a GB
 * advertised rope group" (CONTEXT.md *Advertise + id-match*), reused so CAN stays
 * uniform with UDP/QUIC scatter-gather.
 *
 * LAYERING NOTE (surfaced per CLAUDE.md): the issue (#55) and ADR-0030 name this
 * `mem_can_reassembly` (the L0 `tr::mem` substrate) and require it to "reuse
 * rope.hpp" and yield a rope. Producing/owning an L1 rope_t would, by the
 * precedent in mem_heap.hpp (ADR-0016 §2: "an L1 helper … lives in tr::view, not
 * tr::mem"), normally belong in tr::view. We follow the issue/ADR-0030 naming and
 * keep the type in tr::mem; it is the one L0-named component that references the
 * L1 rope it assembles. The reassembly *buffer* (which indices arrived, totality,
 * gap detection) is the genuine L0 concern; chaining the borrowed slice views is
 * zero-copy (no allocation), so the ADR-0016 §2 ownership concern does not bite.
 */
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>

#include "libtracer/rope.hpp"
#include "libtracer/view.hpp"

/**
 * @file
 * @brief L0 (`tr::mem`) multi-frame CAN reassembly buffer: `mem_can_reassembly_t`.
 */

namespace tr::mem {

/**
 * @brief A 16-byte node/peer identity — mirrors the ROUTER `origin_peer_id`.
 *
 * Held as raw bytes (not `tr::net::peer_id_t`) so the L0 reassembly buffer takes
 * no upward dependency on the transport plane.
 */
using can_origin_id_t = std::array<std::uint8_t, 16>;

/**
 * @brief The in-flight identity of one address-shift group: `(origin, ts)`.
 *
 * The same collision-free `(origin_peer_id, ts)` identity used by cycle-dedup and
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
 */
class mem_can_reassembly_t {
   public:
    mem_can_reassembly_t() = default;

    /**
     * @brief Add (or replace) slice @p index of group @p key.
     * @param key   The `(origin, ts)` group identity.
     * @param index The zero-based slice position.
     * @param slice The slice's bytes (one CAN data field), borrowed zero-copy.
     */
    void add_slice(const reassembly_key_t& key, std::uint32_t index, tr::view::view_t slice) {
        groups_[key].slices[index] = std::move(slice);
    }

    /**
     * @brief Declare the expected slice count of group @p key (totality opt-in).
     * @param key   The group identity.
     * @param count The number of slices the complete group contains.
     */
    void set_expected_count(const reassembly_key_t& key, std::uint32_t count) {
        groups_[key].expected = count;
    }

    /** @brief True when group @p key has at least one buffered slice. */
    [[nodiscard]] bool contains(const reassembly_key_t& key) const {
        return groups_.find(key) != groups_.end();
    }

    /** @brief Number of slices currently buffered for group @p key (0 if unknown). */
    [[nodiscard]] std::size_t slice_count(const reassembly_key_t& key) const {
        const auto it = groups_.find(key);
        return it == groups_.end() ? 0 : it->second.slices.size();
    }

    /**
     * @brief True when an interior slice is missing (a hole below the highest index).
     *
     * Detects a dropped *interior* slice even before the count is known; a missing
     * *trailing* slice is not an interior gap (ADR-0011).
     */
    [[nodiscard]] bool has_interior_gap(const reassembly_key_t& key) const {
        const auto it = groups_.find(key);
        if (it == groups_.end() || it->second.slices.empty()) return false;
        const std::uint32_t highest = it->second.slices.rbegin()->first;
        return it->second.slices.size() != static_cast<std::size_t>(highest) + 1;
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
        if (it->second.slices.size() != static_cast<std::size_t>(expected)) return false;
        return expected == 0 || it->second.slices.rbegin()->first == expected - 1;
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
        for (const auto& [idx, view] : groups_.at(key).slices) {
            (void)idx;
            r.append(view);
        }
        return r;
    }

    /** @brief Drop all buffered state for group @p key (after assembly or timeout). */
    void erase(const reassembly_key_t& key) { groups_.erase(key); }

   private:
    struct group_t {
        std::map<std::uint32_t, tr::view::view_t> slices;  // index -> slice (ordered).
        std::optional<std::uint32_t> expected;             // totality opt-in.
    };
    std::map<reassembly_key_t, group_t> groups_;
};

}  // namespace tr::mem
