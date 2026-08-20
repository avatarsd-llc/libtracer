/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — every wire technology reaches the routing plane through the SAME
 *        three-call seam, and the seam is deliberately ignorant: a `transport_t` moves
 *        framed BYTES and is never told what they mean.
 *
 * `tr::net::transport_t` is the whole contract a kind has to satisfy (ADR-0013):
 *
 *  - `send(std::span<const std::byte>)` — emit one complete frame. Mandatory.
 *  - `send(std::span<const std::span<const std::byte>>)` — emit the SAME frame from
 *    scattered spans, as one record. Optional: the base gathers into one buffer drawn
 *    from the link's egress source, so a kind that has a real writev implements it and
 *    a kind that does not simply inherits the copy.
 *  - `set_receiver` / `set_rope_receiver` — where inbound frames land. Both may fire on
 *    the kind's own receive thread, and both must be installed BEFORE frames flow.
 *
 * That is the entire surface. There is no `send_read`, no `send_reply`, no TLV type
 * anywhere in it, and that absence is the design: the router owns addressing (the `dst`
 * source route), the codec owns structure, and the transport owns exactly one question —
 * how do these bytes cross this wire. It is why `udp`, `tcp`, `ws`, `can`, `quic`,
 * `webtransport` and the in-process loopback are interchangeable to everything above,
 * and why writing a new kind is writing this class and nothing else.
 *
 * The example writes its own kind — 22 lines, no sockets — and then swaps in the shipped
 * `loopback_channel_t` to show the identical calls driving a link with a real receive
 * thread. Runs under ctest as `example_net_transport_seam`; returns non-zero on any
 * failed check.
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <span>
#include <vector>

#include "libtracer/loopback.hpp"
#include "libtracer/transport.hpp"

namespace {

using namespace std::chrono_literals;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/**
 * @brief A complete transport kind, in the smallest form the seam admits: it keeps
 *        every frame handed to it and can hand one back.
 *
 * Only the mandatory `send(span)` is overridden. The scatter-gather overload is
 * deliberately NOT implemented, so the second half of this example can show what the base
 * does about that — which is the reason the seam has two overloads instead of one.
 */
class memo_link_t final : public tr::net::transport_t {
   public:
    /** @brief Frames this link was asked to emit, in order. */
    std::vector<std::vector<std::byte>> sent;

    /**
     * @brief Keep the base's scatter-gather overload visible.
     *
     * Declaring one `send` HIDES the other — ordinary C++ name hiding, and a kind that omits
     * this line silently loses the iovec entry point it meant to inherit.
     */
    using tr::net::transport_t::send;

    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }

    /** @brief Deliver @p frame inbound, exactly as a receive thread would. */
    void inject(std::span<const std::byte> frame) { rx_.deliver_borrowed(frame); }
};

/** @brief A thread-safe borrowed-span sink: a receive thread pushes, `main` waits. */
class sink_t {
   public:
    /** @brief The receiver callback — copies the borrowed span, which dies at return. */
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

    // 1. Egress. A frame is a span of bytes; the transport is told nothing else about it.
    std::printf("the seam, on a kind written here:\n");
    memo_link_t link;
    const auto f1 = frame_of(6, 0x10);
    link.send(f1);
    check(ok, link.sent.size() == 1 && link.sent[0] == f1, "send(span) emitted the frame verbatim");

    // 2. The scatter-gather overload this kind did NOT implement. The base concatenates the
    // spans into ONE block from the link's egress source and calls the span overload — so a
    // rope's `to_iovec()` reaches every kind, whether or not the kind can writev. What a kind
    // gains by overriding it is the elision of exactly this copy, never a different contract:
    // the bytes on the wire are the same one record either way.
    const auto head = frame_of(3, 0x20);
    const auto tail = frame_of(4, 0x30);
    const std::span<const std::byte> parts[] = {head, tail};
    link.send(std::span<const std::span<const std::byte>>{parts});
    std::vector<std::byte> joined = head;
    joined.insert(joined.end(), tail.begin(), tail.end());
    check(ok, link.sent.size() == 2 && link.sent[1] == joined,
          "send(iov) arrived as ONE frame — the base gathered it for a kind that cannot");

    // 3. Ingress. The sink is installed before anything is delivered, because that ordering is
    // the contract: a kind whose receive thread starts in its constructor is already draining
    // the wire while the owner is still wiring, and a frame that lands in an empty slot is
    // dropped with no counter moving. Kinds that dial offer `defer_recv` + `start_receiving()`
    // for exactly this reason.
    sink_t inbound;
    link.set_receiver(inbound);
    const auto f2 = frame_of(5, 0x40);
    link.inject(f2);
    check(ok, inbound.count() == 1 && inbound.at(0) == f2, "the borrowed-span sink got the frame");
    check(ok, !link.delivers_ropes(), "and this kind claims no OWNING delivery (the default)");

    // 4. The same three calls against a shipped kind with a real receive thread. Nothing about
    // the call sites changes — that interchangeability IS the seam.
    std::printf("the same seam, on the shipped in-process loopback kind:\n");
    tr::net::loopback_channel_t channel;
    sink_t at_b;
    channel.b().set_receiver(at_b);
    const auto f3 = frame_of(8, 0x50);
    channel.a().send(f3);
    check(ok, at_b.wait_for(1, 2s), "a frame sent on endpoint a arrived at endpoint b");
    check(ok, at_b.count() == 1 && at_b.at(0) == f3, "byte-identical across the 'wire'");
    channel.shutdown();

    std::printf("one seam, %zu egress calls and %zu inbound frames, zero TLV semantics\n",
                link.sent.size(), inbound.count() + at_b.count());
    return ok ? 0 : 1;
}
