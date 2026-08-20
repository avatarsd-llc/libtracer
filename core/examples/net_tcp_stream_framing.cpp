/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a STREAM kind has to supply the frame boundaries the wire does not
 *        have, and `tcp` does it with a 4-byte little-endian length prefix that is
 *        TRANSPORT framing: it is on the wire, it is not in the TLV, and nothing above the
 *        transport ever sees it.
 *
 * TCP delivers bytes, not messages. Whatever `write` calls a sender makes, the receiver may
 * see them merged, split, or both — so a stream transport that handed its reader's buffer
 * straight up would deliver half frames and double frames. `tcp_transport_t` prefixes each
 * frame with `u32-LE length` and reads it back in two steps: read four bytes, then read
 * exactly that many. That is why the prefix is a FIXED width and not the TLV's own length
 * field — a variable-width header cannot be read without first buffering an unknown number
 * of bytes, which is the problem it was supposed to solve.
 *
 * The three stream hazards, each provoked on purpose against a raw POSIX socket so the
 * boundaries are this example's to choose rather than the kernel's to decide:
 *
 *  1. **Coalesced.** Two whole records in ONE `write` arrive as TWO frames.
 *  2. **Split.** One record dribbled out in three `write`s — the length prefix itself torn
 *     in half — arrives as ONE frame.
 *  3. **Framed on egress too.** A frame the transport SENDS is read off the raw socket as
 *     `u32-LE length ++ bytes`, so the prefix is demonstrated on the wire and not merely
 *     asserted about.
 *
 * A prefix above the effective cap is the fourth case and is deliberately NOT here: it
 * tears the connection down, which is a lifecycle concept rather than a framing one, and it
 * is what `tcp_test`'s oversize-prefix case covers.
 *
 * Needs the TCP transport (`LIBTRACER_TRANSPORT_TCP`, on by default). Runs under ctest as
 * `example_net_tcp_stream_framing`; returns non-zero on any failed check.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <vector>

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

/**
 * @brief A raw POSIX TCP client — this example's own hand on the wire.
 *
 * A second `tcp_transport_t` would be the realistic peer, and it is exactly the wrong tool
 * here: it would choose the write boundaries itself and hide the framing being demonstrated.
 */
class raw_client_t {
   public:
    /** @brief Connect to `127.0.0.1:@p port`; @ref ok reports whether it succeeded. */
    explicit raw_client_t(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~raw_client_t() {
        if (fd_ >= 0) ::close(fd_);
    }

    raw_client_t(const raw_client_t&) = delete;
    raw_client_t& operator=(const raw_client_t&) = delete;

    /** @brief True iff the connect succeeded. */
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    /** @brief Push @p bytes as ONE write, resuming partials — one chosen boundary. */
    void write(std::span<const std::byte> bytes) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::send(fd_, bytes.data() + off, bytes.size() - off, 0);
            if (n <= 0) return;
            off += static_cast<std::size_t>(n);
        }
    }

    /** @brief Read up to @p want bytes within @p budget, answering whatever arrived. */
    [[nodiscard]] std::vector<std::byte> read_within(std::size_t want,
                                                     std::chrono::milliseconds budget) {
        std::vector<std::byte> got;
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (got.size() < want) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            pollfd p{fd_, POLLIN, 0};
            if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) break;
            std::byte buf[256];
            const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got.insert(got.end(), buf, buf + n);
        }
        return got;
    }

   private:
    int fd_ = -1;
};

/** @brief @p n bytes counting up from @p seed — a stand-in for an encoded frame. */
std::vector<std::byte> frame_of(std::size_t n, unsigned seed) {
    std::vector<std::byte> f(n);
    for (std::size_t i = 0; i < n; ++i) f[i] = static_cast<std::byte>(seed + i);
    return f;
}

/** @brief One wire record: `u32-LE length ++ @p payload` — the framing, spelled by hand. */
std::vector<std::byte> record(std::span<const std::byte> payload) {
    const auto len = static_cast<std::uint32_t>(payload.size());
    std::vector<std::byte> out;
    for (unsigned shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::byte>((len >> shift) & 0xFFu));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/** @brief Decode a `u32-LE` at the front of @p bytes — the prefix, read back. */
std::uint32_t le32(std::span<const std::byte> bytes) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(bytes[i]) << (8 * i);
    return v;
}

}  // namespace

int main() {
    bool ok = true;

    sink_t at_listener;
    tcp_transport_t listener(std::uint16_t{0});
    listener.set_receiver(at_listener);
    check(ok, listener.ok(), "the listener bound an ephemeral port");

    raw_client_t client(listener.local_port());
    check(ok, client.ok(), "the raw client connected");

    // 1. COALESCED. Two complete records in one write. A reader that trusted its read
    // boundaries would deliver this as one frame of the wrong length.
    std::printf("two records in one write:\n");
    const auto a = frame_of(5, 0x10);
    const auto b = frame_of(9, 0x20);
    std::vector<std::byte> both = record(a);
    const auto rec_b = record(b);
    both.insert(both.end(), rec_b.begin(), rec_b.end());
    client.write(both);
    check(ok, at_listener.wait_for(2, 2s), "arrived as TWO frames");
    check(ok, at_listener.at(0) == a && at_listener.at(1) == b,
          "…split at the right byte, in order");

    // 2. SPLIT. One record in three writes, with the 4-byte prefix itself torn in half — the
    // case a fixed-width prefix has to survive, since the reader cannot even know the frame
    // length until all four bytes are in hand.
    std::printf("one record in three writes, prefix torn in half:\n");
    const auto c = frame_of(12, 0x30);
    const auto rec_c = record(c);
    client.write(std::span(rec_c).first(2));
    client.write(std::span(rec_c).subspan(2, 6));
    client.write(std::span(rec_c).subspan(8));
    check(ok, at_listener.wait_for(3, 2s), "reassembled into ONE frame");
    check(ok, at_listener.at(2) == c, "…byte-identical to what was framed");
    check(ok, at_listener.count() == 3, "and no fourth frame was invented from the fragments");

    // 3. EGRESS. The prefix is not an internal convention — it is on the wire, and the raw
    // client reads it there.
    std::printf("the prefix, read off the wire:\n");
    const auto out = frame_of(7, 0x40);
    listener.send(out);
    const auto wire = client.read_within(4 + out.size(), 2s);
    check(ok, wire.size() == 4 + out.size(), "the transport wrote exactly prefix + frame");
    check(ok, wire.size() >= 4 && le32(wire) == out.size(),
          "…the first four bytes are the frame length, little-endian");
    check(ok, std::vector<std::byte>(wire.begin() + 4, wire.end()) == out,
          "…and the rest is the frame, untouched");

    check(ok, listener.malformed_rx() == 0, "no prefix on this run was refused as malformed");

    std::printf("tcp: %zu frames reassembled from 4 writes, 4 bytes of framing each\n",
                at_listener.count());
    return ok ? 0 : 1;
}
