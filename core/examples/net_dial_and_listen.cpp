/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — DIAL and LISTEN are not two types, they are two CONSTRUCTORS of the
 *        same type; once a link is up the role has left the building and both ends are the
 *        same `transport_t`.
 *
 * This is the minimal pair, spelled with `tcp_transport_t` because it is the kind that
 * makes the point plainest — one class, two constructors, and nothing downstream can tell
 * which one built the object it holds:
 *
 *  - `tcp_transport_t(bind_port)` LISTENS. Pass `0` and the kernel picks the port;
 *    `local_port()` reports which, which is how a dialer in the same process (a test, a
 *    demo, a supervisor that spawns both halves) finds it without a hard-coded number.
 *  - `tcp_transport_t(peer_host, peer_port)` DIALS, synchronously, in the constructor.
 *
 * Two properties follow from "the role is the constructor", and both are load-bearing:
 *
 *  1. **`ok()` is the CAME-UP predicate, and it is role-specific** (#1059): on LISTEN it
 *     answers "did the bind succeed", on DIAL "did the connect succeed". It is answered
 *     once, right after construction, and never reverts. `link_up()` is the different
 *     question — is the connection alive RIGHT NOW — and after a teardown the two diverge:
 *     `ok()` stays true (the link DID come up), `link_up()` goes false.
 *  2. **Direction is not a property of the link.** The route the FWD plane grows into a
 *     frame's `src` is what makes a reply routable, not which end opened the socket, so
 *     both directions are demonstrated here over the one connection.
 *
 * The failed bring-up is provoked deterministically — a second LISTEN on a port the first
 * one already holds — rather than by dialling a port nobody is expected to answer, which
 * would be a guess about the machine rather than a demonstration.
 *
 * Needs the TCP transport (`LIBTRACER_TRANSPORT_TCP`, on by default). Runs under ctest as
 * `example_net_dial_and_listen`; returns non-zero on any failed check.
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <vector>

#include "libtracer/transport.hpp"
#include "libtracer/transport_tcp.hpp"

namespace {

using namespace std::chrono_literals;
using tr::net::tcp_transport_t;

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

/**
 * @brief Everything past bring-up, written against the SEAM and not against either role.
 *
 * The whole point of the example in one signature: this function cannot tell which
 * constructor made either argument, and does not need to.
 */
void exchange(bool& ok, tr::net::transport_t& from, sink_t& at_far_end, std::size_t nth,
              unsigned seed, const char* what) {
    const auto f = frame_of(6, seed);
    from.send(f);
    check(ok, at_far_end.wait_for(nth, 2s), what);
    check(ok, at_far_end.at(nth - 1) == f, "  …byte-identical");
}

}  // namespace

int main() {
    bool ok = true;

    // LISTEN. Port 0 asks the kernel for an ephemeral one; local_port() reports the answer.
    sink_t at_listener;
    tcp_transport_t listener(std::uint16_t{0});
    listener.set_receiver(at_listener);
    check(ok, listener.ok(), "LISTEN came up — the bind succeeded");
    const std::uint16_t port = listener.local_port();
    check(ok, port != 0, "local_port() resolved the ephemeral 0 to a real port");

    // DIAL. The connect runs inside the constructor, so ok() answers for it on return.
    sink_t at_dialer;
    tcp_transport_t dialer("127.0.0.1", port);
    dialer.set_receiver(at_dialer);
    check(ok, dialer.ok(), "DIAL came up — the connect succeeded");

    // Past bring-up neither end is privileged: the same call, both ways, through a reference
    // that has forgotten which constructor ran.
    std::printf("both directions over the one connection:\n");
    exchange(ok, dialer, at_listener, 1, 0x10, "dialer -> listener");
    exchange(ok, listener, at_dialer, 1, 0x20, "listener -> dialer");
    exchange(ok, dialer, at_listener, 2, 0x30, "dialer -> listener, again");

    check(ok, listener.link_up() && dialer.link_up(), "both ends report the link live");

    // A bring-up that FAILS, provoked deterministically: the port is already held by the
    // listener above, so the second bind cannot succeed on any machine.
    std::printf("a refused bring-up:\n");
    tcp_transport_t taken(port);
    check(ok, !taken.ok(), "a second LISTEN on a held port did NOT come up");
    check(ok, !taken.link_up(), "…and reports no live link either — nothing was spawned");

    // ok() never reverts, which is what makes it a different question from link_up(). The
    // listener is still up here, so this only states the invariant the two accessors carry;
    // the divergence itself belongs to the teardown paths the transport tests cover.
    check(ok, listener.ok() && dialer.ok(), "ok() is the came-up fact, answered once");

    std::printf("one type, two constructors: listener on port %u, %s\n",
                static_cast<unsigned>(port), ok ? "3 frames exchanged" : "FAILED");
    return ok ? 0 : 1;
}
