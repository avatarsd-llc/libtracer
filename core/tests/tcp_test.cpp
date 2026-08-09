/**
 * @file
 * @brief M6 TCP transport tests: length-prefix framing over a real localhost TCP stream.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Proves the stream properties UDP never exercises — a frame split across many
 * small writes reassembles (partial reads), two frames coalesced into one write
 * split apart (stream boundaries honored), and an oversize prefix is rejected as
 * malformed. Plus the ADR-0042 owning-delivery segment identity, backpressure
 * drain (framing sync survives exhaustion), an end-to-end two-node FWD delivery
 * through graph_t + fwd_router_t over TCP (ADR-0040 explicit source routing), and
 * a config-created `kind=tcp` connection via a /net:children[] SPEC. Built under
 * TSan (the recv thread + receiver handoff) and ASan+UBSan. Listeners bind
 * ephemeral ports (local_port()) except the fixed-port config test.
 */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::tcp_transport_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::frame_sink_t;

/**
 * @brief A raw POSIX TCP client — the test's hand on the wire, so writes can be split and coalesced
 *        at will (a tcp_transport_t dialer would hide the boundaries).
 */
struct raw_client_t {
    int fd = -1;

    explicit raw_client_t(std::uint16_t port) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &peer.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) < 0) {
            ::close(fd);
            fd = -1;
        }
    }
    ~raw_client_t() {
        if (fd >= 0) ::close(fd);
    }
    void write(std::span<const std::byte> bytes) {
        std::size_t off = 0;
        while (off < bytes.size()) {
            const ssize_t n = ::send(fd, bytes.data() + off, bytes.size() - off, 0);
            if (n <= 0) return;
            off += static_cast<std::size_t>(n);
        }
    }
};

/** @brief One length-prefixed record: u32-LE len ++ payload (the M6 transport framing). */
std::vector<std::byte> record(std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    tr::detail::append_le(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::byte> test_frame(std::size_t len, std::uint8_t seed) {
    std::vector<std::byte> f(len);
    for (std::size_t i = 0; i < len; ++i) f[i] = static_cast<std::byte>(seed + i);
    return f;
}

/**
 * @brief Read up to @p want bytes off @p fd within @p budget — the raw client's eye on the
 *        wire, returning whatever arrived when the budget expires (so "nothing arrived" is an
 *        answer the caller can check, not a hang).
 */
std::vector<std::byte> read_within(int fd, std::size_t want, std::chrono::milliseconds budget) {
    std::vector<std::byte> got;
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (got.size() < want) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        pollfd p{fd, POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left.count())) <= 0) break;
        std::array<std::byte, 256> chunk;
        const std::size_t take = std::min(chunk.size(), want - got.size());
        const ssize_t n = ::recv(fd, chunk.data(), take, 0);
        if (n <= 0) break;
        got.insert(got.end(), chunk.begin(), chunk.begin() + n);
    }
    return got;
}

void test_raw_frame_duplex() {
    std::printf("TCP transport — raw frames both ways over localhost:\n");
    // Sinks + named receiver lambdas BEFORE the transports: the slot binds the
    // callable by address, and ~tcp_transport_t joins the recv thread, so the
    // callable must outlive the transport.
    frame_sink_t at_listener, at_dialer;
    auto listener_rx = [&](std::span<const std::byte> f) { at_listener.push(f); };
    auto dialer_rx = [&](std::span<const std::byte> f) { at_dialer.push(f); };
    tcp_transport_t listener(std::uint16_t{0});
    check(listener.ok(), "listener bound (ephemeral port)");
    tcp_transport_t dialer("127.0.0.1", listener.local_port());
    check(dialer.ok(), "dialer connected");

    listener.set_receiver(listener_rx);
    dialer.set_receiver(dialer_rx);

    const auto f1 = test_frame(5, 0x10);
    dialer.send(f1);
    check(at_listener.wait_for_count(1, 2000ms), "dialer->listener frame received");
    check(at_listener.count() == 1 && at_listener.at(0) == f1, "received bytes are identical");

    // The reply direction: the listener sends to its accepted peer.
    const auto f2 = test_frame(9, 0x40);
    listener.send(f2);
    check(at_dialer.wait_for_count(1, 2000ms), "listener->dialer frame received");
    check(at_dialer.count() == 1 && at_dialer.at(0) == f2, "reply bytes are identical");
}

void test_partial_and_coalesced() {
    std::printf("TCP transport — split writes reassemble, coalesced writes split:\n");
    frame_sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.push(f); };
    tcp_transport_t listener(std::uint16_t{0});
    listener.set_receiver(rx);

    raw_client_t client(listener.local_port());
    check(client.fd >= 0, "raw client connected");

    // One frame deliberately split across MANY small writes — the prefix arrives
    // byte by byte, the body in two chunks, with pauses so each lands in its own
    // TCP segment (partial-read reassembly across every boundary).
    const auto f1 = test_frame(64, 0x01);
    const auto r1 = record(f1);
    for (std::size_t i = 0; i < 4; ++i) {  // the prefix, one byte at a time
        client.write(std::span(r1).subspan(i, 1));
        std::this_thread::sleep_for(20ms);
    }
    client.write(std::span(r1).subspan(4, 10));  // body head...
    std::this_thread::sleep_for(20ms);
    client.write(std::span(r1).subspan(14));  // ...body tail
    check(sink.wait_for_count(1, 2000ms), "split frame reassembled into ONE delivery");
    check(sink.count() == 1 && sink.at(0) == f1, "reassembled bytes are identical");

    // Two complete frames coalesced into ONE write — the reader must honor the
    // record boundaries and deliver two frames with the right contents.
    const auto f2 = test_frame(7, 0x60);
    const auto f3 = test_frame(31, 0x90);
    std::vector<std::byte> both = record(f2);
    const auto r3 = record(f3);
    both.insert(both.end(), r3.begin(), r3.end());
    client.write(both);
    check(sink.wait_for_count(3, 2000ms), "coalesced write delivered as TWO frames");
    check(sink.count() == 3 && sink.at(1) == f2 && sink.at(2) == f3,
          "stream boundaries honored (both frames byte-identical)");
}

void test_oversize_prefix() {
    std::printf("TCP transport — an oversize length prefix is malformed:\n");
    std::atomic<int> delivered{0};
    auto rx = [&](std::span<const std::byte>) { delivered.fetch_add(1); };
    tcp_transport_t listener(std::uint16_t{0});
    listener.set_receiver(rx);

    raw_client_t client(listener.local_port());
    std::vector<std::byte> prefix;
    tr::detail::append_le(prefix, static_cast<std::uint32_t>(tcp_transport_t::kMaxFrame + 1));
    client.write(prefix);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (listener.malformed_rx() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    check(listener.malformed_rx() == 1, "the oversize prefix counted as malformed");
    check(delivered.load() == 0, "nothing delivered");

    // A desynced stream cannot be re-framed — the transport tears the connection
    // down, observable as EOF on the client side.
    std::array<std::byte, 1> b;
    const ssize_t n = ::recv(client.fd, b.data(), 1, 0);
    check(n == 0, "the connection was closed (EOF at the peer)");
}

/**
 * @brief A per-connection :settings max_frame tightens the receive cap below kMaxFrame: a prefix
 *        within the 16 MiB protocol ceiling but above the connection's cap is rejected as malformed
 *        (kMaxFrame→:settings; behavior-preserving default when 0).
 */
void test_settings_max_frame() {
    std::printf("TCP transport — a :settings max_frame tightens the receive cap:\n");
    std::atomic<int> delivered{0};
    auto rx = [&](std::span<const std::byte>) { delivered.fetch_add(1); };
    tcp_transport_t listener(std::uint16_t{0}, &tr::mem::heap_backend(), /*max_frame=*/64);
    listener.set_receiver(rx);

    raw_client_t client(listener.local_port());
    std::vector<std::byte> prefix;
    tr::detail::append_le(prefix, std::uint32_t{100});  // 100 > 64 (cap), yet 100 < kMaxFrame
    client.write(prefix);

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (listener.malformed_rx() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    check(listener.malformed_rx() == 1, "a frame above the :settings cap is malformed");
    check(delivered.load() == 0, "nothing delivered");
    std::array<std::byte, 1> b;
    check(::recv(client.fd, b.data(), 1, 0) == 0, "the connection was closed (EOF at the peer)");
}

/**
 * @brief A heap-delegating backend that RECORDS every segment it hands out (segment identity) and
 *        can FAIL its first `fail_first` allocations (backpressure).
 */
class recording_backend_t final : public tr::mem::mem_backend_t {
   public:
    explicit recording_backend_t(int fail_first = 0)
        : mem_backend_t("rec_heap"), fail_remaining_(fail_first) {}

    tr::view::segment_t* alloc(std::size_t size,
                               tr::mem::alloc_hint_t hint = tr::mem::alloc_hint_t::NONE) override {
        if (fail_remaining_.fetch_sub(1, std::memory_order_relaxed) > 0) return nullptr;
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
    std::atomic<int> fail_remaining_;
    mutable std::mutex m_;
    std::vector<tr::view::segment_t*> segments_;
};

/**
 * @brief ADR-0042 — the owning delivery path: an installed view receiver gets each frame as a view
 *        over a fresh refcounted segment from the injected backend, allocated at exactly the frame
 *        length (the stream reader knows `len` before it reads).
 */
void test_view_delivery_segment_identity() {
    std::printf("TCP transport — owning view delivery (ADR-0042 receiver seam):\n");
    recording_backend_t rec;
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
    tcp_transport_t listener(std::uint16_t{0}, &rec);
    check(listener.delivers_ropes(), "tcp_transport_t::delivers_ropes() is true");
    tcp_transport_t dialer("127.0.0.1", listener.local_port());

    listener.set_rope_receiver(rope_rx);

    const auto frame = test_frame(48, 0x21);
    dialer.send(frame);

    const bool arrived = fut.wait_for(2s) == std::future_status::ready;
    check(arrived, "owning frame received on the peer");
    if (arrived) {
        const tr::view::view_t v = fut.get();
        check(static_cast<bool>(v.owner), "frame view OWNS a refcounted segment");
        const auto bytes = v.bytes();
        check(bytes.size() == frame.size() &&
                  std::memcmp(bytes.data(), frame.data(), frame.size()) == 0,
              "owning frame bytes are identical");
        const auto segs = rec.segments();
        check(!segs.empty() && v.owner.get() == segs.front(),
              "the frame segment IS the backend's segment (read straight off the socket)");
        check(v.owner && v.owner->bytes.size() == frame.size(),
              "the segment was allocated at exactly the frame length");
        check(v.owner.use_count() == 1, "the receiver holds the ONLY reference (no library copy)");
    }
    check(listener.dropped_rx() == 0, "no backpressure drops on the heap backend");
}

/**
 * @brief ADR-0042 §2 backpressure on a stream: an exhausted backend drops the frame but DRAINS it
 *        off the stream, so framing sync survives and later frames deliver.
 */
void test_backpressure_drain() {
    std::printf("TCP transport — backend exhaustion drains the frame, sync survives:\n");
    recording_backend_t rec(2);  // the first two allocations fail
    frame_sink_t sink;
    auto rope_rx = [&](tr::view::rope_t f) { sink.push(f.links()[0].bytes()); };
    tcp_transport_t listener(std::uint16_t{0}, &rec);
    listener.set_rope_receiver(rope_rx);
    tcp_transport_t dialer("127.0.0.1", listener.local_port());

    const auto f1 = test_frame(8192, 0x01);  // bigger than the drain scratch (4 KiB)
    const auto f2 = test_frame(16, 0x50);
    const auto f3 = test_frame(24, 0xA0);
    dialer.send(f1);
    dialer.send(f2);
    dialer.send(f3);

    check(sink.wait_for_count(1, 3000ms), "the third frame still delivers after two drops");
    check(listener.dropped_rx() == 2, "both exhausted frames counted as backpressure drops");
    check(sink.count() == 1 && sink.at(0) == f3,
          "framing sync survived the drained frames (byte-identical delivery)");
    check(listener.malformed_rx() == 0, "no malformed prefixes — the stream never desynced");
}

void test_scatter_gather() {
    std::printf("TCP transport — scatter-gather send (rope -> one record, no flatten):\n");
    frame_sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.push(f); };
    tcp_transport_t listener(std::uint16_t{0});
    listener.set_receiver(rx);
    tcp_transport_t dialer("127.0.0.1", listener.local_port());

    // A 3-segment rope (the "rope we put into tx"), one writev with the prefix in
    // front — arriving as ONE length-prefixed frame.
    const std::array<std::byte, 2> s0{std::byte{0x01}, std::byte{0x02}};
    const std::array<std::byte, 3> s1{std::byte{0x03}, std::byte{0x04}, std::byte{0x05}};
    const std::array<std::byte, 1> s2{std::byte{0x06}};
    const std::array<std::span<const std::byte>, 3> iov{std::span<const std::byte>(s0),
                                                        std::span<const std::byte>(s1),
                                                        std::span<const std::byte>(s2)};
    dialer.send(std::span<const std::span<const std::byte>>(iov));

    const std::array<std::byte, 6> expect{std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
                                          std::byte{0x04}, std::byte{0x05}, std::byte{0x06}};
    check(sink.wait_for_count(1, 2000ms), "scatter-gather frame received");
    check(sink.count() == 1 && sink.at(0).size() == 6 &&
              std::memcmp(sink.at(0).data(), expect.data(), 6) == 0,
          "gathered segments arrive concatenated as one frame");
}

/**
 * @brief Build FWD{ op=WRITE, dst=<segs...>, src=<empty PATH>, payload=<VALUE> } — a remote write
 *        routed by explicit source route (RFC-0004 §D, ADR-0040).
 */
std::vector<std::byte> fwd_write(std::initializer_list<std::string_view> dst,
                                 std::span<const std::byte> payload_value_tlv) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::WRITE)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst_segs;
    for (std::string_view s : dst) tr::wire::emit_name(dst_segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, dst_segs);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true},
                       std::span<const std::byte>{});  // src: empty, grows per hop
    body.insert(body.end(), payload_value_tlv.begin(), payload_value_tlv.end());
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

/** @brief FWD{ op=READ, dst, src } — a remote read whose REPLY source-routes back. */
std::vector<std::byte> fwd_read(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::READ)};
    tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst_segs;
    for (std::string_view s : dst) tr::wire::emit_name(dst_segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, dst_segs);
    std::vector<std::byte> src_segs;
    for (std::string_view s : src) tr::wire::emit_name(src_segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, src_segs);
    std::vector<std::byte> frame;
    tr::wire::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

void test_two_nodes_over_tcp() {
    std::printf("Two nodes over TCP — FWD delivery through fwd_router_t (ADR-0040):\n");
    // Declaration order matters: the transports are declared AFTER the routers so
    // they destruct FIRST — ~tcp_transport_t joins its recv thread, so no inbound
    // frame can reach a router's child_registry_t after the router is gone.
    graph_t node_a, node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    tcp_transport_t tb(std::uint16_t{0});  // B listens
    tcp_transport_t ta("127.0.0.1", tb.local_port());

    // B holds the target vertex and a subscriber; A knows the link to B as "b".
    (void)node_b.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    router_a.add_child("b", ta);  // A routes a `dst` starting with "b" out over TCP to B
    router_b.add_child("a", tb);  // B's name for the inbound link (src accumulation)

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    auto on_temp = [&got](const tr::view::rope_t& v) {
        const auto b = v.only().bytes();
        got.set_value(std::vector<std::byte>(b.begin(), b.end()));
    };
    (void)node_b.subscribe(path_t("/sensor/temp"), on_temp);

    // A client FWD{WRITE dst=/b/sensor/temp} handed to A's router: A strips "b" and
    // forwards /sensor/temp over real TCP to B, whose terminus writes it locally.
    std::vector<std::byte> payload;
    const std::array<std::byte, 2> pv{std::byte{0x2A}, std::byte{0x2B}};
    tr::wire::emit_tlv(payload, type_t::VALUE, opt_t{}, pv);
    const auto frame = fwd_write({"b", "sensor", "temp"}, payload);
    router_a.on_frame("client", frame);

    const bool arrived = fut.wait_for(3s) == std::future_status::ready;
    check(arrived, "node B receives the FWD-delivered value over real TCP");
    if (arrived) {
        const auto r = fut.get();
        check(r.size() == payload.size() && std::memcmp(r.data(), payload.data(), r.size()) == 0,
              "delivered TLV bytes match across the wire (explicit source route)");
    }
}

view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief `kind = "tcp"` bound into the library's own SPEC builder (#902). */
view_t tcp_conn_spec(std::string_view type, std::string_view name, tr::net::conn_role_t role,
                     std::uint16_t port, std::string_view addr = {}) {
    return tr::net::conn_spec(type, name, role, port, "tcp", addr);
}

void test_config_constructed_tcp() {
    std::printf("Config-constructed sockets: two nodes over TCP from :children[] SPECs:\n");
    // No provide_link anywhere — both nodes' transports are CONSTRUCTED from the SPEC
    // config (`kind=tcp`) and OWNED by their connection vertices. Declaration order
    // matters: each transport_vertex_t (owning the sockets, hence the recv threads)
    // is declared AFTER the router it feeds, so it destructs FIRST.
    graph_t node_a;
    graph_t node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    tr::net::transport_vertex_t net_a(node_a, router_a);
    tr::net::transport_vertex_t net_b(node_b, router_b);
    // ADR-0073 §4 (declared-only): the application mints the module names — the library
    // registers none. This test-app adopts the built-ins' suggested names.
    (void)net_a.register_module(std::string(tr::net::kTcpClientSuggestedModule), "tcp",
                                tr::net::conn_role_t::DIAL);
    (void)net_b.register_module(std::string(tr::net::kTcpServerSuggestedModule), "tcp",
                                tr::net::conn_role_t::LISTEN);

    // A's reply sink is set BEFORE the sockets exist (configure before frames flow).
    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    router_a.on_reply(
        [](void* ctx, const tr::view::rope_t& reply) {
            try {
                const tr::view::view_t mat = reply.materialize();
                const auto b = mat.bytes();
                static_cast<std::promise<std::vector<std::byte>>*>(ctx)->set_value(
                    std::vector<std::byte>(b.begin(), b.end()));
            } catch (...) {
            }
        },
        &got);

    // Pick a fresh OS-assigned free localhost port for B's listener. A hardcoded port
    // races leftover sockets across parallel/repeated runs and flakes (#440); a
    // config-constructed listener requires an EXPLICIT port (the factory refuses
    // port 0), so reserve a real free port via a throwaway ephemeral listener and
    // release it (a listener that never accepts closes straight to CLOSED — no
    // TIME_WAIT — so the port is immediately re-bindable below).
    std::uint16_t port;
    {
        tcp_transport_t port_probe{std::uint16_t{0}};
        port = port_probe.local_port();
    }
    check(port != 0, "reserved a free localhost port for the listener");

    // B: a stored value at /temp and a tcp LISTENER on that free port.
    (void)node_b.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
    std::vector<std::byte> tv;
    const std::byte tb{0x2A};
    tr::wire::emit_tlv(tv, type_t::VALUE, opt_t{}, std::span<const std::byte>(&tb, 1));
    (void)node_b.write(path_t("/temp"), owned(tv));
    const auto wb =
        node_b.write(path_t("/net:children[]"),
                     tcp_conn_spec("listener", "a", tr::net::conn_role_t::LISTEN, port));
    check(wb.has_value(), "B: SPEC{listener, kind=tcp, port} constructs the bound socket");
    check(router_b.registry().by_name("net/tcp-server/a") != nullptr,
          "B: the socket is wired into the router");

    // A: a tcp CLIENT dialing B's port — a SYNCHRONOUS connect from config.
    const auto wa =
        node_a.write(path_t("/net:children[]"),
                     tcp_conn_spec("client", "b", tr::net::conn_role_t::DIAL, port, "127.0.0.1"));
    check(wa.has_value(), "A: SPEC{client, kind=tcp, addr, port} constructs the dialing socket");
    const auto* s = net_a.settings_of("net/tcp-client/b");
    check(s != nullptr && s->kind == "tcp" && s->addr == "127.0.0.1" && s->port == port,
          "A: the parsed :settings carry kind/addr/port");

    // End-to-end: FWD{READ dst=/b/temp} from A crosses A's config-created stream to
    // B's terminus, and the REPLY source-routes back over the same connection.
    router_a.on_frame("self", fwd_read({"net", "tcp-client", "b", "temp"}, {"reply-ep"}));
    const bool replied = fut.wait_for(3s) == std::future_status::ready;
    check(replied, "the READ reached B and the REPLY returned over the accepted peer");
    if (replied) {
        const std::vector<std::byte> reply_bytes = fut.get();  // owns; decode borrows
        const auto dec = tr::wire::decode(reply_bytes);
        bool has_value = false;
        if (dec && dec->type == type_t::FWD)
            for (const auto& c : dec->children)
                if (c.type == type_t::VALUE && c.payload.size() == 1 &&
                    c.payload[0] == std::byte{0x2A})
                    has_value = true;
        check(has_value, "the REPLY carries B's stored /temp value");
    }
}

/** @brief The peer-named sink (the bus_link_t shape — the ws_transport_test twin). */
struct peer_sink_t {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::vector<std::byte>>> frames;

    /** @brief The peer-named receiver callable (bound by address). */
    void operator()(std::string_view peer, std::span<const std::byte> f) {
        {
            const std::lock_guard lock(m);
            frames.emplace_back(std::string(peer), std::vector<std::byte>(f.begin(), f.end()));
        }
        cv.notify_all();
    }
    /** @brief True once at least @p n frames arrived before @p timeout. */
    bool wait_count(std::size_t n, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, timeout, [&] { return frames.size() >= n; });
    }
};

/**
 * @brief The multi-peer server: two concurrent dialers, peer-named inbound
 *        delivery through the bus_link_t facet, broadcast send, a DIRECTED
 *        peer_link send (span AND gathered iov) reaching exactly one peer, and
 *        live peer enumeration tracking a departure — the transport_ws_server
 *        #362 contract over raw length-prefix framing.
 */
void test_server_multi_peer_bus() {
    std::printf("TCP transport — multi-peer server (bus facet):\n");

    // Sinks BEFORE the transports (the file's destruction-order idiom).
    peer_sink_t srv_sink;
    frame_sink_t at_a, at_b;
    auto a_rx = [&](std::span<const std::byte> f) { at_a.push(f); };
    auto b_rx = [&](std::span<const std::byte> f) { at_b.push(f); };

    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/0,
                                         /*peer_named=*/true);
    check(server.ok(), "listen socket bound");
    const std::uint16_t port = server.local_port();
    check(server.bus() != nullptr, "peer_named server exposes the bus_link_t facet (ADR-0044)");
    server.bus()->set_peer_receiver(srv_sink);

    tcp_transport_t a("127.0.0.1", port);
    a.set_receiver(a_rx);
    std::optional<tcp_transport_t> b;
    b.emplace("127.0.0.1", port);
    b->set_receiver(b_rx);
    check(a.ok() && b->ok(), "TWO dialers connected concurrently (listen(fd,1) era over)");

    // --- inbound: each dialer's frame arrives tagged with a DISTINCT peer name ---
    const auto pa = test_frame(2, 0xA1);
    const auto pb = test_frame(2, 0xB1);
    a.send(pa);
    b->send(pb);
    check(srv_sink.wait_count(2, 2s), "server got both dialers' frames");
    std::string name_a;
    {
        const std::lock_guard lock(srv_sink.m);
        check(srv_sink.frames[0].first != srv_sink.frames[1].first,
              "the two deliveries carry two distinct peer names");
        for (const auto& [peer, bytes] : srv_sink.frames)
            if (bytes == pa) name_a = peer;
    }
    check(!name_a.empty(), "dialer a's frame is identifiable by its peer tag");

    // --- enumeration: both peers audible ---
    std::size_t n_peers = 0;
    server.bus()->enumerate_peers([&](std::string_view) { ++n_peers; });
    check(n_peers == 2, "enumerate_peers lists both open peers");

    // --- broadcast: the flat send() reaches every open peer ---
    const auto bc = test_frame(3, 0xCC);
    server.send(bc);
    check(at_a.wait_for_count(1, 2s) && at_b.wait_for_count(1, 2s),
          "flat server.send() broadcast to both dialers");

    // --- directed: peer_link(name)->send() reaches exactly that peer ---
    tr::net::transport_t* const link_a = server.bus()->peer_link(name_a);
    check(link_a != nullptr, "peer_link resolves dialer a's name");
    const auto da = test_frame(2, 0xDA);
    if (link_a != nullptr) link_a->send(da);
    check(at_a.wait_for_count(2, 2s), "directed send reached dialer a");
    check(!at_b.wait_for_count(2, 300ms), "directed send did NOT reach dialer b");

    // --- directed gathered: prefix + spans as ONE record, reassembled whole ---
    const auto g1 = test_frame(3, 0x30);
    const auto g2 = test_frame(4, 0x40);
    const std::array<std::span<const std::byte>, 2> gathered{std::span(g1), std::span(g2)};
    if (link_a != nullptr) link_a->send(gathered);
    check(at_a.wait_for_count(3, 2s), "gathered directed send reached dialer a");
    {
        std::vector<std::byte> want(g1.begin(), g1.end());
        want.insert(want.end(), g2.begin(), g2.end());
        check(at_a.at(2) == want, "gathered spans arrived concatenated as one frame");
    }

    // --- departure: closing b frees its slot; enumeration tracks it ---
    b.reset();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::size_t live = 2;
    while (std::chrono::steady_clock::now() < deadline) {
        live = 0;
        server.bus()->enumerate_peers([&](std::string_view) { ++live; });
        if (live == 1) break;
        std::this_thread::sleep_for(20ms);
    }
    check(live == 1, "departed peer left enumeration (slot recycled)");
    check(server.bus()->peer_link(name_a) != nullptr, "surviving peer still resolves");

    // --- #426 / ADR-0073 §2: peer names are the routable `p<slot>` fallback ---
    // Same contract as the ws twin: every name a legal segment (fails before the
    // rename — `<ip>:<port>` carries two reserved characters), the delivery tag is
    // the p<slot> spelling, and a recycled slot keeps its stable name.
    {
        bool all_legal = true;
        server.bus()->enumerate_peers([&](std::string_view p) {
            if (!tr::graph::valid_segment(p)) all_legal = false;
        });
        check(all_legal, "every enumerated peer name is a legal path segment (#426)");
        check(name_a == "p0" || name_a == "p1",
              "the DELIVERY TAG is the p<slot> name (the return route is addressable)");
    }
    std::optional<tcp_transport_t> c;
    c.emplace("127.0.0.1", port);
    frame_sink_t at_c;
    auto c_rx = [&](std::span<const std::byte> f) { at_c.push(f); };
    c->set_receiver(c_rx);
    check(c->ok(), "third dialer connected after the departure");
    const auto pc = test_frame(2, 0xC1);
    c->send(pc);
    check(srv_sink.wait_count(3, 2s), "server got the third dialer's frame");
    {
        const std::lock_guard lock(srv_sink.m);
        const auto& [peer, bytes] = srv_sink.frames.back();
        const bool p_form = peer.size() >= 2 && peer[0] == 'p' &&
                            peer.find_first_not_of("0123456789", 1) == std::string::npos;
        check(bytes == pc && p_form, "the new session delivers under a p<slot> name");
        check(peer != name_a, "…and it is not the surviving peer's name (directedness held)");
    }
}

/**
 * @brief The max_peers deployment cap (RFC-0006 injected bound) + the FLAT
 *        (non-peer-named) surface: a peer beyond the cap is refused cleanly
 *        (its connection closes — no handshake exists to fail), a departure
 *        frees the slot, and flat-mode inbound reaches the plain transport_t
 *        receiver untagged.
 */
void test_server_max_peers_cap() {
    std::printf("TCP transport — server max_peers admission cap (flat mode):\n");

    frame_sink_t srv_rx_sink;
    auto srv_rx = [&](std::span<const std::byte> f) { srv_rx_sink.push(f); };
    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/1);
    check(server.ok(), "capped server bound");
    check(server.bus() == nullptr, "flat server exposes no bus facet");
    server.set_receiver(srv_rx);
    const std::uint16_t port = server.local_port();

    std::optional<tcp_transport_t> a;
    a.emplace("127.0.0.1", port);
    check(a->ok(), "first dialer admitted");

    // TCP has no handshake: the refused dialer's connect() lands in the OS
    // accept queue, then the server closes it at admission. Its recv loop sees
    // the EOF and tears down — ok() flips false within a poll bound.
    std::optional<tcp_transport_t> refused;
    refused.emplace("127.0.0.1", port);
    const auto rdead = std::chrono::steady_clock::now() + 2s;
    while (refused->ok() && std::chrono::steady_clock::now() < rdead)
        std::this_thread::sleep_for(20ms);
    check(!refused->ok(), "second dialer refused cleanly at the cap (connection closed)");
    refused.reset();

    // Flat-mode inbound: the admitted dialer's frame reaches the plain receiver.
    const auto f = test_frame(4, 0x77);
    a->send(f);
    check(srv_rx_sink.wait_for_count(1, 2s), "flat inbound reached the transport_t receiver");
    check(srv_rx_sink.at(0) == f, "payload intact through the slot framer");

    a.reset();  // departure frees the slot...
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool readmitted = false;
    while (!readmitted && std::chrono::steady_clock::now() < deadline) {
        tcp_transport_t c("127.0.0.1", port);
        if (c.ok()) {
            // Prove real admission (not just a queued connect): a frame flows.
            const auto probe = test_frame(2, 0x55);
            c.send(probe);
            readmitted = srv_rx_sink.wait_for_count(2, 500ms);
        }
        if (!readmitted) std::this_thread::sleep_for(50ms);
    }
    check(readmitted, "...and the next dialer is admitted into the recycled slot");
}

/**
 * @brief #889 — a FLAT multi-peer server reports the link down only when its LAST
 *        session departs, never on a mid-life close.
 *
 * A flat server (`peer_named=false`, the default) admits N concurrent peers and has ONE
 * routing identity for all of them — the registered child NAME. Its departure seam is
 * therefore `transport_t::notify_down` (whole link), which `fwd_router_t::link_down`
 * answers by evicting every subscriber edge under that name. Fire it on one peer's hangup
 * and the surviving peers' routing state is evicted underneath them, which is what this
 * guards: the notifier must stay silent while any session is still open, and fire exactly
 * once when the last one goes.
 *
 * The rule lives in `slot_server_t::teardown_slot` (`posix_endpoint.cpp`), shared verbatim
 * with `transport_ws_server` since #871 — one home, so it is guarded once, here.
 *
 * Both `downs` reads are taken behind an INBOUND-FRAME BARRIER, never on a slot-recycle
 * transition and never after a sleep. `teardown_slot` clears the slot name in its first
 * critical section and fires the departure seam last, so every state the test can poll for
 * becomes visible while the seam is still ahead — read `downs` there and the guard is a
 * coin flip. A frame written after that state was observed cannot be delivered by the poll
 * pass that is running the teardown (`run()` snapshots revents once per pass), so its
 * arrival orders the read strictly after the seam. That is what makes the two assertions
 * naming #889 bite every run instead of one in five.
 */
void test_flat_server_down_only_on_last_session() {
    std::printf("TCP transport — flat server: notify_down only on the LAST departure (#889):\n");

    sink_t srv_rx_sink;
    auto srv_rx = [&](std::span<const std::byte> f) { srv_rx_sink.push(f); };
    std::atomic<int> downs{0};

    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/0);
    check(server.ok(), "flat server bound");
    check(server.bus() == nullptr, "flat server exposes no bus facet (peer_named=false)");
    server.set_receiver(srv_rx);
    server.set_down_notifier([](void* c) { static_cast<std::atomic<int>*>(c)->fetch_add(1); },
                             &downs);
    const std::uint16_t port = server.local_port();

    sink_t at_a;
    auto a_rx = [&](std::span<const std::byte> f) { at_a.push(f); };
    std::optional<tcp_transport_t> a;
    a.emplace("127.0.0.1", port);
    a->set_receiver(a_rx);
    std::optional<tcp_transport_t> b;
    b.emplace("127.0.0.1", port);
    check(a->ok() && b->ok(), "two concurrent dialers on the flat server");

    // Drive one frame from each so BOTH slots are provably open before the close — an
    // assertion about "the last session" is vacuous if the second never got admitted.
    a->send(test_frame(2, 0xA1));
    b->send(test_frame(2, 0xB1));
    check(srv_rx_sink.wait_for_count(2, 2s), "both dialers' frames reached the flat receiver");

    // --- the mid-life close: one of two peers hangs up ---
    b.reset();
    // Wait on the SLOT recycle, not on a sleep: enumerate_peers visits exactly the open,
    // named slots, so 1 means the teardown (and its departure seam) has already run.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::size_t live = 2;
    while (std::chrono::steady_clock::now() < deadline) {
        live = 0;
        server.enumerate_peers([&](std::string_view) { ++live; });
        if (live == 1) break;
        std::this_thread::sleep_for(20ms);
    }
    check(live == 1, "the departed peer's slot was recycled (its teardown ran)");

    // …and the survivor still routes: the broadcast reaches it after the departure.
    const auto bc = test_frame(3, 0xCC);
    server.send(bc);
    check(at_a.wait_for_count(1, 2s), "the surviving peer still receives after the close");
    check(at_a.at(0) == bc, "the survivor's frame is intact");
    a->send(test_frame(2, 0xA2));
    check(srv_rx_sink.wait_for_count(3, 2s), "…and inbound from the survivor still delivers");

    // The `downs` read is taken HERE — one poll-thread step past the teardown — and never on
    // the slot-recycle transition above. `teardown_slot` clears the name in its FIRST
    // critical section and fires the departure seam LAST, so `live == 1` is observable while
    // the seam is still ahead: a read there races the teardown and misses a reintroduced
    // whole-link notify most runs. The survivor's frame closes the race instead of a sleep.
    // It was written after `live == 1` was observed, so the poll pass that carried it cannot
    // be the pass that ran `teardown_slot` — `run()` snapshots revents once per pass, and
    // that pass polled before the frame existed. Its arrival is therefore ordered strictly
    // after the whole teardown, seam included.
    check(downs.load() == 0, "a mid-life close did NOT report the whole link down (#889)");

    // --- the last close: NOW the link is down, exactly once ---
    a.reset();
    const auto ldeadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < ldeadline) {
        live = 0;
        server.enumerate_peers([&](std::string_view) { ++live; });
        if (live == 0) break;
        std::this_thread::sleep_for(20ms);
    }
    check(live == 0, "the last peer's slot was recycled too");

    // The same barrier, built the only way left once every session is gone: a FRESH dialer.
    // Connecting it after `live == 0` puts its accept in a later pass than `teardown_slot`
    // (this pass's listen revents were snapshotted before the connect), and `run()` services
    // peers before it accepts, so its frame lands a pass later still. When that frame
    // arrives, `downs` is final — no sleep is standing in for the ordering, and the window
    // it certifies contains an accept and a delivery, so "nothing further fired" is a
    // statement about a poll loop that demonstrably kept running.
    std::optional<tcp_transport_t> c;
    c.emplace("127.0.0.1", port);
    check(c->ok(), "a fresh dialer connects to the now-idle flat server");
    c->send(test_frame(2, 0xC1));
    check(srv_rx_sink.wait_for_count(4, 2s),
          "the fresh dialer's frame proves the poll thread ran past the LAST teardown");
    check(downs.load() >= 1, "the LAST session's departure reported the link down");
    check(downs.load() == 1,
          "…exactly once — the mid-life close added nothing, and nothing followed (#889)");
}

/**
 * @brief #889 — a FLAT server refuses peer-named wiring: `set_peer_receiver` is rejected
 *        and inbound keeps reaching the flat `transport_t` receiver.
 *
 * `bus()` returning nullptr is the contract "this link has no peer-named tier". But
 * `bus_link_t` is a PUBLIC base, so the setter is reachable by an explicit upcast — and
 * before #889 that silently flipped the server into peer-named delivery the contract said
 * did not exist. The mode authority is the constructed `peer_named` flag alone: the wiring
 * is refused, and delivery keys off the flag rather than off "a peer sink happens to be
 * installed".
 */
void test_flat_server_rejects_peer_receiver() {
    std::printf("TCP transport — flat server refuses peer-named wiring (#889):\n");

    sink_t flat_sink;
    auto flat_rx = [&](std::span<const std::byte> f) { flat_sink.push(f); };
    peer_sink_t peer_sink;

    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/0);
    check(server.ok(), "flat server bound");
    check(server.bus() == nullptr, "flat server exposes no bus facet");
    server.set_receiver(flat_rx);
    // The out-of-contract path itself: the public base, named explicitly. A member-shadowing
    // guard would not catch this call, so the refusal has to live in bus_link_t.
    static_cast<tr::net::bus_link_t&>(server).set_peer_receiver(peer_sink);

    tcp_transport_t a("127.0.0.1", server.local_port());
    check(a.ok(), "dialer connected");
    const auto f = test_frame(4, 0x91);
    a.send(f);

    check(flat_sink.wait_for_count(1, 2s), "inbound still reached the FLAT transport_t receiver");
    check(flat_sink.count() == 1 && flat_sink.at(0) == f, "the flat delivery is byte-intact");
    {
        const std::lock_guard lock(peer_sink.m);
        check(peer_sink.frames.empty(), "the forced peer-named sink received NOTHING");
    }
}

/**
 * @brief #889, the other direction — a PEER-NAMED server never downgrades to flat
 *        delivery just because only a flat receiver happens to be installed.
 *
 * This is the half the mode authority owns on its own. Before #889 the tier select read
 * `peer_rx_.has_any()`, so a peer-named link with no peer sink wired handed its frames to
 * the plain `transport_t` receiver — UNTAGGED. On a link that carries many peers under one
 * name that is a misroute waiting to happen: the return route grown from an untagged frame
 * names the LINK, and a bus mount's own name is not a routable next-hop (RFC-0020 /
 * ADR-0073 §3) — its `send()` BROADCASTS, so the reply would go to every peer. The mode
 * says peer-named, so the peer tier is the only tier; an unwired one drops.
 *
 * The negative half is paired with a live control: the same connection, the right tier
 * wired, delivers — so "nothing arrived" cannot be a dead link passing by accident.
 */
void test_peer_named_server_does_not_downgrade_to_flat() {
    std::printf("TCP transport — a peer-named server does not downgrade to flat (#889):\n");

    sink_t flat_sink;
    auto flat_rx = [&](std::span<const std::byte> f) { flat_sink.push(f); };
    peer_sink_t peer_sink;

    tr::net::transport_tcp_server server(0, &tr::mem::heap_backend(), 0, /*max_peers=*/0,
                                         /*peer_named=*/true);
    check(server.ok(), "peer-named server bound");
    check(server.bus() != nullptr, "peer-named server exposes the bus facet");
    // The WRONG tier for this mode, and the only one wired.
    server.set_receiver(flat_rx);

    tcp_transport_t a("127.0.0.1", server.local_port());
    check(a.ok(), "dialer connected");
    a.send(test_frame(4, 0x31));
    check(!flat_sink.wait_for_count(1, 500ms),
          "the peer-named link did NOT deliver to the flat transport_t receiver");

    // The control: same connection, the RIGHT tier wired — so the silence above was the
    // tier decision, not a dead link.
    server.bus()->set_peer_receiver(peer_sink);
    const auto f2 = test_frame(4, 0x32);
    a.send(f2);
    check(peer_sink.wait_count(1, 2s), "…and the peer-named tier delivers once it is wired");
    {
        const std::lock_guard lock(peer_sink.m);
        check(peer_sink.frames.size() == 1 && peer_sink.frames[0].second == f2 &&
                  peer_sink.frames[0].first == "p0",
              "the delivered frame is intact and tagged with the p<slot> peer name");
    }
    check(flat_sink.count() == 0, "the flat receiver never received anything on this link");
}

/**
 * @brief A broadcast that lands INSIDE the accept publish reaches the peer being accepted —
 *        `open ⇒ fd valid`, proven by holding that instant open rather than by racing for it.
 *
 * `slot_server_t::accept_peer` publishes a slot's two sender-visible fields under `write_m_`
 * (the lock `slot_server_t::teardown_slot` resets them under), fd FIRST. Store them unlocked
 * with `open` first — what this server did before #891 — and a broadcast holding
 * `write_m_` can read `open == true`
 * next to `fd == -1` and hand the record to `write_all_iov(-1)`, which drops it on the floor.
 * In production that window is two instructions wide, so a racing test cannot pin it.
 *
 * So this test does not race: `detail::tcp_peer_publishing_hook` PARKS the poll thread at the
 * mid-publish instant, the whole broadcast happens inside that parked window, and only then
 * is the park released. With the pair published under the lock the sender blocks, wakes to a
 * whole slot, and the client reads the record; with the two stores unlocked and reordered the
 * identical sequence writes to fd -1 and the client's socket stays empty until the budget
 * expires. Deterministic in both directions.
 */
void test_accept_publish_is_atomic_to_senders() {
    std::printf("TCP transport — a broadcast inside the accept publish reaches the new peer:\n");

    tr::net::transport_tcp_server server(0);
    check(server.ok(), "listen socket bound");

    // The parked window. `reached` is set on the server's poll thread; `released` by this
    // thread once the broadcast has been made.
    std::mutex m;
    std::condition_variable cv;
    bool reached = false;
    bool released = false;
    bool armed = true;  // one-shot: a later accept (none here) must not park
    struct hook_state_t {
        std::mutex* m;
        std::condition_variable* cv;
        bool* reached;
        bool* released;
        bool* armed;
    };
    static hook_state_t s_state{};  // the hook is a plain function pointer: no capture
    s_state = hook_state_t{&m, &cv, &reached, &released, &armed};

    /** @brief Clears the seam however this test leaves. */
    struct hook_guard_t {
        ~hook_guard_t() { tr::net::detail::tcp_peer_publishing_hook = nullptr; }
    } const guard;

    tr::net::detail::tcp_peer_publishing_hook = [] {
        std::unique_lock lock(*s_state.m);
        if (!*s_state.armed) return;
        *s_state.armed = false;
        *s_state.reached = true;
        s_state.cv->notify_all();
        // Park the publish here until the test has broadcast. Bounded so a broken build
        // fails the assertions rather than hanging the suite.
        s_state.cv->wait_for(lock, 5s, [] { return *s_state.released; });
    };

    const raw_client_t client(server.local_port());
    check(client.fd >= 0, "raw client connected");

    {
        std::unique_lock lock(m);
        check(cv.wait_for(lock, 2s, [&] { return reached; }),
              "the poll thread reached the instant mid-publish");
    }

    // The whole broadcast happens INSIDE the parked window: under the fix it blocks on
    // write_m_ and lands on a published slot; without it, it reads the half-published one.
    const auto payload = test_frame(6, 0x5A);
    std::thread sender([&] { server.send(payload); });
    // Give the sender time to reach the send path inside the window. Generous either way:
    // with the publish locked the send merely waits longer for write_m_ and still lands.
    std::this_thread::sleep_for(200ms);
    {
        const std::lock_guard lock(m);
        released = true;
    }
    cv.notify_all();
    sender.join();

    const auto want = record(payload);
    const auto got = read_within(client.fd, want.size(), 2s);
    check(got == want, "the frame broadcast in that instant REACHED the accepted peer");
}

/** @brief A bound+listening raw loopback socket the test drives by hand (accept, write, close). */
struct raw_listener_t {
    int fd = -1;            /**< @brief The listening socket, or -1 if setup failed. */
    std::uint16_t port = 0; /**< @brief The ephemeral port the kernel assigned. */

    raw_listener_t() {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        const int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) < 0 ||
            ::listen(fd, 1) < 0) {
            ::close(fd);
            fd = -1;
            return;
        }
        sockaddr_in bound{};
        socklen_t blen = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
            port = ntohs(bound.sin_port);
    }
    ~raw_listener_t() {
        if (fd >= 0) ::close(fd);
    }
    raw_listener_t(const raw_listener_t&) = delete;
    raw_listener_t& operator=(const raw_listener_t&) = delete;
};

/**
 * @brief #1045 — a `defer_recv` DIAL link reads NOTHING until start_receiving(), so a frame
 *        the peer pushes ON CONNECT cannot be dropped into an empty sink.
 *
 * The one-phase constructor connects AND spawns the recv thread before it returns, so
 * `set_receiver` can only ever run afterwards. A peer that pushes the instant it accepts has
 * that frame in flight before the constructor returns, and whether it beats the caller's next
 * statement is decided by nothing but scheduling: the recv thread reaching its first
 * `read_exact` versus the calling thread performing one store. Lose that race and
 * `receiver_slot_t`'s empty slot drops the frame — no `dropped_rx()`, no `malformed_rx()`, a
 * healthy-looking connection. Measured at `ce5e902b` with a 300 ms sink-install delay: peer
 * pushed 1, sink received 0, both counters 0.
 *
 * The guard does not race it. The peer writes ONE complete length-prefixed record the moment
 * it accepts and then goes quiet; the link is constructed with `defer_recv` and its sink is
 * installed IMMEDIATELY — so the silence below cannot be explained by a missing sink, only by
 * an un-armed link. Then:
 *
 *  - SILENCE. For a generous window nothing is delivered and neither counter moves, because
 *    not one byte has been read off the socket.
 *  - the FRAME, once `start_receiving()` runs — the positive control that keeps the silence
 *    from being vacuous: the bytes were really there, and really decodable.
 *
 * Reverting the `defer_recv` gate in `tcp_transport_t`'s DIAL constructor reddens this in one
 * of two ways — the frame is delivered inside the silence window, or it was decoded into the
 * empty slot before `set_receiver` ran and never arrives at all. Both are failures, and which
 * one shows is exactly the scheduling race the fix removes.
 */
void test_push_on_connect_waits_for_start_receiving() {
    std::printf("TCP transport — a push-on-connect frame waits for the sink (#1045):\n");
    const raw_listener_t peer_listener;
    check(peer_listener.fd >= 0, "raw listener bound and listening");

    const auto pushed = test_frame(11, 0x70);
    const auto rec = record(pushed);

    std::promise<bool> one_write_done;  // the whole record went out as ONE send
    std::promise<void> test_done;       // safe to close the peer socket
    auto one_write_fut = one_write_done.get_future();
    auto done_fut = test_done.get_future();

    std::thread pusher([&] {
        const int cfd = ::accept(peer_listener.fd, nullptr, nullptr);
        if (cfd < 0) {
            one_write_done.set_value(false);
            return;
        }
        // The push-on-connect shape: one COMPLETE record the instant the connection is
        // accepted, and then the peer never writes again.
        const ssize_t sent = ::send(cfd, rec.data(), rec.size(), 0);
        one_write_done.set_value(sent == static_cast<ssize_t>(rec.size()));
        done_fut.wait();
        ::close(cfd);
    });

    // The sink outlives the transport that delivers to it (this file's destruction idiom).
    frame_sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.push(f); };
    {
        tcp_transport_t dialer("127.0.0.1", peer_listener.port, &tr::mem::heap_backend(),
                               /*max_frame=*/0, /*recv_stack=*/0, /*defer_recv=*/true);
        check(dialer.ok(), "the deferred dialer's connect still happened in the constructor");
        // Installed BEFORE the window: what holds the frame back below is the un-armed link,
        // and nothing else.
        dialer.set_receiver(rx);
        check(one_write_fut.wait_for(3s) == std::future_status::ready && one_write_fut.get(),
              "the peer pushed one COMPLETE record the moment it accepted");

        // Orders of magnitude more than a recv thread needs to drain what is already sitting
        // in the socket, so a link that spawned that thread in its constructor fails here.
        std::this_thread::sleep_for(500ms);
        check(sink.count() == 0,
              "the deferred link delivered NOTHING for the whole window: nothing was read");
        check(dialer.dropped_rx() == 0 && dialer.malformed_rx() == 0,
              "and it was not shed under another name either (both counters still 0)");

        dialer.start_receiving();
        check(sink.wait_for_count(1, 4s),
              "after start_receiving() the pushed frame was DELIVERED, not dropped");
        check(sink.count() == 1 && sink.at(0) == pushed, "carrying the pushed frame's own bytes");
        check(dialer.dropped_rx() == 0 && dialer.malformed_rx() == 0,
              "delivered with both counters still 0");
        test_done.set_value();
    }
    pusher.join();
}

/**
 * @brief #1045 — `start_receiving()` is a safe no-op on every link that has nothing to arm.
 *
 * `transport_vertex_t::make_connection` calls it unconditionally on every link it wires, so
 * the three cases that are NOT a deferred DIAL must all be inert: a second call on an armed
 * link, a one-phase DIAL (its thread started in the constructor), a LISTEN link (its accept
 * loop is that one `start`), and a link whose dial failed (no socket to serve).
 *
 * The observable for "no second thread" is exactly-one delivery plus a clean teardown:
 * `posix_endpoint_t::start` may be called at most once, and a second call would overwrite the
 * thread handle so `stop_and_join` joins only the newer one — leaving the older thread running
 * on a destroyed object, which the sanitized CI builds of this suite report.
 */
void test_start_receiving_is_idempotent() {
    std::printf("TCP transport — start_receiving() is idempotent and inert where it must be:\n");

    // (a) a one-phase DIAL + (b) a LISTEN link: both started their thread in the constructor.
    {
        frame_sink_t at_listener, at_dialer;
        auto listener_rx = [&](std::span<const std::byte> f) { at_listener.push(f); };
        auto dialer_rx = [&](std::span<const std::byte> f) { at_dialer.push(f); };
        tcp_transport_t listener(std::uint16_t{0});
        check(listener.ok(), "listener bound (ephemeral port)");
        listener.start_receiving();  // before any peer at all
        tcp_transport_t dialer("127.0.0.1", listener.local_port());
        check(dialer.ok(), "one-phase dialer connected");
        listener.set_receiver(listener_rx);
        dialer.set_receiver(dialer_rx);

        // Round-trip FIRST, so the listener has demonstrably ACCEPTED its peer (its accept
        // loop polls on a 100 ms grain, so calling straight after the connect would find
        // `conn_fd_` still -1 and exercise nothing).
        const auto f1 = test_frame(5, 0x10);
        dialer.send(f1);
        check(at_listener.wait_for_count(1, 2000ms), "the listener accepted its peer and receives");

        // ...and only now arm both, twice each: this is the state in which a missing guard
        // spawns a second `serve` loop onto the accepted fd.
        listener.start_receiving();
        listener.start_receiving();
        dialer.start_receiving();
        dialer.start_receiving();

        const auto f1b = test_frame(13, 0x30);
        dialer.send(f1b);
        check(at_listener.wait_for_count(2, 2000ms), "the listener still receives after arming");
        const auto f2 = test_frame(9, 0x40);
        listener.send(f2);
        check(at_dialer.wait_for_count(1, 2000ms), "the one-phase dialer still receives");
        std::this_thread::sleep_for(200ms);
        check(at_listener.count() == 2 && at_listener.at(0) == f1 && at_listener.at(1) == f1b,
              "each frame exactly once at the listener (no second serve loop on its peer)");
        check(at_dialer.count() == 1 && at_dialer.at(0) == f2,
              "exactly once at the dialer (no second recv thread)");
        check(listener.malformed_rx() == 0 && dialer.malformed_rx() == 0,
              "and neither stream lost framing sync");
    }

    // (c) a dial that FAILED: ok() is false and there is no socket to serve. The port is a
    // real free one (reserved and released — a listener that never accepts closes straight to
    // CLOSED, no TIME_WAIT), so the connect is refused rather than landing somewhere.
    std::uint16_t dead_port = 0;
    {
        tcp_transport_t port_probe{std::uint16_t{0}};
        dead_port = port_probe.local_port();
    }
    check(dead_port != 0, "reserved (and released) a free localhost port");
    std::atomic<int> delivered{0};
    auto rx = [&](std::span<const std::byte>) { delivered.fetch_add(1); };
    {
        tcp_transport_t bad("127.0.0.1", dead_port, &tr::mem::heap_backend(), /*max_frame=*/0,
                            /*recv_stack=*/0, /*defer_recv=*/true);
        check(!bad.ok(), "the dial to a closed port failed");
        bad.set_receiver(rx);
        bad.start_receiving();
        bad.start_receiving();
    }
    check(delivered.load() == 0, "arming a failed dial delivered nothing and did not crash");
}

}  // namespace

// Proves the recv_stack knob (libtracer #486): a transport constructed with an
// explicit recv-thread stack size still spawns its recv thread and round-trips.
// On an MCU this is the RAM lever (right-size the thread instead of raising the
// global pthread default); here it confirms the value plumbs through start() to
// pthread_attr_setstacksize and the recv thread runs correctly (under TSan+ASan).
void test_recv_stack_sized() {
    std::printf("TCP transport — explicit recv-thread stack size round-trips:\n");
    // Sized down from the ~8 MiB host default, yet ample under the sanitizers for
    // a one-frame round-trip; the point is that a non-default size is honored.
    constexpr std::size_t kStack = 512 * 1024;
    frame_sink_t at_listener;
    auto listener_rx = [&](std::span<const std::byte> f) { at_listener.push(f); };
    tcp_transport_t listener(std::uint16_t{0}, &tr::mem::heap_backend(), 0, kStack);
    check(listener.ok(), "listener bound with a sized recv stack");
    tcp_transport_t dialer("127.0.0.1", listener.local_port(), &tr::mem::heap_backend(), 0, kStack);
    check(dialer.ok(), "dialer connected with a sized recv stack");

    listener.set_receiver(listener_rx);
    const auto f1 = test_frame(7, 0x22);
    dialer.send(f1);
    check(at_listener.wait_for_count(1, 2000ms), "frame received over the sized-stack recv thread");
    check(at_listener.count() == 1 && at_listener.at(0) == f1, "received bytes are identical");
}

int main() {
    test_raw_frame_duplex();
    test_recv_stack_sized();
    test_partial_and_coalesced();
    test_oversize_prefix();
    test_settings_max_frame();
    test_view_delivery_segment_identity();
    test_backpressure_drain();
    test_scatter_gather();
    test_two_nodes_over_tcp();
    test_config_constructed_tcp();
    test_server_multi_peer_bus();
    test_server_max_peers_cap();
    test_flat_server_down_only_on_last_session();
    test_flat_server_rejects_peer_receiver();
    test_peer_named_server_does_not_downgrade_to_flat();
    test_accept_publish_is_atomic_to_senders();
    test_push_on_connect_waits_for_start_receiving();
    test_start_receiving_is_idempotent();
    return tr::testing::summary("tcp");
}
