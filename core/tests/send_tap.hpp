/**
 * @file
 * @brief A test tap on a link's outbound side (#2277): a link that shows every frame it
 *        sends to an observer, then sends it on the link it wraps.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The router carried two frame observers (`on_raw`, `on_inbound`) that no production adapter
 * used and every received frame paid for. They are gone; a test that wants to see what
 * crossed a hop watches the link the frame was sent on instead. The sender hands the router
 * this tap where it would have handed the link itself; what the tap's observer sees is what
 * the peer receives.
 *
 * Every send lowers to the one contiguous `send`: the tap overrides only that, so the
 * gathered and retained forms reach it through the base's own lowering (one gather copy per
 * frame, which a test does not care about). Inbound frames, liveness and link-down pass
 * straight through from the wrapped link. A flat link only: a bus is not wrapped.
 */

#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "libtracer/rope.hpp"
#include "libtracer/transport.hpp"

namespace tr::testing {

/**
 * @brief A flat link that observes its own sends, then forwards them to @p inner.
 * @tparam Obs Called as `obs(std::span<const std::byte> frame)` on the sending thread, before
 *             the frame goes out. It keeps a copy of anything it needs afterwards.
 */
template <typename Obs>
class send_tap_t final : public net::transport_t {
   public:
    /** @brief Wrap @p inner, which must outlive this tap; take over its inbound delivery
     *         (both tiers: the inner link uses the one it delivers on). */
    send_tap_t(net::transport_t& inner, Obs obs) : inner_(inner), obs_(std::move(obs)) {
        inner_.set_rope_receiver(
            [](void* c, view::rope_t f) {
                static_cast<send_tap_t*>(c)->rx_.deliver_rope(std::move(f));
            },
            this);
        inner_.set_receiver(
            [](void* c, std::span<const std::byte> f) {
                static_cast<send_tap_t*>(c)->rx_.deliver_borrowed(f);
            },
            this);
        inner_.set_down_notifier([](void* c) { static_cast<send_tap_t*>(c)->notify_down(); }, this);
    }

    send_tap_t(const send_tap_t&) = delete;
    send_tap_t& operator=(const send_tap_t&) = delete;
    ~send_tap_t() = default;

    using net::transport_t::send;
    void send(std::span<const std::byte> frame) override {
        obs_(frame);
        inner_.send(frame);
    }
    [[nodiscard]] bool delivers_ropes() const override { return inner_.delivers_ropes(); }
    void start_receiving() override { inner_.start_receiving(); }
    [[nodiscard]] bool link_up() const noexcept override { return inner_.link_up(); }

   private:
    net::transport_t& inner_;
    Obs obs_;
};

}  // namespace tr::testing
