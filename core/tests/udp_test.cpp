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
 * under TSan (the recv thread + receiver handoff) and ASan+UBSan. Uses fixed
 * loopback ports; SO_REUSEADDR is set on the sockets.
 */

#include <array>
#include <atomic>
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

#include "libtracer/mem_pool.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

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
    tr::net::udp_transport_t a(47100, "127.0.0.1", 47101);
    tr::net::udp_transport_t b(47101, "127.0.0.1", 47100);
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
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, tr::wire::type_t::VALUE, tr::wire::opt_t{},
                       std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst_segs;
    for (std::string_view s : dst) tr::wire::emit_name(dst_segs, s);
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{.pl = true}, dst_segs);
    tr::wire::emit_tlv(body, tr::wire::type_t::PATH, tr::wire::opt_t{.pl = true},
                       std::span<const std::byte>{});  // src: empty, grows per hop
    body.insert(body.end(), payload_value_tlv.begin(), payload_value_tlv.end());
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, tr::wire::type_t::FWD, tr::wire::opt_t{.pl = true}, body);
    return frame;
}

void test_two_nodes_over_udp() {
    std::printf("Two nodes over UDP — FWD delivery through fwd_router_t (ADR-0040):\n");
    // Declaration order matters: the transports are declared AFTER the routers so they
    // destruct FIRST — ~udp_transport_t joins its recv thread, so no inbound frame can
    // reach a router's child_registry_t after the router is gone (ASan use-after-free).
    graph_t node_a, node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    tr::net::udp_transport_t ta(47102, "127.0.0.1", 47103);
    tr::net::udp_transport_t tb(47103, "127.0.0.1", 47102);

    // B holds the target vertex and a subscriber; A knows the link to B as "b".
    (void)node_b.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    router_a.add_child("b", ta);  // A routes a `dst` starting with "b" out over UDP to B
    router_b.add_child("a", tb);  // B's name for the inbound link (src accumulation)

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    auto on_temp = [&got](const tr::view::rope_t& v) {
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
    tr::net::udp_transport_t a(47104, "127.0.0.1", 47105);
    tr::net::udp_transport_t b(47105, "127.0.0.1", 47104);
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
    tr::net::udp_transport_t a(47106, "127.0.0.1", 47107);
    tr::net::udp_transport_t b(47107, "127.0.0.1", 47106);
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
    tr::net::udp_transport_t a(47110, "127.0.0.1", 47111);
    tr::net::udp_transport_t b(47111, "127.0.0.1", 47110, &pool);
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
 */
void test_settings_max_frame() {
    std::printf("UDP transport — a :settings max_frame bounds the accepted datagram (#926):\n");
    constexpr std::size_t kCap = 1024;
    constexpr std::size_t kMax = tr::net::udp_transport_t::kMaxDatagram;

    // ----- the accessor: the honored cap is the configured one, and it only tightens. -----
    {
        const tr::net::udp_transport_t plain(0, "127.0.0.1", 1);
        check(plain.effective_max_frame() == kMax, "unset max_frame keeps the kMaxDatagram cap");
        const tr::net::udp_transport_t tight(0, "127.0.0.1", 1, &tr::mem::heap_backend(), kCap);
        check(tight.effective_max_frame() == kCap, "a configured max_frame IS the honored cap");
        const tr::net::udp_transport_t wide(0, "127.0.0.1", 1, &tr::mem::heap_backend(), 8 * kMax);
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
    tr::net::udp_transport_t a(47114, "127.0.0.1", 47115);
    tr::net::udp_transport_t b(47115, "127.0.0.1", 47114, &tr::mem::heap_backend(), kCap);
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

    // ----- the borrowed path: no segment is involved, and the cap still holds. -----
    std::atomic<int> spans{0};
    auto span_rx = [&spans](std::span<const std::byte>) { spans.fetch_add(1); };
    tr::net::udp_transport_t c(47116, "127.0.0.1", 47117);
    tr::net::udp_transport_t d(47117, "127.0.0.1", 47116, &tr::mem::heap_backend(), kCap);
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
}

/**
 * @brief ADR-0042 end to end: two nodes over real UDP with owning view delivery and an RFC-0022
 *        §3.D ratio that clears the pin predicate — the WRITE lands ZERO-copy (the graph's stored
 *        segment IS the RX frame segment, proven by pointer identity through graph read).
 *
 * @section udp_pin_ratio What K has to be here, and why that is the finding
 *
 * `udp_transport_t` receives every datagram into a `kMaxDatagram` (65,536 B) segment and
 * delivers a length-`n` window over it, so `segment_bytes` at the decision site is 65,536 whatever
 * the datagram's length. §3.D's predicate is `payload * K >= segment`, so this 68-byte payload
 * TLV needs **K >= 964** before it pins at all — and a 1 KB payload needs K >= 64.
 *
 * That is not an artefact of the test: it is the honest price of what pinning holds. The
 * predicate is doing exactly its job — declining to hold 64 KB for 68 bytes of value — and the
 * consequence is that §3.D on the reference UDP transport with a heap backend is a no-op at any
 * K a human would write down. The RAM lever that changes it is the RX backend's
 * `max_segment_size` (`test_view_pool_exhaustion`'s pool shape), not K.
 *
 * K is set to 1024 here so the pinned path stays covered; RFC-0022 §6's bench measures what a
 * defensible default is.
 */
void test_two_nodes_zero_copy_store() {
    std::printf("Two nodes over UDP — view delivery + RFC-0022 §3.D ratio zero-copy WRITE:\n");
    recording_backend_t rec;
    graph_t node_a, node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    tr::net::udp_transport_t ta(47112, "127.0.0.1", 47113);
    tr::net::udp_transport_t tb(47113, "127.0.0.1", 47112, &rec);

    // B's target vertex carries the §3.D ratio K as its OWNER-side declaration (RFC-0022
    // §3.B, never a remote write). 68 * 1024 >= 65,536 => pin; K = 8 (the old absolute
    // threshold's value) would copy, because 68 * 8 is 544 against a 64 KB segment.
    tr::graph::vertex_handle_t v =
        node_b.register_vertex(path_t("/sensor/blob"), role_t::STORED_VALUE);
    node_b.set_pin_payload_ratio(v, 1024);
    router_a.add_child("b", ta);
    router_b.add_child("a", tb);  // tb delivers views => the owning receiver is installed

    std::promise<void> written;
    auto fut = written.get_future();
    auto on_blob = [&written](const tr::view::rope_t&) { written.set_value(); };
    (void)node_b.subscribe(path_t("/sensor/blob"), on_blob);

    // A 64-byte payload => a 68-byte trailer-less VALUE TLV; 68 * K clears the 64 KB segment.
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

}  // namespace

int main() {
    test_raw_frame();
    test_two_nodes_over_udp();
    test_scatter_gather();
    test_view_delivery();
    test_view_pool_exhaustion();
    test_settings_max_frame();
    test_two_nodes_zero_copy_store();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
