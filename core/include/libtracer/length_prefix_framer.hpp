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
 * buffer, one copy off the wire).
 *
 * Two failures, two answers (#932): a frame this node cannot STORE right now —
 * `alloc` failed, or the injected backend's slots are permanently smaller than the
 * frame — is BACKPRESSURE (the body is drained so framing sync survives, and
 * counted); only a length beyond the agreed PROTOCOL cap (`max_frame`, itself
 * tighten-only against @ref kDefaultMaxFrame) is malformed, because that is the
 * one case where the stream has lost framing sync and cannot be re-framed, so the
 * caller tears the connection down. A well-behaved peer whose frame merely
 * exceeds OUR local segment size is shed, never disconnected.
 *
 * The state machine is transport-agnostic — no msquic types, no atomics, no
 * connection handle — so it is unit-tested directly (length_prefix_framer_test)
 * in the default build, without a live QUIC connection. The transport keeps its
 * own counters and shutdown: a drop is reported the moment it is decided (the
 * `on_drop` callback), and the per-chunk `result_t` carries only what STOPS the
 * feed. Counting drops per chunk instead was #1255: one chunk routinely carries
 * a dropped frame and a delivered one, so a batched count published after the
 * feed returned became visible to a receiver only AFTER the delivery that
 * followed the drop — an observer woken by that frame could read a stale count.
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
     * @brief The default receive frame cap when `:settings max_frame` is unset
     *        (16 MiB) — one home for the default every u32-length-prefixed
     *        stream transport (tcp / quic / webtransport) restates as its
     *        `kMaxFrame`.
     *
     * A prefix announcing more is malformed (corrupt or hostile): the stream
     * has lost framing sync and the connection is torn down. A per-connection
     * `:settings max_frame` may TIGHTEN the cap below this default, never
     * raise it; the effective cap is further bounded by the injected backend's
     * real capacity (@ref effective_cap — the no-synthetic-limits doctrine).
     */
    static constexpr std::size_t kDefaultMaxFrame = 16u * 1024u * 1024u;

    /**
     * @brief What one length prefix means under the shared framing rules.
     *
     * The single home for the per-prefix decisions every u32-length-prefixed
     * stream applies identically: `len == 0` is a no-op record, `len` beyond the
     * protocol cap is malformed (a desynced stream cannot be re-framed), and a
     * frame the backend cannot hold — `alloc` failed, including because the frame
     * is larger than any segment this backend can ever produce — is backpressure
     * (drain the body, count a drop).
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
            MALFORMED, /**< @brief `len` exceeds the PROTOCOL cap — tear the stream down. */
            DROP,   /**< @brief The backend could not hold the frame (`alloc` failed, or the frame
                          exceeds this backend's segment size) — drain the body, count a drop. */
            ACCEPT, /**< @brief @ref seg owns exactly `len` bytes, ready to fill. */
        };
        kind_t kind = kind_t::EMPTY; /**< @brief The decision. */
        tr::view::segment_ptr_t seg; /**< @brief The accepted frame's segment (ACCEPT only). */
    };

    /**
     * @brief Resolve a `:settings max_frame` value into the per-connection cap —
     *        TIGHTEN-ONLY against @ref kDefaultMaxFrame.
     *
     * `0` (unset) keeps the default; a nonzero value yields
     * `min(max_frame, kDefaultMaxFrame)`. The setting arrives through a
     * config-writable key (a connection SPEC's `:settings`), so it may only
     * narrow what the node will buffer off the wire — never widen it above the
     * protocol default (#1035). Every framed transport (tcp / ws / quic /
     * webtransport) assigns its per-connection cap through this one home.
     */
    [[nodiscard]] static constexpr std::size_t configured_cap(std::size_t max_frame) noexcept {
        if (max_frame == 0) return kDefaultMaxFrame;
        return max_frame < kDefaultMaxFrame ? max_frame : kDefaultMaxFrame;
    }

    /**
     * @brief The effective RX frame cap: `min(max_frame, backend.max_segment_size())` —
     *        the largest frame this connection can actually DELIVER.
     *
     * The bound is the injected resources' real capacity, not a magic number (the
     * no-synthetic-limits doctrine). It is a *deliverability* bound, not the
     * protocol's framing bound: a length above it but within `max_frame` is shed as
     * backpressure, not treated as malformed (#932) — see @ref on_prefix. Transports
     * that refuse an oversize frame from a DECLARED length before buffering a body
     * byte (the WS frame header) compare against this.
     */
    [[nodiscard]] static std::size_t effective_cap(const mem::mem_backend_t& backend,
                                                   std::size_t max_frame) noexcept {
        const std::size_t backend_cap = backend.max_segment_size();
        return max_frame < backend_cap ? max_frame : backend_cap;
    }

    /**
     * @brief Apply the shared framing rules to one decoded u32 length prefix.
     *
     * @param backend   Where an accepted frame's segment is allocated (ADR-0042 §2/§4).
     * @param max_frame The PROTOCOL cap (from @ref configured_cap) — the only bound whose
     *                  violation means the stream is desynced. A length within it that the
     *                  backend cannot satisfy (exhausted, or simply larger than any segment
     *                  this backend produces) comes back as `DROP`, so a legitimate peer is
     *                  backpressured rather than disconnected over OUR local capacity (#932).
     * @param len       The decoded length prefix.
     * @return The @ref prefix_decision_t; `ACCEPT` carries the freshly allocated segment.
     */
    [[nodiscard]] static prefix_decision_t on_prefix(mem::mem_backend_t& backend,
                                                     std::size_t max_frame, std::size_t len) {
        if (len == 0) return {prefix_decision_t::kind_t::EMPTY, {}};
        if (len > max_frame) return {prefix_decision_t::kind_t::MALFORMED, {}};
        // Undeliverable-but-in-spec is backpressure, not a framing error: ask the
        // backend, and let a refusal (exhaustion OR a too-small slot) be the DROP.
        if (len > backend.max_segment_size()) return {prefix_decision_t::kind_t::DROP, {}};
        auto seg = tr::view::segment_ptr_t::adopt(backend.alloc(len));
        if (!seg) return {prefix_decision_t::kind_t::DROP, {}};
        return {prefix_decision_t::kind_t::ACCEPT, std::move(seg)};
    }

    /**
     * @brief The outcome of feeding one chunk (the transport applies the effects).
     *
     * Deliveries happen inline through `on_frame` and drops inline through
     * `on_drop`; this reports only the one outcome that STOPS the feed, which the
     * caller must act on with its connection handle. It deliberately carries no
     * drop COUNT: a per-chunk tally can only be read after the whole chunk has
     * been processed, i.e. after deliveries the drop preceded (#1255).
     */
    struct result_t {
        bool malformed = false; /**< @brief A prefix beyond the PROTOCOL cap was seen — stop,
                                      shut the peer down. */
    };

    /**
     * @brief Feed @p n bytes at @p p; deliver each completed frame via @p on_frame and
     *        report each backpressure drop via @p on_drop, both in arrival order.
     *
     * The ORDERING is the contract, not an implementation detail (#1255): `on_drop`
     * runs at the moment the drop is decided, so it always precedes the `on_frame`
     * of any frame that arrives after the dropped one — including when both share a
     * single chunk, which is the common case for small frames. A caller whose
     * `on_drop` bumps a counter therefore gives every receiver this guarantee: by
     * the time a delivered frame is observable, every drop before it is already
     * counted. A relaxed atomic is enough to carry it — the increment is
     * sequenced-before the delivery, so whatever release/acquire edge publishes the
     * frame to an observer (a sink's mutex, a queue) carries the count with it.
     *
     * @tparam OnFrame  Callable `void(tr::view::segment_ptr_t seg, std::size_t len)`
     *                  — `seg` owns exactly @p len bytes of one reassembled frame.
     * @tparam OnDrop   Callable `void()` — one backpressure drop was just decided.
     * @param backend   Where each frame's segment is allocated (ADR-0042 §2/§4).
     * @param max_frame The caller's PROTOCOL frame ceiling (`configured_cap`): a prefix
     *                  beyond it is malformed and stops the feed. A frame within it that
     *                  the backend cannot hold — exhausted, or larger than any segment it
     *                  produces (`effective_cap`) — is drained and reported through
     *                  @p on_drop instead, so local capacity backpressures a
     *                  legitimate peer rather than disconnecting it (#932).
     * @param p,n       The chunk (may split a prefix or a body arbitrarily).
     * @param on_frame  Invoked once per completed frame, in arrival order.
     * @param on_drop   Invoked once per frame shed to backpressure, at decision time.
     * @return Per-chunk `result_t`: whether an oversize prefix stopped the feed (the
     *         caller shuts the peer down).
     */
    template <class OnFrame, class OnDrop>
    result_t feed(mem::mem_backend_t& backend, std::size_t max_frame, const std::byte* p,
                  std::size_t n, OnFrame&& on_frame, OnDrop&& on_drop) {
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
                prefix_decision_t dec = on_prefix(backend, max_frame, len);
                switch (dec.kind) {
                    case prefix_decision_t::kind_t::EMPTY:
                        // An empty record carries no TLV — a no-op.
                        prefix_have_ = 0;
                        continue;
                    case prefix_decision_t::kind_t::MALFORMED:
                        // Beyond the protocol cap (corrupt/hostile): a desynced stream
                        // cannot be re-framed — stop and let the caller shut the peer
                        // down. A merely undeliverable frame lands in DROP instead.
                        res.malformed = true;
                        return res;
                    case prefix_decision_t::kind_t::DROP:
                        // Report the drop HERE, before a single body byte is drained and
                        // therefore before any later frame in this same chunk can be
                        // delivered. Counting it into the returned result instead put the
                        // publication after those deliveries (#1255).
                        on_drop();
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
