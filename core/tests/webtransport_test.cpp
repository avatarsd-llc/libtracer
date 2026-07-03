/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0043 Phase B — webtransport_transport_t tests (a consumer of the separate
 * libtracer_quic module target; configured only with LIBTRACER_WITH_QUIC).
 * Self-contained end-to-end coverage WITHOUT a browser: the module's DIAL mode
 * implements the client half of the minimal H3 handshake, so a C++
 * WebTransport client establishes a real session (SETTINGS exchange, extended
 * CONNECT, 200, the 0x41 frame channel) against the C++ server over localhost
 * and runs the FWD READ round-trip. The module-private H3/QPACK subset
 * (src/wt_h3.hpp) is additionally pinned against RFC vectors — including the
 * RFC 7541 Huffman examples, since our own encoder never emits Huffman (a
 * browser's does). Run under TSan (setarch -R) and ASan+UBSan.
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../src/wt_h3.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_webtransport.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::webtransport_dial_tls_t;
using tr::net::webtransport_transport_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;
void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

// Dev cert paths — generated once in main() by scripts/gen-dev-cert.sh.
std::string g_cert;
std::string g_key;

webtransport_dial_tls_t dev_tls() {
    return webtransport_dial_tls_t{.ca_file = {}, .insecure_no_verify = true};
}

// A collecting sink: frames delivered on an msquic worker thread, read from the
// test thread.
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
    [[nodiscard]] bool wait_for_count(std::size_t n, std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (count() < n) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }
};

std::vector<std::byte> test_frame(std::size_t len, std::uint8_t seed) {
    std::vector<std::byte> f(len);
    for (std::size_t i = 0; i < len; ++i) f[i] = static_cast<std::byte>(seed + i);
    return f;
}

std::vector<std::uint8_t> from_hex(std::string_view hex) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const auto nib = [](char c) -> std::uint8_t {
            if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
            return static_cast<std::uint8_t>(c - 'a' + 10);
        };
        out.push_back(static_cast<std::uint8_t>((nib(hex[i]) << 4) | nib(hex[i + 1])));
    }
    return out;
}

// ---- the module-private H3/QPACK subset, pinned against RFC vectors ----

void test_wt_h3_varint() {
    std::printf("wt_h3 — QUIC varint codec (RFC 9000 §16 round-trips):\n");
    bool ok = true;
    for (const std::uint64_t v :
         {std::uint64_t{0}, std::uint64_t{63}, std::uint64_t{64}, std::uint64_t{16'383},
          std::uint64_t{16'384}, std::uint64_t{0x2b603742}, std::uint64_t{0xc671706a},
          std::uint64_t{0x3fff'ffff'ffff'ffff}}) {
        std::vector<std::uint8_t> buf;
        tr::net::wt_h3::append_varint(buf, v);
        const auto r = tr::net::wt_h3::read_varint(buf);
        if (!r || r->value != v || r->consumed != buf.size()) ok = false;
    }
    check(ok, "varints round-trip across all four length classes");
    // RFC 9000 Appendix A.1 example: 0x9d7f3e7d decodes to 494878333.
    const auto rfc = from_hex("9d7f3e7d");
    const auto r = tr::net::wt_h3::read_varint(rfc);
    check(r && r->value == 494878333 && r->consumed == 4, "RFC 9000 A.1 vector decodes");
    check(!tr::net::wt_h3::read_varint(std::span<const std::uint8_t>(rfc).first(2)),
          "a truncated varint reports need-more");
}

void test_wt_h3_huffman() {
    std::printf("wt_h3 — Huffman decoding (RFC 7541 Appendix C vectors):\n");
    const auto dec = [](std::string_view hex) {
        const auto bytes = from_hex(hex);
        return tr::net::wt_h3::huffman_decode(bytes);
    };
    const auto v1 = dec("f1e3c2e5f23a6ba0ab90f4ff");
    check(v1 && *v1 == "www.example.com", "C.4.1: 'www.example.com'");
    const auto v2 = dec("a8eb10649cbf");
    check(v2 && *v2 == "no-cache", "C.4.2: 'no-cache'");
    const auto v3 = dec("25a849e95ba97d7f");
    check(v3 && *v3 == "custom-key", "C.4.3: 'custom-key'");
    const auto v4 = dec("25a849e95bb8e8b4bf");
    check(v4 && *v4 == "custom-value", "C.4.3: 'custom-value'");
    const auto bad = dec("25a849e95bb8e8b4b0");  // broken padding (not an EOS prefix)
    check(!bad, "invalid padding is rejected");
}

void test_wt_h3_field_sections() {
    std::printf("wt_h3 — QPACK static-subset field sections:\n");
    // Our own extended CONNECT encoding decodes back to its five pseudo-headers.
    const auto req = tr::net::wt_h3::encode_connect_field_section("robot.local:4433", "/");
    const auto hdrs = tr::net::wt_h3::decode_field_section(req);
    bool ok = hdrs.has_value();
    std::string method, scheme, authority, path, protocol;
    if (ok) {
        for (const auto& h : *hdrs) {
            if (h.name == ":method") method = h.value;
            if (h.name == ":scheme") scheme = h.value;
            if (h.name == ":authority") authority = h.value;
            if (h.name == ":path") path = h.value;
            if (h.name == ":protocol") protocol = h.value;
        }
    }
    check(ok && method == "CONNECT" && protocol == "webtransport" && scheme == "https" &&
              authority == "robot.local:4433" && path == "/",
          "encode_connect_field_section round-trips through the decoder");

    // A Huffman-encoded literal-name field line (what a browser emits), built
    // from the RFC 7541 C.4.3 vectors: custom-key: custom-value.
    std::vector<std::uint8_t> sec{0x00, 0x00};
    sec.push_back(0x2f);  // 001 N=0 H=1, 3-bit len prefix saturated (7)
    sec.push_back(0x01);  // + 1 => name length 8 (Huffman bytes)
    const auto hk = from_hex("25a849e95ba97d7f");
    sec.insert(sec.end(), hk.begin(), hk.end());
    sec.push_back(0x89);  // value: H=1, length 9
    const auto hv = from_hex("25a849e95bb8e8b4bf");
    sec.insert(sec.end(), hv.begin(), hv.end());
    const auto lit = tr::net::wt_h3::decode_field_section(sec);
    check(lit && lit->size() == 1 && (*lit)[0].name == "custom-key" &&
              (*lit)[0].value == "custom-value",
          "a Huffman literal-name field line (browser shape) decodes");

    // The 200 response section decodes to :status 200.
    const auto resp =
        tr::net::wt_h3::decode_field_section(tr::net::wt_h3::encode_status_200_field_section());
    check(resp && resp->size() == 1 && (*resp)[0].name == ":status" && (*resp)[0].value == "200",
          "the 200 response section decodes to :status=200");

    // Dynamic-table use (Required Insert Count != 0) is out of subset.
    const std::array<std::uint8_t, 3> dyn{0x02, 0x00, 0xc1};
    check(!tr::net::wt_h3::decode_field_section(dyn), "RIC != 0 (dynamic table) is rejected");
}

// ---- end-to-end: the C++ WebTransport client against the C++ server ----

void test_session_and_raw_duplex() {
    std::printf("WebTransport — session establishment + raw frames both ways:\n");
    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
    check(listener.ok(), "listener started (ALPN h3, ephemeral port, dev cert)");
    webtransport_transport_t dialer("127.0.0.1", listener.local_port(), "/", dev_tls());
    check(dialer.ok(), "dialer completed QUIC + H3 SETTINGS + extended CONNECT + 200");
    check(dialer.link_up(), "dialer reports link up (CONNECTED)");
    check(dialer.session_up(), "dialer reports the WebTransport session up (200 received)");

    // The server flips session_up when it validates the CONNECT — await it.
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!listener.session_up() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    check(listener.session_up(), "server reports the session up (CONNECT validated, 200 sent)");

    sink_t at_listener, at_dialer;
    listener.set_receiver([&](std::span<const std::byte> f) { at_listener.push(f); });
    dialer.set_receiver([&](std::span<const std::byte> f) { at_dialer.push(f); });

    const auto f1 = test_frame(5, 0x10);
    dialer.send(f1);
    check(at_listener.wait_for_count(1, 3000ms), "dialer->listener frame received");
    check(at_listener.count() == 1 && at_listener.at(0) == f1, "received bytes are identical");

    const auto f2 = test_frame(9, 0x40);
    listener.send(f2);
    check(at_dialer.wait_for_count(1, 3000ms), "listener->dialer frame received");
    check(at_dialer.count() == 1 && at_dialer.at(0) == f2, "reply bytes are identical");
}

void test_big_frame_chunking() {
    std::printf("WebTransport — a big frame arrives through many RECEIVE chunks:\n");
    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
    sink_t sink;
    listener.set_receiver([&](std::span<const std::byte> f) { sink.push(f); });
    webtransport_transport_t dialer("127.0.0.1", listener.local_port(), "/", dev_tls());

    const auto small1 = test_frame(3, 0x07);
    const auto big = test_frame(300 * 1024, 0x11);
    const auto small2 = test_frame(11, 0xC0);
    dialer.send(small1);
    dialer.send(big);
    dialer.send(small2);

    check(sink.wait_for_count(3, 5000ms), "all three frames delivered");
    check(sink.count() == 3 && sink.at(0) == small1, "small frame before the big one intact");
    check(sink.count() == 3 && sink.at(1) == big, "300 KiB frame byte-identical (reassembled)");
    check(sink.count() == 3 && sink.at(2) == small2, "small frame after the big one intact");
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

// ADR-0042 — owning view delivery over the WebTransport frame stream, plus the
// backpressure drain (framing sync survives an exhausted backend).
void test_view_delivery_and_backpressure() {
    std::printf("WebTransport — owning view delivery (ADR-0042) + backpressure drain:\n");
    recording_backend_t rec;
    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key, &rec);
    check(listener.delivers_views(), "webtransport_transport_t::delivers_views() is true");
    webtransport_transport_t dialer("127.0.0.1", listener.local_port(), "/", dev_tls());

    std::promise<tr::view::view_t> got;
    auto fut = got.get_future();
    listener.set_view_receiver([&](tr::view::view_t f) { got.set_value(std::move(f)); });

    const auto frame = test_frame(48, 0x21);
    dialer.send(frame);
    const bool arrived = fut.wait_for(3s) == std::future_status::ready;
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
              "the frame segment IS the backend's segment (one copy off the msquic chunks)");
        check(v.owner && v.owner->bytes.size() == frame.size(),
              "the segment was allocated at exactly the frame length");
    }
    check(listener.dropped_rx() == 0, "no backpressure drops on the healthy backend");

    // Backpressure: the first two allocations fail; the third frame delivers.
    recording_backend_t starved(2);
    webtransport_transport_t listener2(std::uint16_t{0}, g_cert, g_key, &starved);
    sink_t sink;
    listener2.set_view_receiver([&](tr::view::view_t f) { sink.push(f.bytes()); });
    webtransport_transport_t dialer2("127.0.0.1", listener2.local_port(), "/", dev_tls());
    const auto d1 = test_frame(8192, 0x01);
    const auto d2 = test_frame(16, 0x50);
    const auto d3 = test_frame(24, 0xA0);
    dialer2.send(d1);
    dialer2.send(d2);
    dialer2.send(d3);
    check(sink.wait_for_count(1, 4000ms), "the third frame still delivers after two drops");
    check(listener2.dropped_rx() == 2, "both exhausted frames counted as backpressure drops");
    check(sink.count() == 1 && sink.at(0) == d3,
          "framing sync survived the drained frames (byte-identical delivery)");
    check(listener2.malformed_rx() == 0, "no malformed prefixes — the stream never desynced");
}

// FWD{ op=READ, dst, src } — a remote read whose REPLY source-routes back.
std::vector<std::byte> fwd_read(std::initializer_list<std::string_view> dst,
                                std::initializer_list<std::string_view> src) {
    std::vector<std::byte> body;
    const std::byte op{static_cast<std::uint8_t>(tr::graph::fwd_op_t::READ)};
    tr::detail::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&op, 1));
    std::vector<std::byte> dst_segs;
    for (std::string_view s : dst) tr::detail::emit_name(dst_segs, s);
    tr::detail::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, dst_segs);
    std::vector<std::byte> src_segs;
    for (std::string_view s : src) tr::detail::emit_name(src_segs, s);
    tr::detail::emit_tlv(body, type_t::PATH, opt_t{.pl = true}, src_segs);
    std::vector<std::byte> frame;
    tr::detail::emit_tlv(frame, type_t::FWD, opt_t{.pl = true}, body);
    return frame;
}

view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

// The #92 shape, kept entirely in C++: a WebTransport CLIENT reaches a node's
// graph — FWD{READ dst=/b/temp} crosses the session's frame stream to B's
// terminus and the FWD{REPLY} source-routes back over the same session.
void test_fwd_read_round_trip() {
    std::printf("Two nodes over WebTransport — FWD READ round-trip (the #92 shape):\n");
    graph_t node_a, node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    // Transports AFTER the routers (destruct first — drain msquic callbacks
    // before the routers die), the quic_test declaration-order contract.
    webtransport_transport_t tb(std::uint16_t{0}, g_cert, g_key);  // B serves WebTransport
    webtransport_transport_t ta("127.0.0.1", tb.local_port(), "/", dev_tls());
    check(ta.ok(), "the WebTransport session to B is up");

    (void)node_b.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE);
    std::vector<std::byte> tv;
    const std::byte tbyte{0x2A};
    tr::detail::emit_tlv(tv, type_t::VALUE, opt_t{}, std::span<const std::byte>(&tbyte, 1));
    (void)node_b.write(*path_t::parse("/sensor/temp"), owned(tv));

    router_a.add_child("b", ta);
    router_b.add_child("a", tb);

    std::promise<std::vector<std::byte>> got;
    auto fut = got.get_future();
    router_a.on_reply([&got](const tr::wire::tlv_t& reply) {
        try {
            got.set_value(tr::wire::encode(reply));
        } catch (...) {
        }
    });

    router_a.on_frame("self", fwd_read({"b", "sensor", "temp"}, {"reply-ep"}));
    const bool replied = fut.wait_for(4s) == std::future_status::ready;
    check(replied, "the READ reached B over WebTransport and the REPLY returned");
    if (replied) {
        const std::vector<std::byte> reply_bytes = fut.get();  // owns; decode borrows
        const auto dec = tr::wire::decode(reply_bytes);
        bool has_value = false;
        if (dec && dec->type == type_t::FWD)
            for (const auto& c : dec->children)
                if (c.type == type_t::VALUE && c.payload.size() == 1 &&
                    c.payload[0] == std::byte{0x2A})
                    has_value = true;
        check(has_value, "the REPLY carries B's stored /sensor/temp value");
    }
}

// SPEC{ NAME "type" <type>, NAME "name" <name>, SETTINGS "config"{ role, port,
// kind=webtransport [, addr] [, cert, key] } } — the quic_test conn_spec shape.
view_t conn_spec(std::string_view type, std::string_view name, tr::net::conn_role_t role,
                 std::uint16_t port, std::string_view addr = {}, std::string_view cert = {},
                 std::string_view key = {}) {
    std::vector<std::byte> cfg;
    tr::detail::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(role)};
    tr::detail::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    tr::detail::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::detail::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);
    tr::detail::emit_name(cfg, "kind");
    tr::detail::emit_name(cfg, "webtransport");
    if (!addr.empty()) {
        tr::detail::emit_name(cfg, "addr");
        tr::detail::emit_name(cfg, addr);
    }
    if (!cert.empty()) {
        tr::detail::emit_name(cfg, "cert");
        tr::detail::emit_name(cfg, cert);
        tr::detail::emit_name(cfg, "key");
        tr::detail::emit_name(cfg, key);
    }

    std::vector<std::byte> body;
    tr::detail::emit_name(body, "type");
    tr::detail::emit_name(body, type);
    tr::detail::emit_name(body, "name");
    tr::detail::emit_name(body, name);
    tr::detail::emit_name(body, "config");
    tr::detail::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

void test_config_constructed_webtransport() {
    std::printf("Config-constructed endpoints: kind=webtransport from :children[] SPECs:\n");
    graph_t node_a;
    graph_t node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    tr::net::transport_vertex_t net_a(node_a, router_a);
    tr::net::transport_vertex_t net_b(node_b, router_b);
    // The module plugs into the catalog through the extension seam — the core
    // has no `webtransport` builtin.
    net_a.register_transport_type("webtransport", tr::net::webtransport_transport_factory());
    net_b.register_transport_type("webtransport", tr::net::webtransport_transport_factory());

    // B: a webtransport LISTENER on a fixed localhost port, its dev cert/key
    // handed in as the kind-PRIVATE `cert`/`key` config keys.
    const auto wb = node_b.write(
        *path_t::parse("/net:children[]"),
        conn_spec("listener", "a", tr::net::conn_role_t::LISTEN, 47133, {}, g_cert, g_key));
    check(wb.has_value(),
          "B: SPEC{listener, kind=webtransport, port, cert, key} constructs the listener");
    check(router_b.registry().by_name("a") != nullptr, "B: the endpoint is wired into the router");

    // A missing cert/key on a webtransport LISTEN is a TYPE_MISMATCH.
    const auto bad = node_b.write(*path_t::parse("/net:children[]"),
                                  conn_spec("listener", "bad", tr::net::conn_role_t::LISTEN, 0));
    check(!bad.has_value(), "B: a webtransport listener without cert/key fails creation");

    // A: a webtransport CLIENT dialing B — a synchronous session from config
    // (the DEV-ONLY no-verify trust mode, documented on the factory).
    const auto wa =
        node_a.write(*path_t::parse("/net:children[]"),
                     conn_spec("client", "b", tr::net::conn_role_t::DIAL, 47133, "127.0.0.1"));
    check(wa.has_value(),
          "A: SPEC{client, kind=webtransport, addr, port} constructs the dialing endpoint");
    const auto* s = net_a.settings_of("b");
    check(s != nullptr && s->kind == "webtransport" && s->addr == "127.0.0.1" && s->port == 47133,
          "A: the parsed :settings carry kind/addr/port");
}

}  // namespace

int main() {
    // Generate the self-signed dev pair once (scripts/gen-dev-cert.sh) — the
    // LISTEN-side credential every test below serves.
    char tmpl[] = "/tmp/libtracer-wt-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (dir == nullptr) {
        std::printf("FAIL: mkdtemp\n");
        return 1;
    }
    const std::string cmd = std::string("sh ") + LIBTRACER_DEV_CERT_SCRIPT + " " + dir;
    if (std::system(cmd.c_str()) != 0) {
        std::printf("FAIL: gen-dev-cert.sh\n");
        return 1;
    }
    g_cert = std::string(dir) + "/cert.pem";
    g_key = std::string(dir) + "/key.pem";

    test_wt_h3_varint();
    test_wt_h3_huffman();
    test_wt_h3_field_sections();
    test_session_and_raw_duplex();
    test_big_frame_chunking();
    test_view_delivery_and_backpressure();
    test_fwd_read_round_trip();
    test_config_constructed_webtransport();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
