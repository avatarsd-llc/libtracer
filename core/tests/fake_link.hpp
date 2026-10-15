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
 * - **Egress.** A send appends one record to the peer's window and never waits. A record
 *   that does not fit (the peer stopped reading), or any send once the peer has gone, is
 *   shed and counted in `dropped_tx`.
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
#include "libtracer/length_prefix_framer.hpp"
#include "libtracer/rope.hpp"
#include "libtracer/transport.hpp"
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
        : rx_backend_(&rx), max_frame_(max_frame), peer_window_(peer_window) {}
    /** @brief Destroyed as the class it is. */
    ~fake_link_t() = default;
    fake_link_t(const fake_link_t&) = delete;
    fake_link_t& operator=(const fake_link_t&) = delete;

    /** @brief Append one record to the peer's window, or shed and count it. Never waits. */
    void send(std::span<const std::byte> frame) override {
        const std::lock_guard lock(tx_m_);
        const bool fits = out_.size() + kHeaderBytes + frame.size() <= peer_window_;
        if (!up_.load(std::memory_order_acquire) || !fits || frame.size() >= kMore) {
            dropped_tx_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        const std::size_t at = out_.size();
        out_.resize(at + kHeaderBytes + frame.size());
        wire::store_le<std::uint32_t>(std::span<std::byte>(out_).subspan(at, kHeaderBytes),
                                      static_cast<std::uint32_t>(frame.size()));
        if (!frame.empty())
            std::memcpy(out_.data() + at + kHeaderBytes, frame.data(), frame.size());
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

    /** @brief The peer reads: everything the link has put on the wire since the last read. */
    [[nodiscard]] std::vector<std::byte> wire_out() {
        const std::lock_guard lock(tx_m_);
        return std::exchange(out_, {});
    }

    /** @brief The peer leaves: the link goes down and says so. */
    void peer_gone() { close(); }

   private:
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
    [[nodiscard]] const counting_backend_t& rx_backend() const override { return rx_; }

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
        const bytes_t in = link_.wire_out();
        std::size_t at = 0;
        while (in.size() - at >= fake_link_t::kHeaderBytes) {
            const std::size_t len = wire::load_le<std::uint32_t>(
                std::span<const std::byte>(in).subspan(at, fake_link_t::kHeaderBytes));
            if (in.size() - at - fake_link_t::kHeaderBytes < len) break;
            const auto b = in.begin() + static_cast<std::ptrdiff_t>(at + fake_link_t::kHeaderBytes);
            whole.emplace_back(b, b + static_cast<std::ptrdiff_t>(len));
            at += fake_link_t::kHeaderBytes + len;
        }
        return at == in.size() || !link_.link_up();
    }
    void peer_leave() override { link_.peer_gone(); }

   private:
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

    counting_backend_t rx_;                         /**< @brief The receive source. */
    fake_link_t link_{rx_, kMaxFrame, kPeerWindow}; /**< @brief The link under test. */
};

}  // namespace tr::testing::link_seam
