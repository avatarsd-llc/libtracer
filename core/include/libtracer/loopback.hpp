/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * An in-process loopback transport (dev/test only — docs/reference/10's
 * transports are all real wire tech). A loopback_channel_t owns two endpoints; a
 * frame sent on one endpoint is delivered to the OTHER endpoint's receiver, on
 * that endpoint's receive thread (modeling async cross-"wire" delivery, so no
 * forwarding recurses on the sender's stack). No sockets; deterministic; the
 * vehicle for exercising FWD forward/reply routing (RFC-0004, ADR-0040) end to
 * end.
 *
 * Lifetime: call shutdown() (or destroy the channel) before the registered
 * receivers are destroyed — it joins the receive threads so no frame is
 * delivered to a dead receiver.
 */
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

#include "libtracer/transport.hpp"

namespace tr::net {

class loopback_channel_t;

/**
 * @brief One end of an in-process loopback link (dev/test only).
 *
 * A @ref transport_t whose @ref send hands the frame to the PEER endpoint's receiver, on
 * that peer's receive thread. Constructed and owned by a @ref loopback_channel_t.
 */
class loopback_endpoint_t final : public transport_t {
   public:
    /** @brief Send one frame — delivered to the peer endpoint's receiver on its recv thread. */
    void send(std::span<const std::byte> frame) override;

   private:
    friend class loopback_channel_t;
    loopback_endpoint_t() = default;

    void start();
    void stop();                                 // idempotent: join the recv thread
    void enqueue(std::vector<std::byte> frame);  // called by the peer's send()
    void run();                                  // receive thread

    loopback_endpoint_t* peer_ = nullptr;
    std::deque<std::vector<std::byte>> inbox_;
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

/**
 * @brief An in-process loopback channel: two endpoints, each delivering to the other.
 *
 * A frame sent on @ref a is delivered to @ref b's receiver (and vice-versa), on that
 * endpoint's receive thread — modeling async cross-"wire" delivery so forwarding never
 * recurses on the sender's stack. No sockets; deterministic. The vehicle for exercising FWD
 * forward/reply routing end to end (RFC-0004, ADR-0040). Non-copyable. Call @ref shutdown
 * (or destroy the channel) before the registered receivers are destroyed.
 */
class loopback_channel_t {
   public:
    /** @brief Construct a channel and start both endpoints' receive threads. */
    loopback_channel_t();
    /** @brief Destroy the channel, joining both receive threads first. */
    ~loopback_channel_t();
    loopback_channel_t(const loopback_channel_t&) = delete;
    loopback_channel_t& operator=(const loopback_channel_t&) = delete;

    /** @brief The first endpoint; a frame sent here is delivered to @ref b. */
    [[nodiscard]] loopback_endpoint_t& a() noexcept { return a_; }
    /** @brief The second endpoint; a frame sent here is delivered to @ref a. */
    [[nodiscard]] loopback_endpoint_t& b() noexcept { return b_; }

    /** @brief Join both receive threads so no frame reaches a dead receiver (idempotent). */
    void shutdown();

   private:
    loopback_endpoint_t a_;
    loopback_endpoint_t b_;
};

}  // namespace tr::net
