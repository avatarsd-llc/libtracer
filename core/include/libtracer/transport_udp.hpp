/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * transport_udp — a real UDP socket transport_t (M5). The first transport that
 * actually crosses the kernel network stack; the bridge / router / graph above
 * it are unchanged. One datagram carries one whole frame (no stream reassembly),
 * so it pairs with the flat decoder. POSIX sockets; a receive thread drains the
 * socket into the registered receiver. Datagram payloads are bounded by the UDP
 * limit (~65507 bytes) — large TLVs need a streaming transport (future).
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include "libtracer/transport.hpp"

namespace tr::net {

class udp_transport_t : public transport_t {
   public:
    // Bind a local UDP socket on `bind_port` (0 = ephemeral; see local_port());
    // send() targets `peer_host:peer_port` (IPv4 dotted-quad, e.g. "127.0.0.1").
    udp_transport_t(std::uint16_t bind_port, const std::string& peer_host, std::uint16_t peer_port);
    ~udp_transport_t() override;

    udp_transport_t(const udp_transport_t&) = delete;
    udp_transport_t& operator=(const udp_transport_t&) = delete;

    void send(std::span<const std::byte> frame) override;                 // sendto(peer)
    void send(std::span<const std::span<const std::byte>> iov) override;  // sendmsg(iovec)
    void set_receiver(receiver_t receiver) override;

    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }

   private:
    void run();  // receive thread

    int fd_ = -1;
    std::uint16_t bound_port_ = 0;
    std::uint32_t peer_ip_ = 0;    // network byte order
    std::uint16_t peer_port_ = 0;  // host byte order

    receiver_t receiver_;  // guarded by m_
    std::mutex m_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace tr::net
