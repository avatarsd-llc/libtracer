/**
 * @file
 * @brief M5 UDP transport tests: raw frame delivery over a real localhost UDP socket, and an end-
 *        to-end two-node FWD delivery through graph_t + fwd_router_t over UDP (the explicit-source-
 *        routed net plane, ADR-0040 — no bridge_t/ROUTER).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Built
 * under TSan (the recv thread + receiver handoff) and ASan+UBSan.
 *
 * Every socket here binds port 0 (#1362). No port number is reserved for tests: the old fixed
 * 47xxx literals sit INSIDE the kernel's default `net.ipv4.ip_local_port_range` (32768-60999), so
 * the ephemeral allocator can hand the very same port to an unrelated client socket and bind(2)
 * then fails with EADDRINUSE. Binding 0 and reading the granted port back with `local_port()`
 * removes the contention instead of narrowing the window: the receiver is constructed first in
 * LEARN-PEER mode (empty peer host), and the sender is then aimed at the port the kernel granted.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/mem_pool.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;

using tr::testing::check;

/**
 * @brief Poll until @p pred holds or @p budget expires — every counter these tests read is
 *        written by the transport's own recv thread.
 */
template <typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return pred();
}

std::vector<std::byte> value_tlv(std::initializer_list<std::uint8_t> bytes) {
    std::vector<std::byte> payload;
    for (std::uint8_t b : bytes) payload.push_back(std::byte{b});
    tr::wire::tlv_t t{.type = tr::wire::type_t::VALUE, .payload = payload};
    return tr::wire::encode(t);
}

void test_raw_frame() {
    std::printf("UDP transport — raw frame over localhost:\n");
    // The promise + named receiver lambda live BEFORE the transports: the slot
    // binds the callable by address, and ~udp_transport_t joins the recv thread.
    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    auto rx = [&](std::span<const std::byte> f) {
        got.set_value(std::vector<std::byte>(f.begin(), f.end()));
    };
    // Receiver first (ephemeral bind, learn-peer), then the sender aimed at the granted port.
    tr::net::udp_transport_t b(0, "", 0);
    tr::net::udp_transport_t a(0, "127.0.0.1", b.local_port());
    check(a.ok() && b.ok(), "both UDP sockets bound");

    b.set_receiver(rx);

    const std::array<std::byte, 5> frame{std::byte{0x09}, std::byte{0xAB}, std::byte{0xCD},
                                         std::byte{0xEF}, std::byte{0x42}};
    a.send(frame);

    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "frame received on the peer socket");
    if (arrived) {
        const auto r = fut.get();
        check(r.size() == frame.size() && std::memcmp(r.data(), frame.data(), frame.size()) == 0,
              "received bytes are identical");
    }
}

/**
 * @brief Build FWD{ op=WRITE, dst=<segs...>, src=<empty PATH>, payload=<VALUE> } — a remote write
 *        routed by explicit source route (RFC-0004 §D, ADR-0040).
 */
std::vector<std::byte> fwd_write(std::initializer_list<std::string_view> dst,
                                 std::span<const std::byte> payload_value_tlv) {
    return tr::testing::b_fwd(tr::graph::fwd_op_t::WRITE, tr::testing::b_path(dst),
                              tr::testing::b_path(std::initializer_list<std::string_view>{}), {},
                              payload_value_tlv);
}

void test_two_nodes_over_udp() {
    std::printf("Two nodes over UDP — FWD delivery through fwd_router_t (ADR-0040):\n");
    // Declaration order matters: the transports are declared AFTER the routers so they
    // destruct FIRST — ~udp_transport_t joins its recv thread, so no inbound frame can
    // reach a router's child_registry_t after the router is gone (ASan use-after-free).
    graph_t node_a, node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    // Within that constraint, tb (the receiving side) binds first so ta can aim at its port.
    tr::net::udp_transport_t tb(0, "", 0);
    tr::net::udp_transport_t ta(0, "127.0.0.1", tb.local_port());

    // B holds the target vertex and a subscriber; A knows the link to B as "b".
    (void)node_b.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)router_a.add_child("b", ta);  // A routes a `dst` starting with "b" out over UDP to B
    (void)router_b.add_child("a", tb);  // B's name for the inbound link (src accumulation)

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    auto on_temp = [&got](const tr::graph::value_t& v) {
        const auto b = v.only().bytes();
        got.set_value(std::vector<std::byte>(b.begin(), b.end()));
    };
    (void)node_b.subscribe(path_t("/sensor/temp"), on_temp);

    // A client FWD{WRITE dst=/b/sensor/temp} handed to A's router: A strips "b" and
    // forwards /sensor/temp over real UDP to B, whose terminus writes it locally.
    const auto payload = value_tlv({0x2A, 0x2B});
    const auto frame = fwd_write({"b", "sensor", "temp"}, payload);
    router_a.on_frame("client", frame);

    const bool arrived = fut.wait_for(3s) == std::future_status::ready;
    check(arrived, "node B receives the FWD-delivered value over real UDP");
    if (arrived) {
        const auto r = fut.get();
        check(r.size() == payload.size() && std::memcmp(r.data(), payload.data(), r.size()) == 0,
              "delivered TLV bytes match across the wire (explicit source route)");
    }
}

void test_scatter_gather() {
    std::printf("UDP transport — scatter-gather send (rope -> one datagram, no flatten):\n");
    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    auto rx = [&](std::span<const std::byte> f) {
        got.set_value(std::vector<std::byte>(f.begin(), f.end()));
    };
    tr::net::udp_transport_t b(0, "", 0);
    tr::net::udp_transport_t a(0, "127.0.0.1", b.local_port());
    check(a.ok() && b.ok(), "both UDP sockets bound");

    b.set_receiver(rx);

    // A 3-segment rope (the "rope we put into tx"), sent via one sendmsg(iovec).
    const std::array<std::byte, 2> s0{std::byte{0x01}, std::byte{0x02}};
    const std::array<std::byte, 3> s1{std::byte{0x03}, std::byte{0x04}, std::byte{0x05}};
    const std::array<std::byte, 1> s2{std::byte{0x06}};
    const std::array<std::span<const std::byte>, 3> iov{std::span<const std::byte>(s0),
                                                        std::span<const std::byte>(s1),
                                                        std::span<const std::byte>(s2)};
    a.send(std::span<const std::span<const std::byte>>(iov));

    const std::array<std::byte, 6> expect{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                          std::byte{0x04}, std::byte{0x05}, std::byte{0x06}};
    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "scatter-gather frame received");
    if (arrived) {
        const auto r = fut.get();
        check(r.size() == 6 && std::memcmp(r.data(), expect.data(), 6) == 0,
              "gathered segments arrive concatenated as one datagram");
    }
}

/**
 * @brief A heap-delegating backend that RECORDS every segment it hands out, so a test can prove
 *        segment identity end to end (the RX frame segment IS the stored segment).
 *
 * destroy is delegated too, though the heap's alloc stamps itself as the segment's
 * reclaimer, so this override is exercised only if that ever changes.
 */
class recording_backend_t final : public tr::mem::mem_backend_t {
   public:
    recording_backend_t() : mem_backend_t("rec_heap") {}

    tr::view::segment_t* alloc(std::size_t size,
                               tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        tr::view::segment_t* const seg = tr::mem::heap_backend().alloc(size, hint);
        if (seg != nullptr) {
            const std::lock_guard lock(m_);
            segments_.push_back(seg);
        }
        return seg;
    }
    void destroy(tr::view::segment_t* seg) noexcept override {
        tr::mem::heap_backend().destroy(seg);
    }

    [[nodiscard]] std::vector<tr::view::segment_t*> segments() const {
        const std::lock_guard lock(m_);
        return segments_;
    }

   private:
    mutable std::mutex m_;
    std::vector<tr::view::segment_t*> segments_;
};

/**
 * @brief ADR-0042 §2 — the owning delivery path: an installed view receiver gets each datagram as a
 *        view over a fresh refcounted segment from the injected backend.
 */
void test_view_delivery() {
    std::printf("UDP transport — owning view delivery (ADR-0042 receiver seam):\n");
    std::promise<tr::view::view_t> got;
    auto fut = got.get_future();
    // The receiver must release its OWN references BEFORE it unblocks the waiter (#845):
    // set_value wakes the main thread immediately, so a rope still holding the link is a
    // live second reference the use_count() assertion below can observe. Steal the link,
    // clear the rope, and only then signal — leaving exactly the receiver's reference.
    auto rope_rx = [&](tr::view::rope_t f) {
        if (f.link_count() != 1) return;  // single-link: the trivial rope
        tr::view::view_t v = f.only();    // +1: the rope and v now share the segment
        f = tr::view::rope_t{};           // -1: the rope's link is gone before the wake
        got.set_value(std::move(v));      // hand the sole reference to the waiter
    };
    tr::net::udp_transport_t b(0, "", 0);
    tr::net::udp_transport_t a(0, "127.0.0.1", b.local_port());
    check(a.ok() && b.ok(), "both UDP sockets bound");
    check(b.delivers_ropes(), "udp_transport_t::delivers_ropes() is true");

    b.set_rope_receiver(rope_rx);

    const std::array<std::byte, 5> frame{std::byte{0x09}, std::byte{0xAB}, std::byte{0xCD},
                                         std::byte{0xEF}, std::byte{0x42}};
    a.send(frame);

    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "owning frame received on the peer socket");
    if (arrived) {
        const tr::view::view_t v = fut.get();
        check(static_cast<bool>(v.owner), "frame view OWNS a refcounted segment");
        const auto bytes = v.bytes();
        check(bytes.size() == frame.size() &&
                  std::memcmp(bytes.data(), frame.data(), frame.size()) == 0,
              "owning frame bytes are identical (narrowed to the datagram length)");
        check(v.owner && v.owner->bytes.size() == tr::net::udp_transport_t::kMaxDatagram,
              "frame segment was allocated at the transport's max datagram size");
        check(v.owner.use_count() == 1, "the receiver holds the ONLY reference (no library copy)");
    }
    check(b.dropped_rx() == 0, "no backpressure drops on the heap backend");
}

/**
 * @brief ADR-0042 §2 — pool exhaustion is backpressure: alloc == nullptr drops the datagram and
 *        ticks dropped_rx(); never an OOM, never a crash.
 */
void test_view_pool_exhaustion() {
    std::printf("UDP transport — exhausted pool backend drops cleanly (backpressure):\n");
    // A slab far too small for even one kMaxDatagram slot => capacity 0 => every
    // alloc returns nullptr (the deterministic bounded-host exhaustion shape).
    std::array<std::byte, 256> slab;
    tr::mem::pool_t pool(slab, tr::net::udp_transport_t::kMaxDatagram);
    check(pool.capacity() == 0, "pool carves no kMaxDatagram slot from a 256-byte slab");

    std::atomic<int> delivered{0};
    auto rope_rx = [&](tr::view::rope_t) { delivered.fetch_add(1); };
    tr::net::udp_transport_t b(0, "", 0, {.memory = {.rx = &pool}});
    tr::net::udp_transport_t a(0, "127.0.0.1", b.local_port());
    check(a.ok() && b.ok(), "both UDP sockets bound");

    b.set_rope_receiver(rope_rx);

    const std::array<std::byte, 4> frame{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                         std::byte{0x04}};
    a.send(frame);
    a.send(frame);

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (b.dropped_rx() < 2 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    check(b.dropped_rx() >= 2, "both datagrams counted as backpressure drops");
    check(delivered.load() == 0, "nothing delivered while the pool is exhausted");
}

/**
 * @brief #926 — the universal `:settings max_frame` key bounds a UDP connection's ingress.
 *
 * `max_frame` is parsed centrally into `conn_settings_t` for every kind, and tcp / quic /
 * webtransport / ws all honor it. `udp` used to have no constructor slot for it at all, so an
 * operator who set it on a udp connection got no cap and no error. Three things are asserted
 * here, because a cap that only exists in an accessor is not a cap:
 *
 *  - the accessor reports the CONFIGURED value, and a value above `kMaxDatagram` is inert
 *    (a datagram cannot be larger, so this key can only tighten on this kind);
 *  - an over-cap datagram is REFUSED on both the owning and the borrowed receive path —
 *    never delivered, counted in `malformed_rx()`, and the socket still serves the next one;
 *  - the RX segment is drawn at the cap, not at `kMaxDatagram` — so a tight cap is a RAM
 *    lever on a bounded node, not only an admission rule.
 *
 * #934 adds the BOUNDARY's other side on both paths: a datagram of EXACTLY the cap is the
 * largest legal one and lands. Refusing only `kCap + 1` is satisfied by an off-by-one
 * `n >= frame_cap` too, which would silently cost a conforming peer its largest frame.
 */
void test_settings_max_frame() {
    std::printf("UDP transport — a :settings max_frame bounds the accepted datagram (#926):\n");
    constexpr std::size_t kCap = 1024;
    constexpr std::size_t kMax = tr::net::udp_transport_t::kMaxDatagram;

    // ----- the accessor: the honored cap is the configured one, and it only tightens. -----
    {
        const tr::net::udp_transport_t plain(0, "127.0.0.1", 1);
        check(plain.effective_max_frame() == kMax, "unset max_frame keeps the kMaxDatagram cap");
        const tr::net::udp_transport_t tight(0, "127.0.0.1", 1, {.max_frame = kCap});
        check(tight.effective_max_frame() == kCap, "a configured max_frame IS the honored cap");
        const tr::net::udp_transport_t wide(0, "127.0.0.1", 1, {.max_frame = 8 * kMax});
        check(wide.effective_max_frame() == kMax,
              "a max_frame above the datagram ceiling is inert (it cannot raise the bound)");
    }

    // ----- the owning path: over-cap refused, under-cap delivered, segment sized to the cap. --
    std::mutex m;
    std::vector<std::size_t> lens;      // delivered frame lengths
    std::vector<std::size_t> seg_lens;  // and the segment each was received into
    auto rope_rx = [&](tr::view::rope_t f) {
        const tr::view::view_t v = f.only();
        const std::lock_guard lock(m);
        lens.push_back(v.bytes().size());
        seg_lens.push_back(v.owner ? v.owner->bytes.size() : 0);
    };
    tr::net::udp_transport_t b(0, "", 0, {.max_frame = kCap});
    tr::net::udp_transport_t a(0, "127.0.0.1", b.local_port());
    check(a.ok() && b.ok(), "both UDP sockets bound");
    b.set_rope_receiver(rope_rx);

    const std::uint64_t before = b.malformed_rx();
    const std::vector<std::byte> just_over(kCap + 1, std::byte{0xAA});
    a.send(std::span<const std::byte>(just_over));
    check(wait_until([&] { return b.malformed_rx() > before; }, 3s),
          "a datagram ONE BYTE over the cap is refused and counted (owning path)");
    const std::vector<std::byte> far_over(16u * 1024u, std::byte{0xBB});
    a.send(std::span<const std::byte>(far_over));
    check(wait_until([&] { return b.malformed_rx() > before + 1; }, 3s),
          "and so is a 16 KiB one (it is not merely truncated to the cap)");
    {
        const std::lock_guard lock(m);
        check(lens.empty(), "nothing over the cap reached the receiver");
    }
    check(b.dropped_rx() == 0, "a refusal is NOT counted as backend backpressure");

    const std::vector<std::byte> under(64, std::byte{0xCC});
    a.send(std::span<const std::byte>(under));
    bool landed = false;
    check(wait_until(
              [&] {
                  const std::lock_guard lock(m);
                  landed = !lens.empty();
                  return landed;
              },
              3s),
          "an under-cap datagram still lands — the socket survived the refusals");
    if (landed) {
        const std::lock_guard lock(m);
        check(lens.front() == 64, "and arrives whole");
        check(seg_lens.front() < kMax && seg_lens.front() <= kCap + 1,
              "the RX segment was drawn at the configured cap, not at kMaxDatagram");
    }

    // ----- the BOUNDARY, owning path (#934): a datagram of EXACTLY the cap is the largest
    //       legal one and must LAND, next to the kCap + 1 refusal above. Only the pair
    //       distinguishes the shipped `n > frame_cap` from an off-by-one `n >= frame_cap`,
    //       and this is the boundary the deliberate `alloc_cap = min(rx_cap, frame_cap + 1)`
    //       sizing exists to serve: a segment bounded at the cap itself would truncate the
    //       at-cap datagram before its length could be judged (#1074).
    const std::uint64_t malformed_before_boundary = b.malformed_rx();
    const std::vector<std::byte> exactly(kCap, std::byte{0xDD});
    a.send(std::span<const std::byte>(exactly));
    check(wait_until(
              [&] {
                  const std::lock_guard lock(m);
                  return lens.size() >= 2;
              },
              3s),
          "a datagram of EXACTLY the cap is delivered, not refused (owning path)");
    {
        const std::lock_guard lock(m);
        if (lens.size() >= 2) check(lens.back() == kCap, "and arrives whole, all kCap bytes of it");
    }
    check(b.malformed_rx() == malformed_before_boundary, "an at-cap datagram counts no refusal");
    check(b.dropped_rx() == 0, "and no backpressure drop");

    // ----- the borrowed path: no segment is involved, and the cap still holds. -----
    std::atomic<int> spans{0};
    std::atomic<std::size_t> last_span_len{0};
    auto span_rx = [&](std::span<const std::byte> f) {
        last_span_len.store(f.size(), std::memory_order_relaxed);
        spans.fetch_add(1);
    };
    tr::net::udp_transport_t d(0, "", 0, {.max_frame = kCap});
    tr::net::udp_transport_t c(0, "127.0.0.1", d.local_port());
    check(c.ok() && d.ok(), "the borrowed-path socket pair bound");
    d.set_receiver(span_rx);

    const std::uint64_t span_before = d.malformed_rx();
    c.send(std::span<const std::byte>(far_over));
    check(wait_until([&] { return d.malformed_rx() > span_before; }, 3s),
          "an over-cap datagram is refused on the borrowed path too");
    check(spans.load() == 0, "and nothing was handed to the span receiver");
    c.send(std::span<const std::byte>(under));
    check(wait_until([&] { return spans.load() > 0; }, 3s),
          "an under-cap datagram still reaches the span receiver");

    // ----- and the same boundary on the borrowed path (#934). -----
    const std::uint64_t span_malformed_at_cap = d.malformed_rx();
    c.send(std::span<const std::byte>(exactly));
    check(wait_until([&] { return spans.load() > 1; }, 3s),
          "a datagram of EXACTLY the cap reaches the span receiver too (borrowed path)");
    check(last_span_len.load(std::memory_order_relaxed) == kCap,
          "and the borrowed span is the whole kCap bytes, not one short");
    check(d.malformed_rx() == span_malformed_at_cap,
          "the at-cap datagram counted no refusal on the borrowed path either");

    // The refusal side, restated one byte over on this path: the pair, not the far-over
    // datagram alone, is what an off-by-one would fail.
    c.send(std::span<const std::byte>(just_over));
    check(wait_until([&] { return d.malformed_rx() > span_malformed_at_cap; }, 3s),
          "and ONE byte over it is still refused (borrowed path)");
    check(spans.load() == 2, "nothing over the cap was handed to the span receiver");
}

/**
 * @brief An ephemeral bind owns its port: datagrams for it reach this socket (#2027).
 *
 * `bind_port` 0 used to set `SO_REUSEADDR` too, and on a UDP socket that lets the kernel's
 * ephemeral pick land on a port another reuse-enabled socket already holds. Unicast datagrams
 * for a shared port reach whichever socket bound LAST, so a receiver bound first got nothing,
 * about once in 6,000 binds with a few sockets open. Waiting for that collision would make a
 * flaky test, so this one makes it on purpose: a raw socket with `SO_REUSEADDR` binds the
 * receiver's port directly. A port-0 bind refuses it with `EADDRINUSE`; with the old option
 * the intruder bound, took every datagram, and the receiver below saw none.
 */
void test_ephemeral_bind_owns_its_port() {
    std::printf("UDP transport — an ephemeral bind owns its port (#2027):\n");
    std::atomic<int> got{0};
    auto rx = [&](std::span<const std::byte>) { got.fetch_add(1); };
    tr::net::udp_transport_t d(0, "", 0);  // the receiver binds FIRST
    check(d.ok(), "the receiver bound an ephemeral port");
    d.set_receiver(rx);

    const int intruder = ::socket(AF_INET, SOCK_DGRAM, 0);
    const int one = 1;
    ::setsockopt(intruder, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_ANY);
    at.sin_port = htons(d.local_port());
    const int rc = ::bind(intruder, reinterpret_cast<sockaddr*>(&at), sizeof(at));
    const int err = errno;
    check(rc != 0 && err == EADDRINUSE,
          "a reuse-enabled socket cannot share the port an ephemeral bind took");

    tr::net::udp_transport_t c(0, "127.0.0.1", d.local_port());  // the sender binds AFTER
    check(c.ok(), "the sender bound");
    const std::vector<std::byte> frame(32, std::byte{0x5A});
    for (int i = 0; i < 3; ++i) c.send(std::span<const std::byte>(frame));
    check(wait_until([&] { return got.load() == 3; }, 10s),
          "every datagram sent to the receiver's port reached the receiver");
    ::close(intruder);
}

/** @brief `shut_down()` frees the port at once and leaves a valid object a send cannot reach. */
void test_shut_down_frees_the_port() {
    std::printf("UDP transport — shut_down() releases the socket and keeps the object:\n");
    std::atomic<int> got{0};
    tr::net::udp_transport_t d(0, "", 0);
    check(d.ok(), "the receiver bound an ephemeral port");
    auto rx = [&](std::span<const std::byte>) { got.fetch_add(1); };
    d.set_receiver(rx);
    const std::uint16_t port = d.local_port();
    d.shut_down();
    d.shut_down();  // idempotent

    // The port is free: a plain socket (no SO_REUSEADDR) binds it while `d` still exists.
    const int fresh = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_ANY);
    at.sin_port = htons(port);
    check(::bind(fresh, reinterpret_cast<sockaddr*>(&at), sizeof(at)) == 0,
          "the shut-down link's port can be bound again before it is destroyed");
    // A send through the shut-down object goes nowhere, and nothing is delivered to it.
    const std::vector<std::byte> frame(32, std::byte{0x5A});
    d.send(std::span<const std::byte>(frame));
    const int s = ::socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in to = at;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)::sendto(s, frame.data(), frame.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    std::array<std::byte, 64> buf{};
    check(wait_until([&] { return ::recv(fresh, buf.data(), buf.size(), MSG_DONTWAIT) > 0; }, 10s),
          "a datagram to the port reaches the new socket, not the shut-down link");
    check(got.load() == 0, "and the shut-down link delivers nothing");
    ::close(s);
    ::close(fresh);
}

/**
 * @brief ADR-0042 end to end: two nodes over real UDP with owning view delivery and a share
 *        threshold the payload clears (RFC-0028 §5.3) — the WRITE lands ZERO-copy (the graph's
 *        stored segment IS the RX frame segment, proven by pointer identity through graph read).
 *
 * @section udp_share_hold What sharing holds here, and why the threshold is the consumer's
 *
 * `udp_transport_t` receives every datagram into a `kMaxDatagram` (65,536 B) segment and
 * delivers a length-`n` window over it, so sharing this 68-byte payload TLV holds 64 KB for as
 * long as it is the vertex's value. The retired ratio priced that and declined (it needed
 * K >= 964); the threshold prices only the payload, and the hold is the retention hazard the
 * consumer sizes against (RFC-0022 Amendment 2) — which is why the host default (4 KB) copies a
 * payload this small, and this test declares a threshold of 64 to keep the shared path covered.
 */
void test_two_nodes_zero_copy_store() {
    std::printf("Two nodes over UDP — view delivery + share threshold zero-copy WRITE:\n");
    recording_backend_t rec;
    graph_t node_a, node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    tr::net::udp_transport_t tb(0, "", 0, {.memory = {.rx = &rec}});
    tr::net::udp_transport_t ta(0, "127.0.0.1", tb.local_port());

    // B's target vertex carries its share threshold as an OWNER-side declaration (never a
    // remote write): 68 >= 64 => shared.
    tr::graph::vertex_handle_t v =
        node_b.register_vertex(path_t("/sensor/blob"), role_t::STORED_VALUE);
    (void)node_b.set_policy(v, {.share_threshold_bytes = 64});
    (void)router_a.add_child("b", ta);
    (void)router_b.add_child("a", tb);  // tb delivers views => the owning receiver is installed

    std::promise<void> written;
    auto fut = written.get_future();
    auto on_blob = [&written](const tr::graph::value_t&) { written.set_value(); };
    (void)node_b.subscribe(path_t("/sensor/blob"), on_blob);

    // A 64-byte payload => a 68-byte trailer-less VALUE TLV, at or above the threshold.
    std::vector<std::byte> pb(64);
    for (std::size_t i = 0; i < pb.size(); ++i) pb[i] = static_cast<std::byte>(i);
    std::vector<std::byte> payload;
    tr::wire::emit_tlv(payload, tr::wire::type_t::VALUE, tr::wire::opt_t{}, pb);
    const auto frame = fwd_write({"b", "sensor", "blob"}, payload);
    router_a.on_frame("client", frame);

    const bool arrived = fut.wait_for(3s) == std::future_status::ready;
    check(arrived, "node B stores the FWD{WRITE} delivered over real UDP");
    if (arrived) {
        const auto rd = node_b.read(v);
        check(rd.has_value() && (*rd)->only().bytes().size() == payload.size() &&
                  std::memcmp((*rd)->only().bytes().data(), payload.data(), payload.size()) == 0,
              "stored bytes equal the written payload TLV");
        const auto segs = rec.segments();
        check(!segs.empty(), "the RX backend allocated the frame segment");
        check(rd.has_value() && !segs.empty() && (*rd)->only().owner.get() == segs.front(),
              "stored segment IS the RX frame segment (zero-copy socket -> LKV)");
    }
}

/**
 * @brief #932 — a TX-side drop is COUNTED, and readable through the `transport_t` seam.
 *
 * A peer-less listener has nobody to send to, so every `send` sheds the frame. That
 * used to be a bare return: no counter moved, and a consumer holding only a
 * `transport_t*` could not observe any drop from any transport. Now `dropped_tx()`
 * ticks and `drop_stats()` carries it across the interface.
 */
void test_tx_drop_counted() {
    std::printf("tx drops are counted and visible through transport_t (#932):\n");
    // Listener (learn-peer) mode: an unresolvable peer host leaves the endpoint
    // unset, so nothing is addressable until a datagram teaches it a source.
    tr::net::udp_transport_t peerless(0, "", 0);
    check(peerless.ok(), "peer-less listener bound");
    check(peerless.dropped_tx() == 0, "a fresh link has shed nothing");

    const std::vector<std::byte> frame = value_tlv({0x01, 0x02});
    peerless.send(std::span<const std::byte>(frame));
    const std::span<const std::byte> one[1] = {frame};
    peerless.send(std::span<const std::span<const std::byte>>(one));
    check(peerless.dropped_tx() == 2, "both sends with no peer are counted, not silent");

    // The point of the hoist: a generic transport_t* holder sees the same number.
    tr::net::transport_t* generic = &peerless;
    const tr::net::transport_drop_stats_t s = generic->drop_stats();
    check(s.dropped_tx == 2, "drop_stats() carries dropped_tx across the interface");
    check(s.dropped_rx == 0 && s.malformed_rx == 0, "and the RX counters are still zero");
}

}  // namespace

/**
 * @brief #1783 — a bounded RX backend bounds the borrowed path's scratch too.
 *
 * The span path receives into a scratch drawn from `memory.state`. It used to be sized from
 * `max_frame` alone, so a link left at the default cap drew 64 KiB there even with a
 * 256-byte-slot pool as its backend, and a state store that cannot hold 64 KiB (an MCU's
 * static arena) refused it and dropped every datagram. It is now one byte past the backend's
 * slot: a 1 KiB state store serves it, a datagram that fits lands whole, and one longer than
 * the slot is refused (`malformed_rx`), never truncated.
 */
void test_span_scratch_bounded_by_backend() {
    std::printf("UDP transport — a bounded backend bounds the span scratch (#1783):\n");
    constexpr std::size_t kSlot = 256;
    alignas(std::max_align_t) static std::array<std::byte, 4096> slab;
    tr::mem::pool_t pool(slab, kSlot);
    // A state store that can never serve 64 KiB: a 1 KiB bump buffer over a refusing upstream.
    alignas(std::max_align_t) static std::array<std::byte, 1024> state_buf;
    tr::mem::bump_source_t state(state_buf, tr::mem::null_source());

    std::atomic<int> spans{0};
    std::atomic<std::size_t> last_len{0};
    auto span_rx = [&](std::span<const std::byte> f) {
        last_len.store(f.size(), std::memory_order_relaxed);
        spans.fetch_add(1);
    };
    tr::net::udp_transport_t b(0, "", 0, {.memory = {.rx = &pool, .state = &state}});
    tr::net::udp_transport_t a(0, "127.0.0.1", b.local_port());
    check(a.ok() && b.ok(), "both UDP sockets bound");
    b.set_receiver(span_rx);

    const std::vector<std::byte> fits(kSlot, std::byte{0x5A});
    a.send(std::span<const std::byte>(fits));
    check(wait_until([&] { return spans.load() > 0; }, 3s),
          "a slot-sized datagram reaches the span receiver from a 1 KiB state store");
    check(last_len.load(std::memory_order_relaxed) == kSlot, "and arrives whole");
    check(b.dropped_rx() == 0, "the scratch was not refused: no backpressure drop");

    const std::vector<std::byte> over(kSlot + 1, std::byte{0xA5});
    a.send(std::span<const std::byte>(over));
    check(wait_until([&] { return b.malformed_rx() > 0; }, 3s),
          "one byte past the slot is refused, counted in malformed_rx");
    check(spans.load() == 1, "and nothing truncated was handed to the span receiver");
}

int main() {
    test_raw_frame();
    test_two_nodes_over_udp();
    test_scatter_gather();
    test_view_delivery();
    test_view_pool_exhaustion();
    test_settings_max_frame();
    test_span_scratch_bounded_by_backend();
    test_ephemeral_bind_owns_its_port();
    test_shut_down_frees_the_port();
    test_two_nodes_zero_copy_store();
    test_tx_drop_counted();
    return tr::testing::summary("udp");
}
