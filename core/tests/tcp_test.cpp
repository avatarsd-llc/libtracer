/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * M6 TCP transport tests: length-prefix framing over a real localhost TCP stream.
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
#include <sys/socket.h>
#include <unistd.h>

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

#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::tcp_transport_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// A collecting sink: frames delivered on the recv thread, read from the test thread.
struct sink_t {
    std::mutex m;
    std::vector<std::vector<std::byte>> frames;

    void push(std::span<const std::byte> f) {
        const std::lock_guard lock(m);
        frames.emplace_back(f.begin(), f.end());
    }
    [[nodiscard]] std::size_t count() {
        const std::lock_guard lock(m);
        return frames.size();
    }
    [[nodiscard]] std::vector<std::byte> at(std::size_t i) {
        const std::lock_guard lock(m);
        return frames.at(i);
    }
    // Wait until `n` frames arrived (or the deadline passes).
    [[nodiscard]] bool wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (count() < n) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }
};

// A raw POSIX TCP client — the test's hand on the wire, so writes can be split
// and coalesced at will (a tcp_transport_t dialer would hide the boundaries).
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

// One length-prefixed record: u32-LE len ++ payload (the M6 transport framing).
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

void test_raw_frame_duplex() {
    std::printf("TCP transport — raw frames both ways over localhost:\n");
    // Sinks + named receiver lambdas BEFORE the transports: the slot binds the
    // callable by address, and ~tcp_transport_t joins the recv thread, so the
    // callable must outlive the transport.
    sink_t at_listener, at_dialer;
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
    sink_t sink;
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

// A per-connection :settings max_frame tightens the receive cap below kMaxFrame:
// a prefix within the 16 MiB protocol ceiling but above the connection's cap is
// rejected as malformed (kMaxFrame→:settings; behavior-preserving default when 0).
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

// A heap-delegating backend that RECORDS every segment it hands out (segment
// identity) and can FAIL its first `fail_first` allocations (backpressure).
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

// ADR-0042 — the owning delivery path: an installed view receiver gets each frame
// as a view over a fresh refcounted segment from the injected backend, allocated
// at exactly the frame length (the stream reader knows `len` before it reads).
void test_view_delivery_segment_identity() {
    std::printf("TCP transport — owning view delivery (ADR-0042 receiver seam):\n");
    recording_backend_t rec;
    std::promise<tr::view::view_t> got;
    auto fut = got.get_future();
    auto rope_rx = [&](tr::view::rope_t f) {
        if (f.link_count() == 1) got.set_value(f.links()[0]);  // single-link: the trivial rope
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

// ADR-0042 §2 backpressure on a stream: an exhausted backend drops the frame but
// DRAINS it off the stream, so framing sync survives and later frames deliver.
void test_backpressure_drain() {
    std::printf("TCP transport — backend exhaustion drains the frame, sync survives:\n");
    recording_backend_t rec(2);  // the first two allocations fail
    sink_t sink;
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
    sink_t sink;
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

// Build FWD{ op=WRITE, dst=<segs...>, src=<empty PATH>, payload=<VALUE> } — a remote
// write routed by explicit source route (RFC-0004 §D, ADR-0040).
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

// FWD{ op=READ, dst, src } — a remote read whose REPLY source-routes back.
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

// SPEC{ NAME "type" <type>, NAME "name" <name>, SETTINGS "config"{ NAME "role" VALUE u8,
//       NAME "port" VALUE u16, NAME "kind" NAME "tcp" [, NAME "addr" NAME <addr>] } }
// — a connection-creation spec (ADR-0027 / reference/05), the transport_vertex_test shape.
view_t conn_spec(std::string_view type, std::string_view name, tr::net::conn_role_t role,
                 std::uint16_t port, std::string_view addr = {}) {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(role)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    tr::wire::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);
    tr::wire::emit_name(cfg, "kind");
    tr::wire::emit_name(cfg, "tcp");
    if (!addr.empty()) {
        tr::wire::emit_name(cfg, "addr");
        tr::wire::emit_name(cfg, addr);
    }

    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    tr::wire::emit_name(body, "config");
    tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
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

    // A's reply sink is set BEFORE the sockets exist (configure before frames flow).
    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    router_a.on_reply([&got](const tr::view::rope_t& reply) {
        try {
            const tr::view::view_t mat = reply.materialize();
            const auto b = mat.bytes();
            got.set_value(std::vector<std::byte>(b.begin(), b.end()));
        } catch (...) {
        }
    });

    // B: a stored value at /temp and a tcp LISTENER on a fixed localhost port.
    (void)node_b.register_vertex(path_t("/temp"), role_t::STORED_VALUE);
    std::vector<std::byte> tv;
    const std::byte tb{0x2A};
    tr::wire::emit_tlv(tv, type_t::VALUE, opt_t{}, std::span<const std::byte>(&tb, 1));
    (void)node_b.write(path_t("/temp"), owned(tv));
    const auto wb = node_b.write(path_t("/net:children[]"),
                                 conn_spec("listener", "a", tr::net::conn_role_t::LISTEN, 47130));
    check(wb.has_value(), "B: SPEC{listener, kind=tcp, port} constructs the bound socket");
    check(router_b.registry().by_name("a") != nullptr, "B: the socket is wired into the router");

    // A: a tcp CLIENT dialing B's port — a SYNCHRONOUS connect from config.
    const auto wa =
        node_a.write(path_t("/net:children[]"),
                     conn_spec("client", "b", tr::net::conn_role_t::DIAL, 47130, "127.0.0.1"));
    check(wa.has_value(), "A: SPEC{client, kind=tcp, addr, port} constructs the dialing socket");
    const auto* s = net_a.settings_of("b");
    check(s != nullptr && s->kind == "tcp" && s->addr == "127.0.0.1" && s->port == 47130,
          "A: the parsed :settings carry kind/addr/port");

    // End-to-end: FWD{READ dst=/b/temp} from A crosses A's config-created stream to
    // B's terminus, and the REPLY source-routes back over the same connection.
    router_a.on_frame("self", fwd_read({"b", "temp"}, {"reply-ep"}));
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

}  // namespace

int main() {
    test_raw_frame_duplex();
    test_partial_and_coalesced();
    test_oversize_prefix();
    test_settings_max_frame();
    test_view_delivery_segment_identity();
    test_backpressure_drain();
    test_scatter_gather();
    test_two_nodes_over_tcp();
    test_config_constructed_tcp();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
