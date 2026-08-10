/**
 * @file
 * @brief #55 (increment 2) — REAL-bus smoke test for transport_can over Linux SocketCAN.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * It drives two transport_can instances over an actual `vcan0` virtual CAN device
 * via socketcan_link_t (the genuine PF_CAN / SOCK_RAW path) and asserts a byte-exact
 * frame round-trips each way.
 *
 * It also carries the #931 SEAM-RULE vectors, which need a real socket to exist at
 * all: an RTR frame and an 11-bit standard frame injected by a bare CAN_RAW
 * adversary must never reach the link's receiver, and an over-length classic frame
 * handed to write_raw must be dropped rather than memcpy'd into the 8-byte kernel
 * struct behind it.
 *
 * This needs the kernel `vcan` module + a `vcan0` link, which an unprivileged
 * container cannot load — so the test SELF-SKIPS (clean exit 0) when the socket
 * cannot bind (no vcan). The existing required CI gates therefore never depend on
 * kernel CAN; the dedicated can-vcan-e2e workflow sets vcan0 up so the socket path
 * is genuinely exercised. CAN_RAW sockets do not receive their own sent frames
 * (CAN_RAW_RECV_OWN_MSGS defaults off) but other sockets on the same interface do,
 * so A→B and B→A are cleanly separable without self-delivery.
 *
 * Linux-only by construction (it speaks `<linux/can.h>` directly); the build system
 * — not an in-source #ifdef — is what keeps it off other platforms.
 */

#include <linux/can.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "libtracer/transport_can.hpp"
#include "libtracer/view_can.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;

using tr::testing::check;

class sink_t {
   public:
    void on(std::span<const std::byte> f) {
        const std::lock_guard lock(m_);
        last_.assign(f.begin(), f.end());
        ++count_;
        cv_.notify_all();
    }
    bool wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        std::unique_lock lock(m_);
        return cv_.wait_for(lock, budget, [&] { return count_ >= n; });
    }
    std::vector<std::byte> last() {
        const std::lock_guard lock(m_);
        return last_;
    }

   private:
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::byte> last_;
    std::size_t count_ = 0;
};

std::vector<std::byte> payload(std::size_t n, std::uint8_t seed) {
    std::vector<std::byte> v(n);
    for (std::size_t i = 0; i < n; ++i) v[i] = static_cast<std::byte>((i * 5 + seed) & 0xFF);
    return v;
}

bool equal_bytes(const std::vector<std::byte>& a, const std::vector<std::byte>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

/**
 * @brief A bare `CAN_RAW` socket on the bus — the adversary the seam must survive.
 *
 * `socketcan_link_t` can only emit frames the seam already calls well-formed, so
 * the #931 ingress vectors (a remote-transmission request, an 11-bit standard
 * frame) have to be injected by something that is NOT a link. The same socket then
 * observes the bus to see what the link's egress path actually put on it.
 */
class raw_can_socket_t {
   public:
    /** @brief Open + bind a `CAN_RAW` socket on @p ifname with a bounded read window. */
    explicit raw_can_socket_t(const char* ifname) {
        fd_ = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
        if (fd_ < 0) return;
        ifreq ifr{};
        std::strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        if (::ioctl(fd_, SIOCGIFINDEX, &ifr) < 0) {
            close_();
            return;
        }
        sockaddr_can addr{};
        addr.can_family = AF_CAN;
        addr.can_ifindex = ifr.ifr_ifindex;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            close_();
            return;
        }
        // Bounded, so a "the bus stayed quiet" read terminates instead of hanging.
        timeval tv{.tv_sec = 0, .tv_usec = 300000};  // 300 ms
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    ~raw_can_socket_t() { close_(); }

    raw_can_socket_t(const raw_can_socket_t&) = delete;
    raw_can_socket_t& operator=(const raw_can_socket_t&) = delete;

    /** @brief True if the socket opened and bound. */
    [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

    /**
     * @brief Put one classic frame on the bus VERBATIM — flags included, nothing
     *        normalized, which is the whole point of not going through the link.
     */
    bool emit(std::uint32_t can_id, std::uint8_t dlc) {
        can_frame f{};
        f.can_id = can_id;
        f.can_dlc = dlc;
        const std::size_t n = std::min<std::size_t>(dlc, sizeof(f.data));
        for (std::size_t i = 0; i < n; ++i) f.data[i] = static_cast<std::uint8_t>(0xA0 + i);
        return ::write(fd_, &f, sizeof(f)) == static_cast<ssize_t>(sizeof(f));
    }

    /** @brief The next frame on the bus, or `std::nullopt` when the window expires. */
    std::optional<can_frame> recv() {
        can_frame f{};
        if (::read(fd_, &f, sizeof(f)) != static_cast<ssize_t>(sizeof(f))) return std::nullopt;
        return f;
    }

   private:
    void close_() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    int fd_ = -1;
};

/** @brief A counting sink for raw seam frames — what @ref raw_can_socket_t injected. */
class frame_sink_t {
   public:
    void on(const tr::net::can_frame_data_t& f) {
        const std::lock_guard lock(m_);
        seen_.push_back(f);
    }
    /** @brief A snapshot of the frames admitted so far. */
    std::vector<tr::net::can_frame_data_t> snapshot() {
        const std::lock_guard lock(m_);
        return seen_;
    }

   private:
    std::mutex m_;
    std::vector<tr::net::can_frame_data_t> seen_;
};

}  // namespace

int main() {
    std::printf("transport_can REAL vcan0 smoke test:\n");

    // Probe the bus; self-skip cleanly if vcan0 is unavailable.
    auto probe = std::make_unique<tr::net::socketcan_link_t>("vcan0");
    if (!probe->ok()) {
        std::printf(
            "  [SKIP] vcan0 unavailable (no kernel CAN here) — covered by can-vcan-e2e CI\n");
        return 0;
    }
    probe.reset();

    auto link_a = std::make_unique<tr::net::socketcan_link_t>("vcan0");
    auto link_b = std::make_unique<tr::net::socketcan_link_t>("vcan0");
    check(link_a->ok() && link_b->ok(), "two CAN_RAW sockets bound to vcan0");

    // Sinks + named receiver lambdas BEFORE the transports: the slot binds the
    // callable by address, and ~transport_can joins its receive thread.
    sink_t sink_a, sink_b;
    auto rx_a = [&](std::span<const std::byte> f) { sink_a.on(f); };
    auto rx_b = [&](std::span<const std::byte> f) { sink_b.on(f); };
    tr::net::transport_can tx_a(std::move(link_a),
                                {0, 1, tr::view::can_frame_mode_t::CLASSIC, "a/p"});
    tr::net::transport_can tx_b(std::move(link_b),
                                {0, 2, tr::view::can_frame_mode_t::CLASSIC, "b/q"});

    tx_a.set_receiver(rx_a);
    tx_b.set_receiver(rx_b);

    // A -> B (a 20-byte payload spans 3 classic CAN data frames).
    const std::vector<std::byte> a2b = payload(20, 0x11);
    tx_a.send(a2b);
    const bool got_b = sink_b.wait_for_count(1, 3s);
    check(got_b, "B received A's frame over vcan0");
    if (got_b) check(equal_bytes(sink_b.last(), a2b), "A->B bytes byte-exact over the real bus");

    // B -> A.
    const std::vector<std::byte> b2a = payload(14, 0x44);
    tx_b.send(b2a);
    const bool got_a = sink_a.wait_for_count(1, 3s);
    check(got_a, "A received B's frame over vcan0");
    if (got_a) check(equal_bytes(sink_a.last(), b2a), "B->A bytes byte-exact over the real bus");

    // ADR-0044: stateless peer enumeration over the REAL bus. Both nodes have
    // spoken (join hello + the round trips above), so each is audible to the other.
    const auto peers_of = [](tr::net::transport_can& t) {
        std::vector<std::string> names;
        t.enumerate_peers([&](std::string_view p) { names.emplace_back(p); });
        return names;
    };
    check(peers_of(tx_a) == std::vector<std::string>{"n2"}, "A enumerates exactly peer n2");
    check(peers_of(tx_b) == std::vector<std::string>{"n1"}, "B enumerates exactly peer n1");

    // ADR-0044: directed per-peer send over the real bus. C (node 3) joins the same
    // vcan0; a frame A sends via its n2 peer endpoint reaches B and NOT C.
    auto link_c = std::make_unique<tr::net::socketcan_link_t>("vcan0");
    check(link_c->ok(), "third CAN_RAW socket bound to vcan0");
    sink_t sink_c;
    auto rx_c = [&](std::span<const std::byte> f) { sink_c.on(f); };
    tr::net::transport_can tx_c(std::move(link_c),
                                {0, 3, tr::view::can_frame_mode_t::CLASSIC, "c/r"});
    tx_c.set_receiver(rx_c);

    tr::net::transport_t* const to_b = tx_a.peer_link("n2");
    check(to_b != nullptr, "A resolves peer n2 to a directed endpoint");
    const std::vector<std::byte> directed = payload(18, 0x77);
    if (to_b != nullptr) to_b->send(directed);
    const bool got_directed = sink_b.wait_for_count(2, 3s);
    check(got_directed, "B received the DIRECTED frame over vcan0");
    if (got_directed) {
        check(equal_bytes(sink_b.last(), directed), "directed bytes byte-exact");
    }
    check(!sink_c.wait_for_count(1, 300ms),
          "bystander C delivered nothing (directed group skips non-addressed peers)");

    // ------------------------------------------------------------------------
    // #931 — the can_link_t seam's INGRESS rule over the real bus.
    //
    // A CAN_RAW socket carries no filter by default, so remote-request and 11-bit
    // standard frames arrive alongside the extended data frames the binding
    // actually speaks. socketcan_link_t masks the id down to 29 bits on the way
    // out, which destroys the evidence — so an admitted RTR frame would reach the
    // reassembler as a data slice whose DLC promises bytes it never carried.
    // ------------------------------------------------------------------------
    raw_can_socket_t adversary("vcan0");
    check(adversary.ok(), "bare CAN_RAW adversary socket bound to vcan0");

    frame_sink_t admitted;
    auto guard_link = std::make_unique<tr::net::socketcan_link_t>("vcan0");
    check(guard_link->ok(), "seam-rule link bound to vcan0");
    guard_link->on_receive([&](const tr::net::can_frame_data_t& f) { admitted.on(f); });

    constexpr std::uint32_t kRemoteId = 0x1FEED1u;
    constexpr std::uint32_t kStandardId = 0x123u;  // 11-bit, no CAN_EFF_FLAG
    constexpr std::uint32_t kDataId = 0x1ABCDEu;
    check(adversary.emit(CAN_EFF_FLAG | CAN_RTR_FLAG | kRemoteId, 8), "injected an RTR frame");
    check(adversary.emit(kStandardId, 8), "injected an 11-bit standard frame");
    check(adversary.emit(CAN_EFF_FLAG | kDataId, 8), "injected the admissible control frame");

    // vcan delivers in send order, so once the control frame lands the two ahead of
    // it have already been offered; the extra settle is insurance against a
    // descheduled receive thread, not a race the assertion depends on.
    std::vector<tr::net::can_frame_data_t> got;
    for (int i = 0; i < 60 && got.empty(); ++i) {
        std::this_thread::sleep_for(50ms);
        got = admitted.snapshot();
    }
    std::this_thread::sleep_for(200ms);
    got = admitted.snapshot();
    check(got.size() == 1, "exactly one of the three injected frames crossed the seam");
    check(got.size() == 1 && got[0].id == kDataId,
          "the one admitted frame is the 29-bit DATA frame (RTR + standard refused)");

    // ------------------------------------------------------------------------
    // #931 — the seam's EGRESS rule over the real bus.
    //
    // A classic frame declaring more bytes than classic CAN carries must be dropped
    // before write_raw's memcpy reaches the 8-byte kernel `struct can_frame` on its
    // stack. The kernel would refuse the oversized write too, so the BUS cannot
    // distinguish a guarded link from an unguarded one — the smash is what differs,
    // and ASan is the instrument that sees it (this vector is why the sanitizer
    // build must be able to run against a live vcan0). What the bus CAN witness is
    // the drop-vs-clamp choice: a clamping link would put a truncated frame out
    // first, so asserting the legal frame is the NEXT thing on the wire pins the
    // policy this seam actually chose.
    //
    // For that assertion to discriminate, the clamped form of the over-length frame
    // must be WITNESSABLY different from the legal one. Two zero-payload frames on
    // the same id are wire-identical once one is truncated to 8 bytes, so a clamping
    // link would satisfy a naive "first frame has dlc 8" check and the guard would
    // pin nothing. The two frames therefore carry distinct ids and distinct payloads.
    // ------------------------------------------------------------------------
    raw_can_socket_t observer("vcan0");
    check(observer.ok(), "observer CAN_RAW socket bound to vcan0");
    auto tx_link = std::make_unique<tr::net::socketcan_link_t>("vcan0");
    check(tx_link->ok(), "egress-vector link bound to vcan0");

    constexpr std::uint32_t kOverId = 0x1BADBEu;
    constexpr std::uint32_t kLegalId = 0x0C0FFEu;
    tr::net::can_frame_data_t over;
    over.id = kOverId;
    over.fd = false;
    over.len = 20;  // legal for the 64-byte carrier, impossible for classic CAN
    over.data.fill(std::byte{0xAB});
    tx_link->write_raw(over);

    tr::net::can_frame_data_t legal;
    legal.id = kLegalId;
    legal.fd = false;
    legal.len = tr::net::can_max_len(/*fd=*/false);
    legal.data.fill(std::byte{0x5C});
    tx_link->write_raw(legal);

    const auto witnessed = observer.recv();
    check(witnessed.has_value() && (witnessed->can_id & CAN_EFF_MASK) == kLegalId,
          "the over-length classic frame was DROPPED, not clamped — the bus's first "
          "sight of this link is the legal frame, not a truncated one on the over id");
    check(witnessed.has_value() && witnessed->can_dlc == tr::net::can_max_len(/*fd=*/false) &&
              witnessed->data[0] == 0x5Cu,
          "and it is the legal frame's own payload, not the over-length frame's first 8 bytes");

    return tr::testing::summary("transport_can_vcan");
}
