/**
 * @file
 * @brief The fake link adapter (#2139): a `transport_t` with no socket, whose wire is two
 *        byte buffers a test drives directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * It is shaped as ADR-0087 §1–§2 says every link will be, so the case suite of
 * `link_seam_suite.hpp` has one link that already passes every case deterministically and
 * the frame-ingress and egress modules have a socket-free place to be driven from:
 *
 * - **Ingress.** Bytes arrive in whatever pieces the test writes. The header is read first
 *   and judged by the shared rules (`length_prefix_framer_t::on_prefix`): an announced
 *   length above `max_frame` closes the link before anything is drawn; otherwise the body is
 *   drawn at its exact length from the link's own receive source and filled in place. An
 *   unfragmented frame goes up as one view over that segment; fragments chain into a rope
 *   with no copy. The fragments of one message together count against `max_frame`.
 * - **Egress.** Every send goes through the shared-frame egress module (ADR-0087 §2,
 *   `egress.hpp`), the one a socket link uses: a record the peer's window takes is appended
 *   whole and never waits; one that does not fit (the peer stopped reading) waits on the
 *   wire, and the records behind it wait in their vertex's lane at its depth, a retained
 *   send keeping its value by reference. A plain send rides the control lane. What a lane
 *   cannot keep, and any send once the peer has gone, is shed and counted in `dropped_tx`.
 *   The peer reading (@ref fake_link_t::wire_out) is the writability event that resumes the
 *   waiting records.
 *
 * The wire format is the fake's own: a u32 little-endian header whose low 31 bits are the
 * body length and whose top bit says another fragment of the same message follows. Only the
 * fake and its rig know it; the suite never does.
 */

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/egress.hpp"
#include "libtracer/length_prefix_framer.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/value.hpp"
#include "link_seam_suite.hpp"

namespace tr::testing::link_seam {

/** @brief A socket-free link whose wire a test writes and reads directly. */
class fake_link_t final : public net::transport_t {
   public:
    /** @brief The header's size on the fake wire. */
    static constexpr std::size_t kHeaderBytes = 4;
    /** @brief The header bit that says another fragment of this message follows. */
    static constexpr std::uint32_t kMore = 0x8000'0000u;

    /**
     * @brief A link that draws inbound frames from @p rx and refuses any message longer than
     *        @p max_frame; the peer's receive window holds @p peer_window bytes.
     */
    fake_link_t(mem::mem_backend_t& rx, std::size_t max_frame, std::size_t peer_window) noexcept
        : rx_backend_(&rx), max_frame_(max_frame), peer_window_(peer_window) {
        egress_.open(peer_);
    }
    /** @brief Destroyed as the class it is; every waiting record goes back to its source. */
    ~fake_link_t() { egress_.close(peer_); }
    fake_link_t(const fake_link_t&) = delete;
    fake_link_t& operator=(const fake_link_t&) = delete;

    /** @brief One record on the control lane. */
    void send(std::span<const std::byte> frame) override {
        const std::span<const std::byte> one[1] = {frame};
        send_record(one, nullptr, {});
    }
    /** @brief One gathered record on the control lane. */
    void send(std::span<const std::span<const std::byte>> iov) override {
        send_record(iov, nullptr, {});
    }
    /** @brief One record in @p lane; a record that waits keeps @p value by reference. */
    void send(std::span<const std::span<const std::byte>> head, const graph::value_t& value,
              const graph::delivery_lane_t& lane) override {
        send_record(head, &value, lane);
    }

    /** @brief Owning delivery: an unfragmented frame is a single-link rope. */
    [[nodiscard]] bool delivers_ropes() const override { return true; }
    /** @brief Up until the peer leaves or a malformed header closes the link. */
    [[nodiscard]] bool link_up() const noexcept override {
        return up_.load(std::memory_order_acquire);
    }
    /** @brief The three counters. */
    [[nodiscard]] net::transport_drop_stats_t drop_stats() const noexcept override {
        return {dropped_rx_.load(std::memory_order_relaxed),
                malformed_rx_.load(std::memory_order_relaxed),
                dropped_tx_.load(std::memory_order_relaxed)};
    }

    /**
     * @brief The peer's bytes arrive: frame them and deliver every completed message.
     *
     * One caller at a time (the link's one receive context).
     */
    void wire_in(std::span<const std::byte> in) {
        while (!in.empty() && link_up()) {
            if (hdr_have_ < kHeaderBytes) {
                const std::size_t take = std::min(kHeaderBytes - hdr_have_, in.size());
                std::memcpy(hdr_.data() + hdr_have_, in.data(), take);
                hdr_have_ += take;
                in = in.subspan(take);
                if (hdr_have_ == kHeaderBytes) on_header();
                continue;
            }
            const std::size_t take = std::min(body_len_ - body_have_, in.size());
            if (blk_.seg) std::memcpy(blk_.frame(body_len_).data() + body_have_, in.data(), take);
            body_have_ += take;
            in = in.subspan(take);
            if (body_have_ == body_len_) on_body();
        }
    }

    /**
     * @brief The peer reads: everything the link has put on the wire since the last read.
     *
     * The window it frees is the writability event: the records waiting behind it are then
     * written into the window, for the next read.
     */
    [[nodiscard]] std::vector<std::byte> wire_out() {
        std::vector<std::byte> in;
        {
            const std::lock_guard lock(tx_m_);
            in = std::exchange(out_, {});
        }
        const net::egress_result_t r = egress_.flush(
            peer_, [&](const net::egress_frame_t& f, std::size_t off) { return put(f, off); },
            [&] { close(); });
        dropped_tx_.fetch_add(r.shed, std::memory_order_relaxed);
        return in;
    }

    /** @brief The peer leaves: the link goes down and says so, and sheds what waits. */
    void peer_gone() {
        close();
        egress_.close(peer_);
    }

   private:
    /** @brief Put @p head, then @p value's bytes when given, on the wire as ONE record. */
    void send_record(std::span<const std::span<const std::byte>> head, const graph::value_t* value,
                     const graph::delivery_lane_t& lane) {
        std::size_t len = value != nullptr ? value->total_length() : 0;
        for (const std::span<const std::byte>& s : head) len += s.size();
        if (len >= kMore) {
            dropped_tx_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::array<std::byte, kHeaderBytes> hdr{};
        wire::store_le<std::uint32_t>(hdr, static_cast<std::uint32_t>(len));
        // The copied part of a waiting frame is the header and the head; the value is kept.
        std::vector<std::span<const std::byte>> copied{std::span<const std::byte>(hdr)};
        copied.insert(copied.end(), head.begin(), head.end());
        std::vector<std::span<const std::byte>> whole = copied;
        if (value != nullptr)
            for (const view::view_t& l : value->links()) whole.push_back(l.bytes());
        net::egress_once_t once(
            [&] { return net::egress_frame_t::make(mem::heap_source(), copied, value); });
        const net::egress_result_t r = egress_.send(
            peer_, lane, once, [&] { return put(whole, len + kHeaderBytes); },
            [&](const net::egress_frame_t& f, std::size_t off) { return put(f, off); },
            [&] { close(); });
        dropped_tx_.fetch_add(r.shed, std::memory_order_relaxed);
    }

    /** @brief The rest of a waiting frame from @p off, as one write. */
    net::egress_step_t put(const net::egress_frame_t& f, std::size_t off) {
        std::vector<std::span<const std::byte>> spans(f.span_count());
        spans.resize(f.spans_from(off, spans.data()));
        return put(spans, f.size() - off);
    }

    /**
     * @brief The socket: the peer's window takes the @p bytes of @p spans whole, or nothing
     *        (the write is held). A record is never torn, so a read is always whole records.
     */
    net::egress_step_t put(std::span<const std::span<const std::byte>> spans, std::size_t bytes) {
        const std::lock_guard lock(tx_m_);
        if (!up_.load(std::memory_order_acquire)) return {0, false, true};
        if (out_.size() + bytes > peer_window_) return {0, true, false};
        for (const std::span<const std::byte>& s : spans)
            out_.insert(out_.end(), s.begin(), s.end());
        return {bytes, false, false};
    }

    /** @brief Close the link once, and fire the down notifier. */
    void close() {
        if (up_.exchange(false, std::memory_order_acq_rel)) notify_down();
    }

    /** @brief A whole header is in: judge it, and draw the body's segment if it is accepted. */
    void on_header() {
        const std::uint32_t word = wire::load_le<std::uint32_t>(hdr_);
        more_ = (word & kMore) != 0;
        body_len_ = word & ~kMore;
        body_have_ = 0;
        // The message's earlier fragments already hold part of the cap.
        const std::size_t room = max_frame_ - pending_len_;
        using kind_t = net::length_prefix_framer_t::prefix_decision_t::kind_t;
        net::length_prefix_framer_t::prefix_decision_t dec =
            discard_ ? net::length_prefix_framer_t::prefix_decision_t{kind_t::DROP, {}}
                     : net::length_prefix_framer_t::on_prefix(*rx_backend_, room, body_len_);
        if (dec.kind == kind_t::MALFORMED) {
            malformed_rx_.fetch_add(1, std::memory_order_relaxed);
            close();
            return;
        }
        if (dec.kind == kind_t::DROP && !discard_) {
            dropped_rx_.fetch_add(1, std::memory_order_relaxed);
            pending_ = view::rope_t{};
            pending_len_ = 0;
            discard_ = true;
        }
        blk_ = view::rx_block_t{std::move(dec.seg), dec.off};
        if (body_len_ == 0) on_body();
    }

    /** @brief A whole body is in: hand up the message it completes, if it completes one. */
    void on_body() {
        hdr_have_ = 0;
        if (blk_.seg) {
            view::view_t piece = blk_.take(body_len_);
            if (!more_ && pending_.link_count() == 0) {
                rx_.deliver(std::move(piece));
                return;
            }
            pending_len_ += body_len_;
            pending_.append(std::move(piece));
        }
        if (more_) return;
        if (!discard_ && pending_.link_count() != 0) rx_.deliver_rope(std::move(pending_));
        pending_ = view::rope_t{};
        pending_len_ = 0;
        discard_ = false;
    }

    mem::mem_backend_t* rx_backend_; /**< @brief The link's own receive source. */
    std::size_t max_frame_;          /**< @brief The longest message accepted. */
    std::size_t peer_window_;        /**< @brief Bytes the peer's window holds. */
    std::atomic<bool> up_{true};     /**< @brief Liveness. */

    // Ingress: touched by the one receive context only.
    std::array<std::byte, kHeaderBytes> hdr_{}; /**< @brief The header being read. */
    std::size_t hdr_have_ = 0;                  /**< @brief Header bytes read so far. */
    bool more_ = false;                         /**< @brief The current header's MORE bit. */
    std::size_t body_len_ = 0;                  /**< @brief The current body's length. */
    std::size_t body_have_ = 0;                 /**< @brief Body bytes read so far. */
    view::rx_block_t blk_;                      /**< @brief The current body's segment. */
    view::rope_t pending_;                      /**< @brief Earlier fragments of the message. */
    std::size_t pending_len_ = 0;               /**< @brief Their total length. */
    bool discard_ = false; /**< @brief The rest of a shed message is skipped, not drawn. */

    // Egress.
    std::mutex tx_m_;            /**< @brief Guards @ref out_. */
    std::vector<std::byte> out_; /**< @brief Bytes on the wire toward the peer, unread. */
    net::egress_t egress_{mem::heap_source()}; /**< @brief The lanes, drawn from the heap. */
    net::egress_peer_t peer_;                  /**< @brief The one peer's wire and lanes. */

    std::atomic<std::uint64_t> dropped_rx_{0};   /**< @brief Inbound frames shed. */
    std::atomic<std::uint64_t> malformed_rx_{0}; /**< @brief Inbound headers refused. */
    std::atomic<std::uint64_t> dropped_tx_{0};   /**< @brief Outbound frames shed. */
};

/** @brief The fake link plus the peer that drives its wire. */
class fake_rig_t final : public link_rig_t {
   public:
    /** @brief The peer's receive window: four of the stalled-peer case's frames fit. */
    static constexpr std::size_t kPeerWindow = 1024u * 1024u + 64u;

    [[nodiscard]] std::string_view name() const override { return "fake"; }
    [[nodiscard]] bool ready() const override { return true; }
    [[nodiscard]] net::transport_t& link() override { return link_; }
    [[nodiscard]] counting_backend_t& rx_backend() override { return rx_; }
    [[nodiscard]] bool lane_cases() const override { return true; }

    [[nodiscard]] bytes_t encode(std::span<const std::byte> body) const override {
        return record(body, false);
    }
    [[nodiscard]] bool supports_fragments() const override { return true; }
    [[nodiscard]] bytes_t encode_fragments(std::span<const std::byte> body,
                                           std::size_t pieces) const override {
        bytes_t out;
        const std::size_t step = (body.size() + pieces - 1) / pieces;
        for (std::size_t at = 0; at < body.size(); at += step) {
            const std::span<const std::byte> part =
                body.subspan(at, std::min(step, body.size() - at));
            const bytes_t one = record(part, at + part.size() < body.size());
            out.insert(out.end(), one.begin(), one.end());
        }
        return out;
    }
    [[nodiscard]] bytes_t oversize_header() const override {
        bytes_t out(fake_link_t::kHeaderBytes);
        wire::store_le<std::uint32_t>(out, static_cast<std::uint32_t>(kMaxFrame + 1));
        return out;
    }

    void peer_write(std::span<const std::byte> bytes) override { link_.wire_in(bytes); }
    void peer_stall() override {}  // the fake peer reads only inside peer_drain
    [[nodiscard]] bool peer_drain(std::chrono::milliseconds /*budget*/,
                                  std::vector<bytes_t>& whole) override {
        return decode(link_, whole);
    }
    void peer_leave() override { link_.peer_gone(); }

    /** @brief The peer reads @p link's wire and decodes it into @p whole (see
     *         @ref link_rig_t::peer_drain). */
    [[nodiscard]] static bool decode(fake_link_t& link, std::vector<bytes_t>& whole) {
        const bytes_t in = link.wire_out();
        std::size_t at = 0;
        while (in.size() - at >= fake_link_t::kHeaderBytes) {
            const std::size_t len = wire::load_le<std::uint32_t>(
                std::span<const std::byte>(in).subspan(at, fake_link_t::kHeaderBytes));
            if (in.size() - at - fake_link_t::kHeaderBytes < len) break;
            const auto b = in.begin() + static_cast<std::ptrdiff_t>(at + fake_link_t::kHeaderBytes);
            whole.emplace_back(b, b + static_cast<std::ptrdiff_t>(len));
            at += fake_link_t::kHeaderBytes + len;
        }
        return at == in.size() || !link.link_up();
    }

    /** @brief One record of @p body, with the MORE bit when @p more. */
    [[nodiscard]] static bytes_t record(std::span<const std::byte> body, bool more) {
        bytes_t out(fake_link_t::kHeaderBytes + body.size());
        const std::uint32_t word =
            static_cast<std::uint32_t>(body.size()) | (more ? fake_link_t::kMore : 0u);
        wire::store_le<std::uint32_t>(out, word);
        if (!body.empty())
            std::memcpy(out.data() + fake_link_t::kHeaderBytes, body.data(), body.size());
        return out;
    }

   private:
    counting_backend_t rx_;                         /**< @brief The receive source. */
    fake_link_t link_{rx_, kMaxFrame, kPeerWindow}; /**< @brief The link under test. */
};

}  // namespace tr::testing::link_seam
