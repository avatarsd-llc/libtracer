/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * length_prefix_framer — reassembles u32-LE length-prefixed frames from a stream
 * delivered as arbitrary chunks (a msquic RECEIVE event's buffers). Extracted
 * from the byte-for-byte identical RX state machine that transport_quic and
 * transport_webtransport each open-coded (the review's finding #4 "verbatim"
 * duplication): each complete frame is reassembled into ONE exactly-sized
 * refcounted segment drawn from the caller's backend (ADR-0042 §2/§4 — no library
 * buffer, one copy off the wire); an allocation failure is BACKPRESSURE (the
 * frame is drained so framing sync survives, and counted), and an oversize length
 * prefix is malformed (a desynced stream cannot be re-framed, so the caller tears
 * the connection down).
 *
 * The state machine is transport-agnostic — no msquic types, no atomics, no
 * connection handle — so it is unit-tested directly (length_prefix_framer_test)
 * in the default build, without a live QUIC connection. The transport keeps its
 * own counters and shutdown, driven by the per-chunk `result_t`.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "libtracer/backend.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/segment.hpp"

/**
 * @file
 * @brief `tr::net` u32-length-prefix stream reassembler: `length_prefix_framer`.
 */

namespace tr::net {

/**
 * @brief Reassembles u32-LE length-prefixed frames from an arbitrarily-chunked stream.
 *
 * Fed one chunk at a time via @ref feed; each completed frame is delivered through
 * the caller's `on_frame` callback as a fresh, exactly-sized segment. Holds only
 * partial-reassembly state (a prefix scratch + the in-flight segment), so one
 * framer serves one stream and is reused across a stream's chunks; @ref reset
 * discards partial state when a new peer's stream takes over.
 */
class length_prefix_framer {
   public:
    /** @brief The u32-LE length prefix's size on the wire (transport framing). */
    static constexpr std::size_t kPrefixBytes = 4;

    /**
     * @brief What one length prefix means under the shared framing rules.
     *
     * The single home for the per-prefix decisions every u32-length-prefixed
     * stream applies identically: `len == 0` is a no-op record, `len` beyond the
     * @ref effective_cap is malformed (a desynced stream cannot be re-framed),
     * and an allocation failure is backpressure (drain the body, count a drop).
     * @ref feed applies them to a chunk-fed stream; a pull-mode blocking reader
     * (`tcp_transport_t`) applies the same rules but reads the body straight off
     * the socket into the accepted segment — adopting @ref feed there would add
     * a scratch-buffer copy on the hot path (ADR-0042 §2/§4), so the *rules* are
     * shared instead of the state machine.
     */
    struct prefix_decision_t {
        /** @brief The decision kind. */
        enum class kind_t : std::uint8_t {
            EMPTY,     /**< @brief `len == 0` — the record carries no TLV; skip it. */
            MALFORMED, /**< @brief `len` exceeds the effective cap — tear the stream down. */
            DROP,      /**< @brief Backend `alloc` failed — drain the body bytes, count a drop. */
            ACCEPT,    /**< @brief @ref seg owns exactly `len` bytes, ready to fill. */
        };
        kind_t kind = kind_t::EMPTY; /**< @brief The decision. */
        tr::view::segment_ptr_t seg; /**< @brief The accepted frame's segment (ACCEPT only). */
    };

    /**
     * @brief The effective RX frame cap: `min(max_frame, backend.max_segment_size())`.
     *
     * A prefix claiming more than the backend could ever allocate (e.g. a bounded
     * pool's slot) is undeliverable, so it is rejected up front and never drained —
     * the bound is the injected resource's real capacity, not a magic number (the
     * no-synthetic-limits doctrine).
     */
    [[nodiscard]] static std::size_t effective_cap(const mem::mem_backend_t& backend,
                                                   std::size_t max_frame) noexcept {
        const std::size_t backend_cap = backend.max_segment_size();
        return max_frame < backend_cap ? max_frame : backend_cap;
    }

    /**
     * @brief Apply the shared framing rules to one decoded u32 length prefix.
     *
     * @param backend Where an accepted frame's segment is allocated (ADR-0042 §2/§4).
     * @param cap     The effective cap from @ref effective_cap.
     * @param len     The decoded length prefix.
     * @return The @ref prefix_decision_t; `ACCEPT` carries the freshly allocated segment.
     */
    [[nodiscard]] static prefix_decision_t on_prefix(mem::mem_backend_t& backend, std::size_t cap,
                                                     std::size_t len) {
        if (len == 0) return {prefix_decision_t::kind_t::EMPTY, {}};
        if (len > cap) return {prefix_decision_t::kind_t::MALFORMED, {}};
        auto seg = tr::view::segment_ptr_t::adopt(backend.alloc(len));
        if (!seg) return {prefix_decision_t::kind_t::DROP, {}};
        return {prefix_decision_t::kind_t::ACCEPT, std::move(seg)};
    }

    /**
     * @brief The outcome of feeding one chunk (the transport applies the effects).
     *
     * Deliveries happen inline through `on_frame`; this reports only what the
     * transport must act on with its own counters / connection handle.
     */
    struct result_t {
        std::size_t dropped =
            0; /**< @brief Frames skipped to backpressure (backend `alloc` failed). */
        bool malformed =
            false; /**< @brief An oversize length prefix was seen — stop, shut the peer down. */
    };

    /**
     * @brief Feed @p n bytes at @p p; deliver each completed frame via @p on_frame.
     *
     * @tparam OnFrame  Callable `void(tr::view::segment_ptr_t seg, std::size_t len)`
     *                  — `seg` owns exactly @p len bytes of one reassembled frame.
     * @param backend   Where each frame's segment is allocated (ADR-0042 §2/§4).
     * @param max_frame The caller's frame ceiling. The **effective** cap is
     *                  `min(max_frame, backend.max_segment_size())` — a prefix
     *                  claiming more than the backend could ever allocate (e.g. a
     *                  bounded pool's slot) is rejected up front, so no undeliverable
     *                  frame is drained (the no-synthetic-limits doctrine: the bound
     *                  is the injected resource's real capacity, not a magic number).
     * @param p,n       The chunk (may split a prefix or a body arbitrarily).
     * @param on_frame  Invoked once per completed frame, in arrival order.
     * @return Per-chunk `result_t`: frames dropped to backpressure, and whether
     *         an oversize prefix stopped the feed (the caller shuts the peer down).
     */
    template <class OnFrame>
    result_t feed(mem::mem_backend_t& backend, std::size_t max_frame, const std::byte* p,
                  std::size_t n, OnFrame&& on_frame) {
        const std::size_t cap = effective_cap(backend, max_frame);
        result_t res;
        while (n > 0) {
            if (drain_left_ > 0) {
                // Backpressure discard: skip the dropped frame's bytes so the next
                // prefix lines up (framing sync survives exhaustion).
                const std::size_t take = drain_left_ < n ? drain_left_ : n;
                drain_left_ -= take;
                p += take;
                n -= take;
                if (drain_left_ == 0) prefix_have_ = 0;
                continue;
            }
            if (prefix_have_ < kPrefixBytes) {
                // Reassemble the 4-byte length prefix across chunk boundaries.
                const std::size_t want = kPrefixBytes - prefix_have_;
                const std::size_t take = want < n ? want : n;
                std::memcpy(prefix_.data() + prefix_have_, p, take);
                prefix_have_ += take;
                p += take;
                n -= take;
                if (prefix_have_ < kPrefixBytes) return res;  // await the rest of the prefix
                const std::size_t len = tr::detail::load_le<std::uint32_t>(prefix_);
                prefix_decision_t dec = on_prefix(backend, cap, len);
                switch (dec.kind) {
                    case prefix_decision_t::kind_t::EMPTY:
                        // An empty record carries no TLV — a no-op.
                        prefix_have_ = 0;
                        continue;
                    case prefix_decision_t::kind_t::MALFORMED:
                        // Malformed (corrupt/hostile) or undeliverable (exceeds the
                        // backend's capacity): a desynced stream cannot be re-framed —
                        // stop and let the caller shut the peer down.
                        res.malformed = true;
                        return res;
                    case prefix_decision_t::kind_t::DROP:
                        ++res.dropped;
                        drain_left_ = len;
                        continue;
                    case prefix_decision_t::kind_t::ACCEPT:
                        rx_seg_ = std::move(dec.seg);
                        rx_len_ = len;
                        rx_off_ = 0;
                        continue;
                }
                continue;  // unreachable — the switch above covers every kind
            }
            // Fill the frame body from this chunk (the single unavoidable copy off
            // the wire into the owned segment).
            const std::size_t want = rx_len_ - rx_off_;
            const std::size_t take = want < n ? want : n;
            std::memcpy(rx_seg_->bytes.data() + rx_off_, p, take);
            rx_off_ += take;
            p += take;
            n -= take;
            if (rx_off_ == rx_len_) {
                on_frame(std::move(rx_seg_), rx_len_);
                rx_seg_ = {};
                prefix_have_ = 0;
                rx_len_ = rx_off_ = 0;
            }
        }
        return res;
    }

    /** @brief Discard partial reassembly state (a new peer's stream reuses the framer). */
    void reset() noexcept {
        prefix_have_ = 0;
        rx_seg_ = {};
        rx_len_ = rx_off_ = drain_left_ = 0;
    }

   private:
    std::array<std::byte, kPrefixBytes> prefix_{};  // prefix scratch, filled across chunks
    std::size_t prefix_have_ = 0;                   // prefix bytes accumulated so far
    tr::view::segment_ptr_t rx_seg_;                // the in-flight frame's segment
    std::size_t rx_len_ = 0;                        // its total length
    std::size_t rx_off_ = 0;                        // body bytes filled so far
    std::size_t drain_left_ = 0;  // backpressure: dropped-frame bytes left to skip
};

}  // namespace tr::net
