/**
 * @file
 * @brief The forwarder's bounded table of forwarded requests awaiting a reply (RFC 0028 §4.7,
 *        #1625).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A hop that forwards a request toward another session used to keep nothing about it: the
 * reply retraced the accumulated `src` on its own, and a far end that never answered left the
 * requester with no reply and no error at all. A requester that sends one request at a time
 * then had its next request queued behind the silent one until its own timeout tore the
 * session down — one stale peer stalling an unrelated workflow.
 *
 * This table is what the hop now keeps: one fixed-size entry per forwarded request that asked
 * for a reply, holding the two routes an addressed error reply needs. The reply that comes
 * back through the hop settles the entry. An entry still open past its deadline, or whose far
 * end went away, is answered by the hop itself with an addressed error, so the requester
 * always gets exactly one answer in bounded time. A full table refuses the new forward at once
 * rather than growing, so the RAM cost is fixed at build time and visible.
 *
 * An entry the hop answered itself does not free its slot at once. It stays behind as a
 * TOMBSTONE for one more `kForwardDeadline`, so the far end's late reply finds it and is
 * dropped rather than forwarded. Without that, the late reply would match the requester's
 * NEXT request on the same route (same link, same `src`) and settle it — a second answer to
 * the first request, handed to the requester as the answer to the second, and the second left
 * with no bound. On a chain of hops the same tombstone swallows the inner hop's own timeout,
 * which arrives after the outer hop has already answered.
 *
 * The table is the data structure only; which frames it sees, and how an expired entry is
 * turned into a frame, is the router's business (`fwd_router.cpp`).
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include "libtracer/config.hpp"

namespace tr::net {

/**
 * @brief One snapshot of a forwarder's pending-reply table (#1625).
 *
 * Vocabulary and polarity per `core/STYLE.md` §Introspection; the snapshot-coherence clause
 * stated there applies.
 */
struct forward_stats_t {
    std::size_t capacity = 0; /**< @brief Entries the table was built with. */
    std::size_t in_use = 0;   /**< @brief Forwarded requests still awaiting their reply. */
    std::size_t peak = 0;     /**< @brief High-water mark of @ref in_use. */
    /** @brief Forwards refused because the table was full — answered to the requester with
     *         `BACKPRESSURE` instead of being forwarded. */
    std::size_t refused = 0;
    /** @brief Entries answered by the hop itself with an error: the deadline passed, or the
     *         far end went away first. */
    std::size_t expired = 0;
    /** @brief Late replies dropped: the reply matched a tombstone, so the hop had already
     *         answered that request itself and forwarding it would be a second answer. */
    std::size_t late = 0;
    /** @brief Forwards that asked for a reply but could not be tracked, because their two
     *         routes together exceed `kForwardRouteBytes`. They are forwarded unbounded, as
     *         before; this counts how often, so the route budget can be sized against it. */
    std::size_t untracked = 0;
};

/**
 * @brief The fixed-size table of forwarded requests awaiting a reply.
 *
 * Entries are keyed by the link the request arrived on (an opaque pointer the router owns)
 * and a hash of the request's `src` as it arrived — which is byte for byte the remaining
 * `dst` of the reply that retraces it through this hop, so a reply finds its entry without
 * any correlation field on the wire. Two open requests with the same key are settled in
 * admission order.
 *
 * @note Thread-safe: every operation takes the internal mutex, which is never held across a
 *       send. The router copies an entry out (@ref take_expired, @ref take_via) and emits
 *       the error reply after the lock is released.
 */
class fwd_pending_t {
   public:
    /** @brief The clock deadlines are measured on. */
    using clock_t = std::chrono::steady_clock;
    /** @brief Entries in the table. */
    static constexpr std::size_t kSlots = graph::config_t::kForwardPendingSlots;
    /** @brief Route bytes one entry can hold: the request's `src` body plus its `dst` body. */
    static constexpr std::size_t kRouteBytes = graph::config_t::kForwardRouteBytes;

    /** @brief One open forward: who asked, who was asked, and the two routes. */
    struct entry_t {
        const void* requester = nullptr; /**< @brief The link the request arrived on — the
                                                     key a reply is matched by. */
        const void* reply_to = nullptr;  /**< @brief Where the hop's own error reply goes
                                                     (the router's context for @ref requester). */
        const void* responder = nullptr; /**< @brief The link it was forwarded over. */
        std::uint32_t key = 0;           /**< @brief Hash of the request's `src` body. */
        std::uint16_t src_len = 0;       /**< @brief `src` body bytes at the front of @ref route. */
        std::uint16_t dst_len = 0;       /**< @brief `dst` body bytes right after them. */
        clock_t::time_point deadline{};  /**< @brief When the hop answers it itself; also
                                                     the admission order replies settle in. */
        clock_t::time_point tomb_until{};           /**< @brief When a tombstone frees its slot. */
        std::array<std::byte, kRouteBytes> route{}; /**< @brief `src` body, then `dst` body. */
        bool used = false; /**< @brief Whether this slot is taken (open or tombstone). */
        bool tomb = false; /**< @brief Answered by the hop; waiting only to drop a late reply. */

        /** @brief The request's `src` body — the error reply's `dst`. */
        [[nodiscard]] std::span<const std::byte> src() const noexcept {
            return {route.data(), src_len};
        }
        /** @brief The request's `dst` body — the error reply's `src`. */
        [[nodiscard]] std::span<const std::byte> dst() const noexcept {
            return {route.data() + src_len, dst_len};
        }
    };

    /** @brief What @ref track decided. */
    enum class track_t : std::uint8_t {
        TRACKED,   /**< @brief An entry was opened; the forward proceeds. */
        FULL,      /**< @brief Every slot is taken (open or tombstone): refuse the forward. */
        UNTRACKED, /**< @brief The routes do not fit an entry: forward, unbounded. */
    };

    /**
     * @brief Open an entry for a forwarded request.
     *
     * @p fill writes the `src` body then the `dst` body into the span it is handed, which is
     * exactly `src_len + dst_len` bytes.
     *
     * @tparam Fill Callable `void(std::span<std::byte>)`.
     */
    template <class Fill>
    [[nodiscard]] track_t track(const void* requester, const void* reply_to, const void* responder,
                                std::uint32_t key, std::size_t src_len, std::size_t dst_len,
                                clock_t::time_point deadline, Fill&& fill) {
        if (src_len + dst_len > kRouteBytes) {
            untracked_.fetch_add(1, std::memory_order_relaxed);
            return track_t::UNTRACKED;
        }
        const std::lock_guard lock(m_);
        for (entry_t& e : slots_) {
            if (e.used) continue;
            e.used = true;
            e.tomb = false;
            e.requester = requester;
            e.reply_to = reply_to;
            e.responder = responder;
            e.key = key;
            e.src_len = static_cast<std::uint16_t>(src_len);
            e.dst_len = static_cast<std::uint16_t>(dst_len);
            e.deadline = deadline;
            fill(std::span<std::byte>(e.route.data(), src_len + dst_len));
            ++in_use_;
            if (in_use_ > peak_) peak_ = in_use_;
            publish_hint();
            return track_t::TRACKED;
        }
        ++refused_;
        return track_t::FULL;
    }

    /** @brief What @ref settle found for a reply. */
    enum class settle_t : std::uint8_t {
        NONE,    /**< @brief No slot matches: forward the reply, nothing to bookkeep. */
        SETTLED, /**< @brief An open entry was closed: forward the reply. */
        LATE,    /**< @brief The oldest match is a tombstone: the hop already answered that
                              request, so DROP the reply. */
    };

    /**
     * @brief Match a reply against the oldest slot it answers, open or tombstone.
     *
     * Replies on one route come back in request order, so the reply belongs to the OLDEST
     * matching slot. If that slot is a tombstone the reply is late: the tombstone is consumed
     * and the caller drops the reply. Otherwise the open entry is closed.
     *
     * @param requester The link the reply is being forwarded onto (the request's arrival).
     * @param key       Hash of the reply's remaining `dst` body.
     * @param same      Callable `bool(std::span<const std::byte> src)`: whether a candidate
     *                  slot's stored `src` equals the reply's remaining `dst` byte for byte.
     */
    template <class Same>
    settle_t settle(const void* requester, std::uint32_t key, Same&& same) {
        if (hint_.load(std::memory_order_relaxed) == 0) return settle_t::NONE;
        const std::lock_guard lock(m_);
        entry_t* oldest = nullptr;
        for (entry_t& e : slots_) {
            if (!e.used || e.requester != requester || e.key != key || !same(e.src())) continue;
            if (oldest == nullptr || e.deadline < oldest->deadline) oldest = &e;
        }
        if (oldest == nullptr) return settle_t::NONE;
        if (oldest->tomb) {
            free_tomb(*oldest);
            ++late_;
            return settle_t::LATE;
        }
        release(*oldest);
        return settle_t::SETTLED;
    }

    /**
     * @brief Whether any slot is taken (open or tombstone) — a lock-free peek, so a hop with
     *        nothing outstanding never reads the clock or takes the lock.
     */
    [[nodiscard]] bool any_open() const noexcept {
        return hint_.load(std::memory_order_relaxed) != 0;
    }

    /**
     * @brief Copy out ONE open entry whose deadline has passed at @p now, and turn it into a
     *        tombstone until `now + tomb_for`. Tombstones already past their own time are
     *        freed on the way.
     * @retval false No open entry has expired.
     */
    bool take_expired(clock_t::time_point now, clock_t::duration tomb_for, entry_t& out) {
        const std::lock_guard lock(m_);
        for (entry_t& e : slots_) {
            if (!e.used) continue;
            if (e.tomb) {
                if (e.tomb_until <= now) free_tomb(e);
                continue;
            }
            if (e.deadline > now) continue;
            out = e;
            e.tomb = true;
            e.tomb_until = now + tomb_for;
            --in_use_;
            ++tombs_;
            ++expired_;
            publish_hint();
            return true;
        }
        return false;
    }

    /**
     * @brief Close and copy out ONE entry that was forwarded over @p responder — its far end
     *        went away, so it is answered now rather than at its deadline.
     * @retval false No open entry names @p responder.
     */
    bool take_via(const void* responder, entry_t& out) {
        if (responder == nullptr) return false;
        const std::lock_guard lock(m_);
        for (entry_t& e : slots_) {
            if (!e.used || e.responder != responder) continue;
            if (e.tomb) {  // nobody is left to send a late reply over the departed link
                free_tomb(e);
                continue;
            }
            out = e;
            release(e);
            ++expired_;
            return true;
        }
        return false;
    }

    /** @brief Close, without answering, every slot (open or tombstone) whose requester is
     *         @p requester — the link that asked is gone, so there is nobody to answer. */
    void forget_requester(const void* requester) {
        if (requester == nullptr) return;
        const std::lock_guard lock(m_);
        for (entry_t& e : slots_) {
            if (!e.used || e.requester != requester) continue;
            if (e.tomb)
                free_tomb(e);
            else
                release(e);
        }
    }

    /** @brief One snapshot of the table's counters (see @ref forward_stats_t). */
    [[nodiscard]] forward_stats_t stats() const {
        const std::lock_guard lock(m_);
        return {kSlots,
                in_use_,
                peak_,
                refused_,
                expired_,
                late_,
                untracked_.load(std::memory_order_relaxed)};
    }

   private:
    /** @brief Close the open entry @p e. Caller holds `m_`. */
    void release(entry_t& e) noexcept {
        e.used = false;
        --in_use_;
        publish_hint();
    }

    /** @brief Free the tombstone @p e. Caller holds `m_`. */
    void free_tomb(entry_t& e) noexcept {
        e.used = false;
        e.tomb = false;
        --tombs_;
        publish_hint();
    }

    /** @brief Mirror the taken-slot count into `hint_`. Caller holds `m_`. */
    void publish_hint() noexcept { hint_.store(in_use_ + tombs_, std::memory_order_relaxed); }

    mutable std::mutex m_;                /**< @brief Guards the table; never held across a
                                                      send. */
    std::array<entry_t, kSlots> slots_{}; /**< @brief The entries. */
    std::size_t in_use_ = 0;              /**< @brief Open entries. */
    std::size_t tombs_ = 0;               /**< @brief Tombstones. */
    std::size_t late_ = 0;                /**< @brief Late replies dropped on a tombstone. */
    std::size_t peak_ = 0;                /**< @brief High-water mark of `in_use_`. */
    std::size_t refused_ = 0;             /**< @brief Forwards refused on a full table. */
    std::size_t expired_ = 0;             /**< @brief Entries the hop answered itself. */
    /** @brief `in_use_ + tombs_`, readable without the lock, so an empty table costs a
     *         forwarded reply one relaxed load and no lock. */
    std::atomic<std::size_t> hint_{0};
    /** @brief Forwards too long to track; bumped without the lock. */
    std::atomic<std::size_t> untracked_{0};
};

}  // namespace tr::net
