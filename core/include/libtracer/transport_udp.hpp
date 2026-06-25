// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright 2026 Avatar LLC
//
// transport_udp — a real UDP socket Transport (M5). The first transport that
// actually crosses the kernel network stack; the bridge / router / graph above
// it are unchanged. One datagram carries one whole frame (no stream reassembly),
// so it pairs with the flat decoder. POSIX sockets; a receive thread drains the
// socket into the registered receiver. Datagram payloads are bounded by the UDP
// limit (~65507 bytes) — large TLVs need a streaming transport (future).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <thread>

#include "libtracer/transport.hpp"

namespace tracer {

class UdpTransport : public Transport {
   public:
    // Bind a local UDP socket on `bind_port` (0 = ephemeral; see local_port());
    // send() targets `peer_host:peer_port` (IPv4 dotted-quad, e.g. "127.0.0.1").
    UdpTransport(std::uint16_t bind_port, const std::string& peer_host, std::uint16_t peer_port);
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    void send(std::span<const std::byte> frame) override;  // sendto(peer)
    void set_receiver(Receiver receiver) override;

    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }
    [[nodiscard]] std::uint16_t local_port() const noexcept { return bound_port_; }

   private:
    void run();  // receive thread

    int fd_ = -1;
    std::uint16_t bound_port_ = 0;
    std::uint32_t peer_ip_ = 0;    // network byte order
    std::uint16_t peer_port_ = 0;  // host byte order

    Receiver receiver_;  // guarded by m_
    std::mutex m_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace tracer
