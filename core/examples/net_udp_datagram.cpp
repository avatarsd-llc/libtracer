/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — on a DATAGRAM kind the frame boundary is the wire's own, so `udp`
 *        carries no framing layer at all: one datagram in, one whole frame out, and the
 *        MTU is therefore the frame size limit rather than a tuning knob.
 *
 * Every stream kind in the tree (`tcp`, `ws`, `quic`) has to invent message boundaries,
 * because a stream has none. UDP already has them, so `udp_transport_t` adds nothing: a
 * `send` is one `sendto`, an inbound datagram is one frame, and there is no reassembler,
 * no length prefix and no partial-frame state anywhere in the kind. That is the whole
 * difference between the two families, and it has two consequences worth seeing:
 *
 *  - **The bound is hard, not configurable upward.** `kMaxDatagram` is what a datagram can
 *    physically be, so the `:settings max_frame` key can only TIGHTEN it. A frame larger
 *    than a datagram is not a UDP frame; that is what streaming kinds are for.
 *  - **The listener has no peer until one talks to it.** UDP is connectionless, so a
 *    listener-mode transport — constructed with no peer address — LEARNS its peer from the
 *    source address of the first inbound datagram. Until then `send` is a no-op, which is
 *    exactly what lets a config-created listener reply to a dialer whose ephemeral source
 *    port could not have been known in advance.
 *
 * The example runs two real UDP sockets on the loopback interface, the way `udp_test` does:
 * both bind port 0, so the kernel picks the ports and nothing here can collide with
 * whatever else is running on the machine.
 *
 * Needs the UDP transport (`LIBTRACER_TRANSPORT_UDP`, on by default). Runs under ctest as
 * `example_net_udp_datagram`; returns non-zero on any failed check.
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <vector>

#include "libtracer/transport_udp.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::udp_transport_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief A thread-safe borrowed-span sink: the recv thread pushes, `main` waits. */
class sink_t {
   public:
    /** @brief The receiver callback — copies the span, which dies when it returns. */
    void operator()(std::span<const std::byte> frame) {
        {
            const std::lock_guard lock(m_);
            frames_.emplace_back(frame.begin(), frame.end());
        }
        cv_.notify_all();
    }

    /** @brief Wait until at least @p n frames have landed, or @p budget expires. */
    [[nodiscard]] bool wait_for(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, budget, [&] { return frames_.size() >= n; });
    }

    /** @brief How many frames have landed so far. */
    [[nodiscard]] std::size_t count() const {
        const std::lock_guard lock(m_);
        return frames_.size();
    }

    /** @brief Frame @p i, by value. */
    [[nodiscard]] std::vector<std::byte> at(std::size_t i) const {
        const std::lock_guard lock(m_);
        return frames_.at(i);
    }

   private:
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::vector<std::byte>> frames_;
};

/** @brief @p n bytes counting up from @p seed — a stand-in for an encoded frame. */
std::vector<std::byte> frame_of(std::size_t n, unsigned seed) {
    std::vector<std::byte> f(n);
    for (std::size_t i = 0; i < n; ++i) f[i] = static_cast<std::byte>(seed + i);
    return f;
}

}  // namespace

int main() {
    bool ok = true;

    // The listener half: bind an ephemeral port, name NO peer. This is the shape a
    // `role=listener` config creates, and the reason `send` has to tolerate having nobody to
    // send to yet.
    sink_t at_listener;
    udp_transport_t listener(/*bind_port=*/0, /*peer_host=*/"", /*peer_port=*/0);
    listener.set_receiver(at_listener);
    check(ok, listener.ok(), "the listener socket bound");
    const std::uint16_t port = listener.local_port();
    check(ok, port != 0, "local_port() resolved the ephemeral 0");

    // A send before any peer is known is a NO-OP, not an error and not a queued frame: there
    // is no address to send to, and UDP has nowhere to hold it.
    listener.send(frame_of(4, 0x01));
    check(ok, at_listener.count() == 0, "a send with no peer learned yet went nowhere");

    // The dialer half: also an ephemeral local port, but with the peer named up front.
    sink_t at_dialer;
    udp_transport_t dialer(/*bind_port=*/0, "127.0.0.1", port);
    dialer.set_receiver(at_dialer);
    check(ok, dialer.ok(), "the dialer socket bound");

    // One datagram, one frame. No prefix is written and none is stripped — what the sender
    // handed to `send` is what the receiver's callback sees, and its LENGTH came from the
    // datagram rather than from anything in the bytes.
    std::printf("one datagram is one frame:\n");
    const auto f1 = frame_of(9, 0x10);
    dialer.send(f1);
    check(ok, at_listener.wait_for(1, 2s), "the datagram arrived as one whole frame");
    check(ok, at_listener.at(0) == f1, "byte-identical, with no framing bytes added");

    // Two sends are two datagrams and therefore two frames — never one coalesced read the
    // receiver has to split. This is the property a stream kind has to reconstruct.
    const auto f2 = frame_of(3, 0x20);
    const auto f3 = frame_of(11, 0x30);
    dialer.send(f2);
    dialer.send(f3);
    check(ok, at_listener.wait_for(3, 2s), "two more sends arrived as exactly two more frames");
    check(ok, at_listener.at(1) == f2 && at_listener.at(2) == f3,
          "…each with its own boundary, in order");

    // The listener has now heard from the dialer, so it knows where to reply — learned from
    // the datagram's source address, with nothing stored per request.
    std::printf("the peer is learned from ingress:\n");
    const auto reply = frame_of(7, 0x40);
    listener.send(reply);
    check(ok, at_dialer.wait_for(1, 2s), "the listener could reply once it had heard a peer");
    check(ok, at_dialer.at(0) == reply, "…and the reply is byte-identical too");

    // The bound is the datagram's, and `max_frame` may only tighten it.
    check(ok, listener.effective_max_frame() <= udp_transport_t::kMaxDatagram,
          "the receive cap never exceeds what a datagram can carry");
    check(ok, listener.malformed_rx() == 0 && listener.dropped_rx() == 0,
          "nothing was refused and nothing was shed on this run");

    std::printf("udp: %zu frames at the listener, %zu at the dialer, 0 bytes of framing\n",
                at_listener.count(), at_dialer.count());
    return ok ? 0 : 1;
}
