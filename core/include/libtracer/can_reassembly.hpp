/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * can_reassembly (#55): multi-frame CAN payload reassembly via libtracer's own
 * address-shift slicing / advertise+id-match — NOT ISO-TP (ADR-0030). Each CAN
 * frame is a slice; `(origin, ts) + index` places it in its group. The advertise names the
 * group's total length, so the receiver draws one segment of that size when it admits the
 * group and copies each slice to its offset: one allocation and a one-link value per
 * message (#2258), whatever the slice count.
 *
 * Lives in `tr::net`, beside `can_transport_t` — the reassembly buffer is a
 * transport-plane concern, and the earlier `tr::mem::mem_can_reassembly_t`
 * naming was a self-admitted L0→L1 layer inversion (an L0 `tr::mem` type
 * referencing the L1 `rope_t` it assembles). Resolved by the rehome (ADR-0048
 * round 2): the reassembly is a `tr::net` component that composes L1 views into a
 * rope, exactly as any transport does.
 *
 * Structure is drawn from an injected `tr::mem::block_source_t` (the one allocation seam,
 * ADR-0083 — #1780), each group's bytes from the caller's receive backend, and the group
 * count is bounded by config, so on a constrained node exhaustion is a bounded drop
 * (evict-oldest, a refused admission + a `dropped_groups` counter, or a refused slice the
 * caller discards), never an OOM (the no-synthetic-limits doctrine: bounds are
 * injected resources / config, never a hardcoded magic number). The defaults — the process
 * net sub-pool, unbounded — preserve the pre-rehome behavior; a target injects a bounded
 * source + `max_groups` to bound it (the per-connection `:settings` path).
 *
 * A count bound alone does not reclaim a group that will NEVER complete — the
 * exact residue a lost advertise or a lost data slice leaves behind, since
 * a group is taken only once it `is_complete` (#912). So the buffer also ages
 * out: the caller stamps it with its own monotonic clock (`set_now`) and sweeps
 * (`sweep_stale`) on a cadence it chooses. The stamp is opaque here — the buffer
 * stays a pure framing primitive with NO clock of its own, exactly as it has no
 * allocator of its own.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <utility>

#include "libtracer/can_framing.hpp"
#include "libtracer/config.hpp"
#include "libtracer/mem_heap.hpp"
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
 * @brief Reassembles multi-frame CAN payloads from out-of-order slices into ONE segment.
 *
 * A group is admitted by its advertise (@ref open_group), which names its slice count and its
 * total length; the two name the producer's slice width. Admission draws ONE segment of the total
 * length from the caller's receive backend, so the receiver pays for the size its peer announced,
 * out of its own store: a size the backend cannot serve, or one the slice geometry cannot carry, is
 * refused and counted on @ref dropped_groups. Each slice (@ref add_slice) is then copied to
 * offset `index` times `width` in that segment; slices may arrive in any order. Totality comes from
 * the advertise (ADR-0011): a dropped *interior* slice is detectable (@ref has_interior_gap) and
 * the group is
 * @ref is_complete only when every index `0..count-1` is present. @ref take hands the segment out
 * owning, trimmed to the total: one allocation, one copy and a one-link value per message,
 * where the per-slice buffer it replaces drew one segment and one map entry per slice (#2258).
 *
 * The segment also carries the group's slice-presence bits, `ceil(count / 8)` bytes past the
 * total length, so a re-sent slice is still counted once without a second draw. The delivered
 * view stops at the total, so the bits never reach the receiver.
 *
 * Admission is bounded per link, by caps fixed at construction: a message longer than the
 * link's `max_message` is refused, and so is a group whose segment would take the link's bytes
 * in flight, summed over every open group, past `max_inflight`. A full budget refuses the
 * newcomer; it never evicts a group already open. Both refusals tick @ref dropped_groups.
 *
 * Structure is drawn from the injected block source; when `max_groups` is non-zero and a new
 * group would exceed it, the oldest group is evicted (its segment freed) and @ref
 * dropped_groups is incremented: a bounded drop rather than unbounded growth. Independently of
 * that count bound, @ref sweep_stale reclaims groups that stopped making progress, which is the
 * only thing that frees a group a lost slice left permanently incomplete.
 */
class can_reassembly_t {
   public:
    /**
     * @brief Construct over @p src, bounding the live group count at @p max_groups and the
     *        bytes admission may draw at @p max_message and @p max_inflight.
     * @param src          Where the group structure is drawn from
     *                     (default: the process net sub-pool).
     * @param max_groups   Live-group ceiling; `0` means unbounded (the default, and
     *                     the pre-rehome behavior).
     * @param max_message  The longest message one group may claim, in bytes.
     * @param max_inflight The most segment bytes all open groups may hold together.
     */
    explicit can_reassembly_t(
        mem::block_source_t& src = mem::net_source(), std::size_t max_groups = 0,
        std::size_t max_message = graph::config_t::kCanMaxMessageBytes,
        std::size_t max_inflight = graph::config_t::kCanRxInflightBytes) noexcept
        : max_groups_(max_groups),
          max_message_(max_message),
          max_inflight_(max_inflight),
          groups_(src) {}

    /** @brief Releases every open group's segment to the backend it came from. */
    ~can_reassembly_t() {
        for (std::size_t i = 0; i < groups_.size(); ++i) release(groups_.at(i).value.seg);
    }

    can_reassembly_t(const can_reassembly_t&) = delete;            /**< @brief Owns segments. */
    can_reassembly_t& operator=(const can_reassembly_t&) = delete; /**< @brief Owns segments. */

    /**
     * @brief Admit group @p key: draw its one segment from @p bytes.
     *
     * A group already tracked under @p key is replaced, uncounted (its producer re-advertised
     * the run). Every refusal ticks @ref dropped_groups, since the group is lost before delivery.
     *
     * @param key       The `(origin, ts)` group identity.
     * @param count     The advertised slice count.
     * @param total     The advertised total length, in bytes.
     * @param max_width The widest slice this link carries (8 classic, 64 CAN-FD).
     * @param max_count The most slices the group can ever receive (the endpoint slots left
     *                  from its base).
     * @param bytes     The receive backend the segment is drawn from.
     * @retval false The claim is impossible (no slices, more than @p max_count, or a slice
     *               geometry that cannot carry `total`), over the link's caps, or the backend
     *               or the group table refused: nothing is tracked and nothing is drawn.
     */
    [[nodiscard]] bool open_group(const reassembly_key_t& key, std::uint32_t count,
                                  std::uint32_t total, std::size_t max_width,
                                  std::uint32_t max_count, mem::mem_backend_t& bytes) noexcept {
        if (groups_.contains(key)) drop_group(key);
        // The producer sliced at its mode's width, and (count, total) names it: a classic group
        // fits `count * 8`, an FD group of two or more slices never does. Every slice but the
        // last is `width` bytes, so `count` slices carry exactly the totals in
        // ((count-1)*width, count*width]. All in 64 bits, so nothing wraps for any u32 claim,
        // and the segment size is checked against the budget before it becomes a `size_t`.
        const std::uint64_t classic = std::uint64_t{count} * can::kCanClassicMaxData;
        const std::uint64_t width = total <= classic ? can::kCanClassicMaxData : max_width;
        const std::uint64_t full = std::uint64_t{count} * width;
        const std::uint64_t bits = (std::uint64_t{count} + 7) / 8;
        const std::uint64_t size = std::uint64_t{total} + bits;
        // `count - 1 >= max_count` refuses both 0 slices (it wraps) and a run past the window.
        if (count - 1u >= max_count || total > full || total + width <= full ||
            total > max_message_ || size > max_inflight_ - inflight_) {
            ++dropped_groups_;
            return false;
        }
        if (max_groups_ != 0 && groups_.size() >= max_groups_) evict_oldest();
        tr::view::segment_t* const seg = bytes.alloc(static_cast<std::size_t>(size));
        group_meta_t* const g =
            seg == nullptr
                ? nullptr
                : groups_
                      .try_emplace(key, group_meta_t{.seg = seg,
                                                     .total = total,
                                                     .width = static_cast<std::uint32_t>(width),
                                                     .expected = count,
                                                     .count = 0,
                                                     .highest = 0,
                                                     .seq = next_seq_++,
                                                     .last_touch = now_})
                      .value;
        if (g == nullptr) {
            release(seg);
            ++dropped_groups_;
            return false;
        }
        inflight_ += static_cast<std::size_t>(size);
        std::memset(seg->bytes.data() + total, 0, static_cast<std::size_t>(bits));
        return true;
    }

    /** @brief Segment bytes all open groups hold together (charged against `max_inflight`). */
    [[nodiscard]] std::size_t inflight_bytes() const noexcept { return inflight_; }

    /**
     * @brief Copy slice @p index of group @p key into the group's segment.
     *
     * A re-sent index overwrites the bytes it repeats and is counted once (#2049). A slice
     * longer than its place (CAN-FD DLC padding on the last slice) is cut to it.
     *
     * @param key   The `(origin, ts)` group identity.
     * @param index The zero-based slice position.
     * @param slice The slice's bytes (one CAN data field).
     * @retval false No group is admitted under @p key, @p index is past its count, or the
     *               slice is shorter than its place: nothing was written, and the caller
     *               abandons the group (@ref discard).
     */
    [[nodiscard]] bool add_slice(const reassembly_key_t& key, std::uint32_t index,
                                 std::span<const std::byte> slice) noexcept {
        group_meta_t* const g = groups_.find(key);
        if (g == nullptr || index >= g->expected) return false;
        const std::size_t off = std::size_t{index} * g->width;
        const std::size_t len = std::min<std::size_t>(g->width, g->total - off);
        if (slice.size() < len) return false;
        std::byte* const base = g->seg->bytes.data();
        std::memcpy(base + off, slice.data(), len);
        g->last_touch = now_;
        std::byte& bits = base[g->total + index / 8];
        const std::byte bit{static_cast<std::uint8_t>(1u << (index % 8))};
        if ((bits & bit) != std::byte{0}) return true;
        bits |= bit;
        ++g->count;
        g->highest = std::max(g->highest, index);
        return true;
    }

    /** @brief True when group @p key is admitted and not yet taken or dropped. */
    [[nodiscard]] bool contains(const reassembly_key_t& key) const noexcept {
        return groups_.contains(key);
    }

    /** @brief Number of distinct slices received for group @p key (0 if unknown). */
    [[nodiscard]] std::size_t slice_count(const reassembly_key_t& key) const noexcept {
        const group_meta_t* const g = groups_.find(key);
        return g == nullptr ? 0 : g->count;
    }

    /**
     * @brief True when an interior slice is missing (a hole below the highest index).
     *
     * A missing *trailing* slice is not an interior gap (ADR-0011).
     */
    [[nodiscard]] bool has_interior_gap(const reassembly_key_t& key) const noexcept {
        const group_meta_t* const g = groups_.find(key);
        return g != nullptr && g->count != 0 &&
               g->count != static_cast<std::size_t>(g->highest) + 1;
    }

    /** @brief True when every slice `0..count-1` of group @p key is present. */
    [[nodiscard]] bool is_complete(const reassembly_key_t& key) const noexcept {
        // Each index is counted once and is below the count, so reaching the count is having
        // all of them. O(1): this runs once per arriving slice (#2049).
        const group_meta_t* const g = groups_.find(key);
        return g != nullptr && g->count == g->expected;
    }

    /**
     * @brief Hand the complete group's segment out, owning, and stop tracking the group.
     * @param key The group identity.
     * @return A view of exactly the advertised total length, or `std::nullopt` unless the
     *         group @ref is_complete (it is then left as it was).
     */
    [[nodiscard]] std::optional<tr::view::view_t> take(const reassembly_key_t& key) noexcept {
        group_meta_t* const g = groups_.find(key);
        if (g == nullptr || g->count != g->expected) return std::nullopt;
        const std::size_t total = g->total;
        inflight_ -= seg_bytes(*g);
        tr::view::view_t whole =
            tr::view::view_t::over(tr::view::segment_ptr_t::adopt(std::exchange(g->seg, nullptr)));
        (void)groups_.erase(key);
        whole.length = total;  // the presence bits past the total stay behind
        return whole;
    }

    /** @brief Drop all buffered state for group @p key, uncounted. */
    void erase(const reassembly_key_t& key) noexcept { drop_group(key); }

    /**
     * @brief Abandon group @p key as one that will NEVER complete — erase it AND count it.
     *
     * The counted twin of @ref erase. The ingress path refused a slice, so the group is dead
     * and its segment is reclaimed BEFORE delivery. That is exactly what @ref dropped_groups
     * counts, whatever forced it: a refused admission, a `max_groups` eviction, a @ref
     * sweep_stale age-out, or this. Without it a caller either delivers a byte-wrong short
     * frame or loses a whole group with no counter moving (#911).
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
     * A group is "touched" when it is admitted or a slice is added, so one that stopped
     * making progress — a lost data slice — ages out here. Without this, such a group is
     * never @ref is_complete, so it is never taken and its segment is pinned for the
     * process's life (#912). Each erased group ticks @ref dropped_groups, exactly as a
     * `max_groups` eviction does.
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
     * @brief Count of groups lost before delivery — a refused admission, a `max_groups`
     *        eviction, a @ref sweep_stale age-out or a @ref discard (never an OOM).
     */
    [[nodiscard]] std::uint64_t dropped_groups() const noexcept { return dropped_groups_; }

   private:
    struct group_meta_t {
        tr::view::segment_t* seg = nullptr;  // owned: total bytes, then ceil(expected / 8) bits.
        std::uint32_t total = 0;             // advertised total length.
        std::uint32_t width = 0;             // advertised slice width.
        std::uint32_t expected = 0;          // advertised slice count.
        std::uint32_t count = 0;             // distinct slices received.
        std::uint32_t highest = 0;           // highest slice index received.
        std::uint64_t seq = 0;               // admission order, for evict-oldest.
        std::uint64_t last_touch = 0;        // caller's stamp at the last progress.
    };

    // The segment's bytes, presence bits included: what admission charged.
    [[nodiscard]] static std::size_t seg_bytes(const group_meta_t& g) noexcept {
        return std::size_t{g.total} + (std::size_t{g.expected} + 7) / 8;
    }
    // A group's segment is held raw and never shared until `take`, so it goes straight back to
    // the backend it came from: the injected seam, not the process heap (ADR-0083).
    static void release(tr::view::segment_t* seg) noexcept {
        if (seg != nullptr) seg->backend->destroy(seg);
    }
    void drop_group(const reassembly_key_t& key) noexcept {
        group_meta_t* const g = groups_.find(key);
        if (g == nullptr) return;
        inflight_ -= seg_bytes(*g);
        release(g->seg);
        (void)groups_.erase(key);
    }

    // Evict the oldest-admitted group to make room (bounded drop, counted).
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
    std::size_t max_message_ = 0;   // per-group cap on the claimed total.
    std::size_t max_inflight_ = 0;  // per-link cap on segment bytes held by open groups.
    std::size_t inflight_ = 0;      // segment bytes held by open groups now.
    std::uint64_t next_seq_ = 0;
    std::uint64_t dropped_groups_ = 0;
    std::uint64_t now_ = 0;  // the caller's latest stamp (set_now); no clock here.
    mem::sorted_map_t<reassembly_key_t, group_meta_t> groups_;
};

}  // namespace tr::net
