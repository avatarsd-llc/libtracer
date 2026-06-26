/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * An in-process loopback transport (dev/test only — docs/reference/10's
 * transports are all real wire tech). A loopback_channel_t owns two endpoints; a
 * frame sent on one endpoint is delivered to the OTHER endpoint's receiver, on
 * that endpoint's receive thread (modeling async cross-"wire" delivery, so a
 * bridge cycle terminates by hop_count rather than stack recursion). No sockets;
 * deterministic; the vehicle for exercising the bridge + ROUTER + dedup end to
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

class loopback_endpoint_t final : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override;
    void set_receiver(receiver_t receiver) override;

   private:
    friend class loopback_channel_t;
    loopback_endpoint_t() = default;

    void start();
    void stop();                                 // idempotent: join the recv thread
    void enqueue(std::vector<std::byte> frame);  // called by the peer's send()
    void run();                                  // receive thread

    loopback_endpoint_t* peer_ = nullptr;
    std::deque<std::vector<std::byte>> inbox_;
    receiver_t receiver_;  // guarded by m_
    std::mutex m_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

class loopback_channel_t {
   public:
    loopback_channel_t();
    ~loopback_channel_t();
    loopback_channel_t(const loopback_channel_t&) = delete;
    loopback_channel_t& operator=(const loopback_channel_t&) = delete;

    [[nodiscard]] loopback_endpoint_t& a() noexcept { return a_; }
    [[nodiscard]] loopback_endpoint_t& b() noexcept { return b_; }

    void shutdown();  // join both receive threads (idempotent)

   private:
    loopback_endpoint_t a_;
    loopback_endpoint_t b_;
};

}  // namespace tr::net
