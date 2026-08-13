/**
 * @file
 * @brief ADR-0043 Phase B — webtransport_transport_t tests (a consumer of the separate
 *        libtracer_quic module target; configured only with LIBTRACER_WITH_QUIC).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Self-contained end-to-end coverage WITHOUT a browser: the module's DIAL mode
 * implements the client half of the minimal H3 handshake, so a C++
 * WebTransport client establishes a real session (SETTINGS exchange, extended
 * CONNECT, 200, the 0x41 frame channel) against the C++ server over localhost
 * and runs the FWD READ round-trip. The module-private H3/QPACK subset
 * (src/wt_h3.hpp) is additionally pinned against RFC vectors — including the
 * RFC 7541 Huffman examples, since our own encoder never emits Huffman (a
 * browser's does). Run under TSan (setarch -R) and ASan+UBSan.
 *
 * The well-behaved DIAL client cannot express a MISBEHAVING peer, so the
 * classifier's two contracts get a raw msquic H3 client of their own
 * (raw_wt_client_t): #919's three 0x41-adoption guards (session-gated,
 * id-checked, single) and #920's unknown/GREASE frame skip.
 */

#include <msquic.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../src/wt_h3.hpp"
#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_webtransport.hpp"
#include "raw_wt_client.hpp"
#include "test_support.hpp"

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

using tr::testing::check;
using tr::testing::frame_sink_t;
// The raw msquic H3 client below crosses the same non-TSan-instrumented library boundary
// transport_webtransport.cpp does: restate msquic's StreamSend -> SEND_COMPLETE and
// callback-serialization happens-before edges for TSan (the matching annotations and their
// rationale are in src/msquic_endpoint.hpp). No-ops outside a TSan build.
using tr::testing::tsan_acquire;
using tr::testing::tsan_cb_guard_t;
using tr::testing::tsan_release;

// The raw peer and the byte builders its vectors are written in (raw_wt_client.hpp — shared
// with the out-of-process driver, #1182).
using tr::testing::wt::connect_frame;
using tr::testing::wt::grease_frame;
using tr::testing::wt::grease_type;
using tr::testing::wt::payload;
using tr::testing::wt::raw_wt_client_t;
using tr::testing::wt::record;
using tr::testing::wt::wt_preamble;

/** @brief Dev cert paths — generated once in main() by tools/gen-dev-cert.sh. */
std::string g_cert;
std::string g_key;
/** @brief A SECOND, unrelated self-signed certificate — never served by anything.
 *         The wrong-CA-bundle vector that proves the `ca` config key is genuinely
 *         consulted rather than merely accepted and ignored (#918). */
std::string g_other_cert;

webtransport_dial_tls_t dev_tls() {
    return webtransport_dial_tls_t{.ca_file = {}, .insecure_no_verify = true};
}

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
    // Sinks + named receiver lambdas BEFORE the transports: the slot binds the
    // callable by address, and the transport dtor drains msquic callbacks.
    frame_sink_t at_listener, at_dialer;
    auto listener_rx = [&](std::span<const std::byte> f) { at_listener.push(f); };
    auto dialer_rx = [&](std::span<const std::byte> f) { at_dialer.push(f); };
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

    listener.set_receiver(listener_rx);
    dialer.set_receiver(dialer_rx);

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
    frame_sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.push(f); };
    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
    listener.set_receiver(rx);
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

/**
 * @brief Both send overloads produce the SAME on-wire record (#890).
 *
 * `send(span)` and `send(iov)` differ only in how they FILL the one owned send
 * buffer — the length prefix, the locked frame-stream read and the
 * ownership-transfer hand-off to msquic are one shared submit path
 * (msquic_endpoint_t::submit_frame / ::submit). This pins that: the same
 * payload delivered whole and delivered as three gathered spans must arrive as
 * two byte-identical frames, so a prefix or gather-offset that only one
 * overload got right cannot pass. quic_test covers the gather for the `quic`
 * kind; the `webtransport` kind had no iov coverage at all.
 */
void test_send_overload_parity() {
    std::printf("WebTransport — span and scatter-gather sends agree on the wire:\n");
    frame_sink_t sink;
    auto rx = [&](std::span<const std::byte> f) { sink.push(f); };
    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
    listener.set_receiver(rx);
    webtransport_transport_t dialer("127.0.0.1", listener.local_port(), "/", dev_tls());

    // One 9-byte payload, sent twice: whole, then split 2 + 0 + 7 (the empty
    // span is skipped by the gather, never by the length).
    const auto whole = test_frame(9, 0x30);
    const std::span<const std::byte> p0(whole.data(), 2);
    const std::span<const std::byte> p1(whole.data() + 2, 0);
    const std::span<const std::byte> p2(whole.data() + 2, 7);
    const std::array<std::span<const std::byte>, 3> iov{p0, p1, p2};

    dialer.send(whole);
    check(sink.wait_for_count(1, 3000ms), "the single-span frame arrived");
    dialer.send(std::span<const std::span<const std::byte>>(iov));
    check(sink.wait_for_count(2, 3000ms), "the gathered frame arrived");
    check(sink.count() == 2 && sink.at(0) == whole,
          "single-span send delivers the payload byte-identically");
    check(sink.count() == 2 && sink.at(1) == whole,
          "gathered send delivers the SAME payload byte-identically");
    check(sink.count() == 2 && sink.at(0) == sink.at(1),
          "both overloads framed the record identically");
    check(listener.malformed_rx() == 0 && listener.dropped_rx() == 0,
          "neither record desynced the length-prefix reassembler");
    // The counting half of #932 that a peerless link cannot show: a send that
    // REACHED the wire must not count. Both overloads delivered above, so an
    // over-counting egress is caught here rather than inflating the metric.
    check(dialer.dropped_tx() == 0, "a delivered send counts no TX drop, on either overload");
}

/**
 * @brief WebTransport's egress shed counter (#932) — the same shed classes as the
 *        QUIC transport, since both derive the counting from `msquic_endpoint_t`.
 *
 * Driven WITHOUT a peer so neither assertion depends on handshake timing; the
 * paired positive (a delivered send counts nothing) is asserted in
 * @ref test_send_overload_parity.
 */
void test_tx_drop_counters() {
    std::printf("WebTransport — outbound sheds are counted (#932):\n");
    webtransport_transport_t idle(std::uint16_t{0}, g_cert, g_key);
    check(idle.dropped_tx() == 0, "a fresh link has shed nothing");

    const auto small = test_frame(4, 0x41);
    idle.send(small);
    check(idle.dropped_tx() == 1, "a send with no peer session counted one TX drop");

    std::vector<std::byte> huge(webtransport_transport_t::kMaxFrame + 1, std::byte{0xCD});
    idle.send(std::span<const std::byte>(huge));
    check(idle.dropped_tx() == 2, "an oversize frame counted a TX drop");

    const tr::net::transport_t& generic = idle;
    check(generic.drop_stats().dropped_tx == 2,
          "drop_stats() reports the TX drops, not a hardcoded zero");
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
    std::promise<tr::view::view_t> got;
    auto fut = got.get_future();
    auto rope_rx = [&](tr::view::rope_t f) {
        if (f.link_count() == 1) got.set_value(f.links()[0]);  // single-link: the trivial rope
    };
    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key, &rec);
    check(listener.delivers_ropes(), "webtransport_transport_t::delivers_ropes() is true");
    webtransport_transport_t dialer("127.0.0.1", listener.local_port(), "/", dev_tls());

    listener.set_rope_receiver(rope_rx);

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
    frame_sink_t sink;
    auto rope_rx2 = [&](tr::view::rope_t f) { sink.push(f.links()[0].bytes()); };
    webtransport_transport_t listener2(std::uint16_t{0}, g_cert, g_key, &starved);
    listener2.set_rope_receiver(rope_rx2);
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
    return tr::testing::b_fwd(tr::graph::fwd_op_t::READ, tr::testing::b_path(dst),
                              tr::testing::b_path(src));
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

    (void)node_b.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    std::vector<std::byte> tv;
    const std::byte tbyte{0x2A};
    tr::wire::emit_tlv(tv, type_t::VALUE, opt_t{}, std::span<const std::byte>(&tbyte, 1));
    (void)node_b.write(path_t("/sensor/temp"), owned(tv));

    router_a.add_child("b", ta);
    router_b.add_child("a", tb);

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

/**
 * @brief SPEC{ NAME "type" <type>, NAME "name" <name>, SETTINGS "config"{ role,
 *        port, kind=webtransport [, addr] [, cert, key] [, ca] [, insecure]
 *        [, path] } }.
 *
 * `kind = "webtransport"` bound into the library's public SPEC builder (#902), including the
 * DIAL-side trust pair `ca`/`insecure` (#918) and the DIAL-side extended CONNECT `:path`
 * (#1023).
 */
view_t wt_conn_spec(std::string_view type, std::string_view name, tr::net::conn_role_t role,
                    std::uint16_t port, std::string_view addr = {}, std::string_view cert = {},
                    std::string_view key = {}, std::string_view hijack_key = {},
                    std::string_view ca = {}, std::optional<bool> insecure = std::nullopt,
                    std::string_view wt_path = {}) {
    tr::net::conn_spec_t spec(type, name);
    spec.role(role).port(port).kind("webtransport");
    if (!addr.empty()) spec.addr(addr);
    // The kind-private keys go through the generic pair setters (ADR-0043 §5).
    if (!cert.empty()) spec.text("cert", cert).text("key", key);
    if (!hijack_key.empty()) {
        // #927: a forward-compat pair a node that predates `hint` must skip WHOLE. Its
        // string VALUE spells `key`, so the every-offset scan re-read it as a key and
        // bound the FOLLOWING child as the private-key path, last-match-wins overriding
        // the legitimate one above. Two ordinary pairs on the wire — the builder emits
        // pairs, so the vector is still expressible without a hand-rolled emit.
        spec.text("hint", "key").text(hijack_key, "pem");
    }
    if (!ca.empty()) spec.text("ca", ca);
    if (insecure) spec.flag("insecure", *insecure);
    if (!wt_path.empty()) spec.text("path", wt_path);
    return spec.view();
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
    // ADR-0073 §4 (declared-only): the module names are minted HERE, by the application —
    // an external kind no longer inherits a library-derived "<kind>-client" name.
    (void)net_a.register_module("webtransport-client", "webtransport", tr::net::conn_role_t::DIAL);
    (void)net_b.register_module("webtransport-server", "webtransport",
                                tr::net::conn_role_t::LISTEN);

    // B: a webtransport LISTENER on a fixed localhost port, its dev cert/key
    // handed in as the kind-PRIVATE `cert`/`key` config keys.
    const auto wb = node_b.write(
        path_t("/net:children[]"),
        wt_conn_spec("listener", "a", tr::net::conn_role_t::LISTEN, 47133, {}, g_cert, g_key));
    check(wb.has_value(),
          "B: SPEC{listener, kind=webtransport, port, cert, key} constructs the listener");
    check(router_b.registry().by_name("net/webtransport-server/a") != nullptr,
          "B: the endpoint is wired into the router");

    // A missing cert/key on a webtransport LISTEN is a TYPE_MISMATCH.
    const auto bad = node_b.write(path_t("/net:children[]"),
                                  wt_conn_spec("listener", "bad", tr::net::conn_role_t::LISTEN, 0));
    check(!bad.has_value(), "B: a webtransport listener without cert/key fails creation");

    // #927 against the REAL parse_wt_config, over the REAL wire path (`:children[]` SPEC →
    // graph_t::create_child → webtransport_transport_factory). `parse_wt_config` kept a
    // hand-rolled every-offset copy of the SETTINGS walk after the other parsers moved to
    // net::config_reader_t, so this — the one shape that names a PRIVATE KEY FILE — was the
    // last place the defect lived. The hijack target does not exist on disk, so a listener
    // that adopted it could not load credentials and the write would be refused; the
    // observable is that the listener still comes up on the REAL dev key.
    const auto hj =
        node_b.write(path_t("/net:children[]"),
                     wt_conn_spec("listener", "hj", tr::net::conn_role_t::LISTEN, 47134, {}, g_cert,
                                  g_key, "/nonexistent/libtracer-attacker-key.pem"));
    check(hj.has_value(),
          "B: a forward-compat `hint=\"key\"` pair does NOT re-point the private key (#927)");
    check(router_b.registry().by_name("net/webtransport-server/hj") != nullptr,
          "B: the hijack-vector listener is wired in — it loaded the REAL dev key");

    // A: a webtransport CLIENT dialing B — a synchronous session from config.
    // The dial VERIFIES B's certificate (#918): B serves the self-signed dev cert,
    // so this names that very file as its `ca` bundle. Without a trust key the
    // session would be refused — asserted in test_spec_dial_trust_keys.
    const auto wa = node_a.write(path_t("/net:children[]"),
                                 wt_conn_spec("client", "b", tr::net::conn_role_t::DIAL, 47133,
                                              "127.0.0.1", {}, {}, {}, g_cert));
    check(wa.has_value(),
          "A: SPEC{client, kind=webtransport, addr, port, ca} constructs the dialing endpoint");
    const auto* s = net_a.settings_of("net/webtransport-client/b");
    check(s != nullptr && s->kind == "webtransport" && s->addr == "127.0.0.1" && s->port == 47133,
          "A: the parsed :settings carry kind/addr/port");
}

/**
 * @brief #918 — the webtransport mirror of the quic trust-key gate.
 *
 * The listeners serve the self-signed dev certificate (it chains to nothing in
 * the system trust store), and every leg asserts the HANDSHAKE outcome, not a
 * round-tripped config value. Five listeners because a webtransport listener
 * holds ONE session at a time.
 */
void test_spec_dial_trust_keys() {
    std::printf("SPEC dial certificate validation (#918):\n");
    graph_t node_a;
    graph_t node_b;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::fwd_router_t router_b(node_b);
    tr::net::transport_vertex_t net_a(node_a, router_a);
    tr::net::transport_vertex_t net_b(node_b, router_b);
    net_a.register_transport_type("webtransport", tr::net::webtransport_transport_factory());
    net_b.register_transport_type("webtransport", tr::net::webtransport_transport_factory());
    (void)net_a.register_module("webtransport-client", "webtransport", tr::net::conn_role_t::DIAL);
    (void)net_b.register_module("webtransport-server", "webtransport",
                                tr::net::conn_role_t::LISTEN);

    // 4716x, not 4715x: 47151 is bound wildcard by transport_vertex_test.cpp's udp listener,
    // and both bind INADDR_ANY, so under `ctest -j` in a LIBTRACER_WITH_QUIC=ON build whichever
    // lost the race reddened. 47155-47199 carry no other hardcoded port in the tree.
    // The hardcoded pattern itself is the weaker shape — transport_alloc_softfail_test.cpp:729
    // records why it moved to EPHEMERAL ports (a hardcoded pair makes the binary non-reentrant,
    // so two concurrent copies cross-deliver). These five need a port the dialer can name up
    // front, so they stay fixed for now; moving them to bind-then-read-back is tracked
    // separately.
    bool listening = true;
    for (const auto& [nm, port] : {std::pair<const char*, std::uint16_t>{"l1", 47160},
                                   {"l2", 47161},
                                   {"l3", 47162},
                                   {"l4", 47163},
                                   {"l5", 47164}}) {
        const auto w = node_b.write(
            path_t("/net:children[]"),
            wt_conn_spec("listener", nm, tr::net::conn_role_t::LISTEN, port, {}, g_cert, g_key));
        listening = listening && w.has_value();
    }
    check(listening, "B: five self-signed dev-cert listeners are up");

    // 1. No trust key: verification is the default, the dev cert validates against
    //    nothing, the session is REFUSED. This write SUCCEEDED before the fix.
    const auto plain = node_a.write(
        path_t("/net:children[]"),
        wt_conn_spec("client", "verify", tr::net::conn_role_t::DIAL, 47160, "127.0.0.1"));
    check(!plain.has_value() && plain.error() == tr::graph::status_t::TRANSPORT_DOWN,
          "A: a SPEC dial carrying no trust key is REFUSED — the peer cert does not validate");
    check(router_a.registry().by_name("net/webtransport-client/verify") == nullptr,
          "A: the refused dial leaves no endpoint behind");

    // 2. `insecure = 1` — the explicit DEV-ONLY opt-out reaches the dialer.
    const auto insec = node_a.write(path_t("/net:children[]"),
                                    wt_conn_spec("client", "insec", tr::net::conn_role_t::DIAL,
                                                 47161, "127.0.0.1", {}, {}, {}, {}, true));
    check(insec.has_value(), "A: `insecure = 1` connects to that same unvalidatable peer");

    // 3. `ca = <the peer's own cert>` — verification stays ON, against a private bundle.
    const auto with_ca = node_a.write(
        path_t("/net:children[]"), wt_conn_spec("client", "ca", tr::net::conn_role_t::DIAL, 47162,
                                                "127.0.0.1", {}, {}, {}, g_cert));
    check(with_ca.has_value(), "A: `ca = <the peer's cert>` connects with verification ON");

    // 4. `insecure = 0` is the explicit "verify" spelling, not a weaker opt-out.
    const auto zero = node_a.write(path_t("/net:children[]"),
                                   wt_conn_spec("client", "zero", tr::net::conn_role_t::DIAL, 47163,
                                                "127.0.0.1", {}, {}, {}, {}, false));
    check(!zero.has_value(), "A: `insecure = 0` still verifies — the dial is REFUSED");

    // 5. `ca = <an UNRELATED bundle>` — the bundle is genuinely consulted, not
    //    merely accepted: one that does not certify this peer still refuses.
    const auto wrong_ca = node_a.write(path_t("/net:children[]"),
                                       wt_conn_spec("client", "wrongca", tr::net::conn_role_t::DIAL,
                                                    47164, "127.0.0.1", {}, {}, {}, g_other_cert));
    check(!wrong_ca.has_value(), "A: `ca = <an unrelated CA>` is REFUSED — the bundle is applied");
}

/**
 * @brief #1023 — the DIAL-side `path` key carries the extended CONNECT `:path`.
 *
 * The observable is the path THAT REACHED THE SERVER, read off the listener's
 * accepted CONNECT (`session_path()`), not the config value round-tripped back
 * out of the dialer: the defect was that the factory hard-coded `"/"` into the
 * constructor, so every SPEC-created dialer requested the root whatever its
 * config said, and a server serving its session anywhere else was unreachable
 * without abandoning the creation SPEC for `provide_link`.
 *
 * The listeners are constructed DIRECTLY (not from a SPEC) because the test has
 * to HOLD the server object to interrogate it; the dialer — the thing under
 * test — goes through the real `:children[]` SPEC wire path every time. One
 * listener per leg: a webtransport endpoint carries one session at a time.
 *
 * Legs 4 and 5 are #1039: the value the key carries must be origin-form, and
 * one that is not is refused at creation instead of put on the wire.
 */
void test_spec_dial_connect_path() {
    std::printf("SPEC dial extended-CONNECT :path (#1023):\n");
    graph_t node_a;
    tr::net::fwd_router_t router_a(node_a);
    tr::net::transport_vertex_t net_a(node_a, router_a);
    net_a.register_transport_type("webtransport", tr::net::webtransport_transport_factory());
    (void)net_a.register_module("webtransport-client", "webtransport", tr::net::conn_role_t::DIAL);

    webtransport_transport_t served(std::uint16_t{0}, g_cert, g_key);
    check(served.ok(), "a directly-constructed listener is up");
    check(served.session_path().empty(), "no session yet — the listener has accepted no CONNECT");

    // 1. `path = "/tracer"` — the key reaches the wire. Verification stays ON
    //    (`ca` = the peer's own self-signed cert), so this is a real session.
    const auto named = node_a.write(
        path_t("/net:children[]"),
        wt_conn_spec("client", "named", tr::net::conn_role_t::DIAL, served.local_port(),
                     "127.0.0.1", {}, {}, {}, g_cert, std::nullopt, "/tracer"));
    check(named.has_value(), "A: SPEC{client, ..., path=\"/tracer\"} constructs the dialer");
    check(served.session_path() == "/tracer",
          "the CONNECT that reached the server named /tracer — the SPEC key is on the wire");

    // 2. No `path` key: the "/" default the factory used to hard-code is preserved.
    webtransport_transport_t defaulted(std::uint16_t{0}, g_cert, g_key);
    const auto plain =
        node_a.write(path_t("/net:children[]"),
                     wt_conn_spec("client", "plain", tr::net::conn_role_t::DIAL,
                                  defaulted.local_port(), "127.0.0.1", {}, {}, {}, g_cert));
    check(plain.has_value(), "A: a SPEC with no `path` key still constructs the dialer");
    check(defaulted.session_path() == "/", "a SPEC with no `path` key dials the / default");

    // 3. The empty-string normalisation the factory leans on (an absent key
    //    parses to an empty string, which the constructor turns into "/").
    webtransport_transport_t empty_path(std::uint16_t{0}, g_cert, g_key);
    webtransport_transport_t dialer("127.0.0.1", empty_path.local_port(), "", dev_tls());
    check(dialer.ok(), "a direct dial with an EMPTY path establishes its session");
    check(empty_path.session_path() == "/", "an empty path is normalised to / before the CONNECT");

    // 4. #1039 — a non-empty `path` that is not origin-form. RFC 9114 §4.3.1 /
    //    RFC 9113 §8.3.1: an `https` request's `:path` is non-empty and, in
    //    origin-form, begins with "/". `"tracer"` therefore cannot be right,
    //    and the factory refuses it at creation beside the empty-`addr` /
    //    zero-`port` preconditions instead of emitting it.
    //
    //    THIS LISTENER CANNOT SEE THE DEFECT: its accept arm validates
    //    `:method`/`:protocol` and serves every resource, so before the fix the
    //    malformed CONNECT SUCCEEDED here — measured on unmodified main, the
    //    write returned a value and the listener reported `session_path` =
    //    "tracer", `session_up` = 1. All three checks below redden there.
    //
    //    The "no socket was opened" observable is the listener's: a dialer that
    //    was never constructed cannot handshake, so this fresh listener has
    //    accepted no CONNECT and reports no session.
    webtransport_transport_t untouched(std::uint16_t{0}, g_cert, g_key);
    check(untouched.ok(), "a fresh listener for the refusal leg is up");
    const auto bare = node_a.write(
        path_t("/net:children[]"),
        wt_conn_spec("client", "bare", tr::net::conn_role_t::DIAL, untouched.local_port(),
                     "127.0.0.1", {}, {}, {}, g_cert, std::nullopt, "tracer"));
    check(!bare.has_value() && bare.error() == tr::graph::status_t::TYPE_MISMATCH,
          "A: SPEC{client, ..., path=\"tracer\"} is REFUSED with TYPE_MISMATCH");
    check(untouched.session_path().empty() && !untouched.session_up(),
          "the refused creation dialled nothing — the listener accepted no CONNECT");
    check(router_a.registry().by_name("net/webtransport-client/bare") == nullptr,
          "A: the refused creation leaves no endpoint behind");

    // 5. The rule is the leading "/", not the spelling of leg 1: a deeper
    //    origin-form path still reaches the wire untouched.
    webtransport_transport_t deep(std::uint16_t{0}, g_cert, g_key);
    const auto nested =
        node_a.write(path_t("/net:children[]"),
                     wt_conn_spec("client", "deep", tr::net::conn_role_t::DIAL, deep.local_port(),
                                  "127.0.0.1", {}, {}, {}, g_cert, std::nullopt, "/a/b"));
    check(nested.has_value(), "A: SPEC{client, ..., path=\"/a/b\"} still constructs the dialer");
    check(deep.session_path() == "/a/b", "the origin-form path reached the server verbatim");
}

// ---- LISTEN-side classifier vectors: the raw peer lives in raw_wt_client.hpp ----

/** @brief A delivered frame against the raw octets that were sent — BYTE
 *         identity, not just length: interleaving two streams into one
 *         reassembler can produce a same-length frame of the wrong bytes. */
[[nodiscard]] bool same(const std::vector<std::byte>& got, const std::vector<std::uint8_t>& want) {
    return got.size() == want.size() && std::memcmp(got.data(), want.data(), want.size()) == 0;
}

/** @brief Block until the LISTEN side reports its session up, or the budget expires. */
[[nodiscard]] bool wait_session(const webtransport_transport_t& t,
                                std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (!t.session_up()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

/**
 * @brief #919 — which 0x41 stream may become the frame channel.
 *
 * One vector per guard (the mutation-sweep rule: a combined misbehaviour cannot
 * tell which guard fired), each on its OWN listener + connection. Separate
 * connections are not cosmetic: the server grants a peer four concurrent
 * bidirectional streams and never closes a refused handle before teardown, so a
 * refused stream keeps its credit and a single connection would starve.
 */
void test_frame_stream_adoption_gate() {
    std::printf("WebTransport — 0x41 adoption is session-gated, id-checked and single (#919):\n");

    // Guard 1: a 0x41 stream BEFORE the extended CONNECT — the handshake bypass.
    {
        frame_sink_t sink;
        auto rx = [&](std::span<const std::byte> f) { sink.push(f); };
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        listener.set_receiver(rx);
        raw_wt_client_t cli(listener.local_port());
        check(cli.ok, "raw msquic h3 client connected (SETTINGS + QPACK streams sent)");

        auto* early = cli.open_bidi();
        cli.write(early, wt_preamble(0));
        cli.write(early, record(payload(6, 0x70)));
        check(raw_wt_client_t::wait_aborted(early, 3000ms),
              "guard 1: a 0x41 stream opened BEFORE the CONNECT is aborted");
        check(!cli.shut.load(std::memory_order_relaxed),
              "guard 1: refusal is STREAM-scoped — the connection stays up");
        check(!listener.session_up(), "guard 1: no session was bypassed into existence");

        // The same connection still establishes a legitimate session afterwards.
        auto* connect = cli.open_bidi();
        cli.write(connect, connect_frame("127.0.0.1:0"));
        check(wait_session(listener, 3000ms), "guard 1: the extended CONNECT still succeeds after");
        auto* frames = cli.open_bidi();
        cli.write(frames, wt_preamble(connect->id));
        const auto good = payload(7, 0x31);
        cli.write(frames, record(good));
        check(sink.wait_for_count(1, 3000ms), "guard 1: the legitimate frame channel is adopted");
        check(sink.count() == 1 && same(sink.at(0), good),
              "guard 1: exactly the legitimate frame arrived — the refused stream fed nothing");
    }

    // Guard 2: a 0x41 stream naming a session id that is not the CONNECT stream's.
    {
        frame_sink_t sink;
        auto rx = [&](std::span<const std::byte> f) { sink.push(f); };
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        listener.set_receiver(rx);
        raw_wt_client_t cli(listener.local_port());
        auto* connect = cli.open_bidi();
        cli.write(connect, connect_frame("127.0.0.1:0"));
        check(wait_session(listener, 3000ms), "guard 2: the session is up");

        auto* wrong = cli.open_bidi();
        cli.write(wrong, wt_preamble(connect->id + 4));  // a valid, different stream id
        cli.write(wrong, record(payload(6, 0x90)));
        check(raw_wt_client_t::wait_aborted(wrong, 3000ms),
              "guard 2: a 0x41 stream naming the WRONG session id is aborted");
        check(listener.session_up(), "guard 2: the established session survives the refusal");

        auto* frames = cli.open_bidi();
        cli.write(frames, wt_preamble(connect->id));
        const auto good = payload(9, 0x44);
        cli.write(frames, record(good));
        check(sink.wait_for_count(1, 3000ms), "guard 2: the correctly-addressed stream IS adopted");
        check(sink.count() == 1 && same(sink.at(0), good),
              "guard 2: only the correctly-addressed stream's frame arrived");
    }

    // Guard 3: a SECOND 0x41 stream after adoption — the framer-corruption arm.
    {
        frame_sink_t sink;
        auto rx = [&](std::span<const std::byte> f) { sink.push(f); };
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        listener.set_receiver(rx);
        raw_wt_client_t cli(listener.local_port());
        auto* connect = cli.open_bidi();
        cli.write(connect, connect_frame("127.0.0.1:0"));
        check(wait_session(listener, 3000ms), "guard 3: the session is up");

        auto* first = cli.open_bidi();
        cli.write(first, wt_preamble(connect->id));
        const auto r1 = payload(5, 0x11);
        const auto r2 = payload(13, 0x22);
        cli.write(first, record(r1));
        cli.write(first, record(r2));
        check(sink.wait_for_count(2, 3000ms), "guard 3: the first frame channel is adopted");

        // A second, equally well-formed 0x41 stream injecting its own records.
        auto* second = cli.open_bidi();
        cli.write(second, wt_preamble(connect->id));
        cli.write(second, record(payload(4, 0xDE)));
        check(raw_wt_client_t::wait_aborted(second, 3000ms),
              "guard 3: a SECOND 0x41 stream is refused — the first adoption wins");
        check(listener.session_up(), "guard 3: the live session survives the second stream");

        // Framer integrity: the shared reassembler saw stream one and nothing else.
        const auto r3 = payload(21, 0x33);
        cli.write(first, record(r3));
        check(sink.wait_for_count(3, 3000ms), "guard 3: the adopted stream keeps delivering");
        std::this_thread::sleep_for(250ms);  // let any injected record land if it were adopted
        check(sink.count() == 3, "guard 3: exactly three frames — the refused stream fed nothing");
        check(sink.count() == 3 && same(sink.at(0), r1) && same(sink.at(1), r2) &&
                  same(sink.at(2), r3),
              "guard 3: the three frames are the first stream's, byte-identical and in order");
        check(listener.malformed_rx() == 0,
              "guard 3: the length-prefix reassembler never desynced (no interleaving)");
    }
}

/**
 * @brief #920 — an unknown/GREASE H3 frame is skipped by its length, not fatal.
 *
 * The deliberate opposite pull from #919 above: identity is pinned, unknown
 * extensions are ignored (RFC 9114 §7.2.8), because §9 has conformant peers
 * emit reserved types precisely to catch endpoints that treat them as errors.
 */
void test_unknown_h3_frames_skipped() {
    std::printf("WebTransport — unknown/GREASE H3 frames are skipped (RFC 9114 §7.2.8, §9):\n");

    // Chrome's real shape: reserved frames ahead of the extended CONNECT.
    {
        frame_sink_t sink;
        auto rx = [&](std::span<const std::byte> f) { sink.push(f); };
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        listener.set_receiver(rx);
        raw_wt_client_t cli(listener.local_port());
        check(cli.ok, "raw msquic h3 client connected");

        auto* connect = cli.open_bidi();
        std::vector<std::uint8_t> bytes = grease_frame(0, 5);  // type 0x21, 1-byte varint
        const auto g2 = grease_frame(8, 0);                    // type 0x119, 2-byte varint, empty
        bytes.insert(bytes.end(), g2.begin(), g2.end());
        const auto hdr = connect_frame("127.0.0.1:0");
        bytes.insert(bytes.end(), hdr.begin(), hdr.end());
        cli.write(connect, bytes);
        check(wait_session(listener, 3000ms),
              "GREASE frames before the extended CONNECT are skipped — the session establishes");
        check(!cli.shut.load(std::memory_order_relaxed), "the connection was NOT torn down");
        check(raw_wt_client_t::wait_rx(connect, 1, 3000ms),
              "the 200 response really came back on the wire (not just a flipped flag)");

        // A post-CONNECT GREASE frame on the request stream changes nothing.
        cli.write(connect, grease_frame(3, 7));
        auto* frames = cli.open_bidi();
        cli.write(frames, wt_preamble(connect->id));
        const auto good = payload(12, 0x61);
        cli.write(frames, record(good));
        check(sink.wait_for_count(1, 3000ms), "frames flow across the skipped GREASE frames");
        check(sink.count() == 1 && same(sink.at(0), good), "the frame is byte-identical");
        check(listener.session_up(), "a post-CONNECT GREASE frame leaves the session alone");
    }

    // Ignoring the TYPE is obligatory; buffering an arbitrary body is not. The
    // pair is what makes the bound observable — a kilobyte-scale GREASE body is
    // skipped, and only the declared length beyond the cap is refused. Without
    // the first leg "refused" would also be satisfied by refusing everything.
    {
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        raw_wt_client_t cli(listener.local_port());
        auto* connect = cli.open_bidi();
        std::vector<std::uint8_t> big = grease_frame(5, 8'000);  // under the cap
        const auto hdr = connect_frame("127.0.0.1:0");
        big.insert(big.end(), hdr.begin(), hdr.end());
        cli.write(connect, big);
        check(wait_session(listener, 3000ms),
              "an 8 KiB unknown frame is skipped whole — the session still establishes");
    }
    {
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        raw_wt_client_t cli(listener.local_port());
        auto* s = cli.open_bidi();
        std::vector<std::uint8_t> huge;
        tr::net::wt_h3::append_varint(huge, grease_type(5));
        tr::net::wt_h3::append_varint(huge, 100'000);  // beyond the handshake cap
        cli.write(s, huge);
        check(cli.wait_shutdown(3000ms),
              "an unknown frame declaring more than the handshake cap is refused");
    }

    // A truncated unknown frame parks classification; the rest completes it.
    {
        webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
        raw_wt_client_t cli(listener.local_port());
        auto* connect = cli.open_bidi();
        const auto whole = grease_frame(2, 16);
        const std::size_t cut = whole.size() - 9;
        cli.write(connect, std::span<const std::uint8_t>(whole).first(cut));
        std::this_thread::sleep_for(250ms);
        check(!listener.session_up() && !cli.shut.load(std::memory_order_relaxed),
              "a truncated unknown frame neither completes nor kills the connection");
        std::vector<std::uint8_t> rest(whole.begin() + static_cast<std::ptrdiff_t>(cut),
                                       whole.end());
        const auto hdr = connect_frame("127.0.0.1:0");
        rest.insert(rest.end(), hdr.begin(), hdr.end());
        cli.write(connect, rest);
        check(wait_session(listener, 3000ms),
              "classification resumes once the skipped frame's bytes arrive");
    }
}

/**
 * @brief #1163 — a peer that opens and closes streams must not grow the endpoint forever.
 *
 * The leak: `impl_t::ctxs` gained one entry per peer stream and had no `erase` anywhere in the
 * TU. The only frees were two WHOLESALE harvests — peer replacement and endpoint teardown — so
 * every finished stream kept its `stream_ctx_t`, its `acc` buffer and its unreleased msquic
 * handle for the life of the SESSION, and the peer chooses how many that is.
 *
 * Why the msquic caps do not already bound it: `PeerUnidiStreamCount = 8` limits how many
 * streams may be open AT ONCE, not how many may be opened over a session's life. The vector
 * below therefore CYCLES — open, close, repeat — because a test that opens N streams once
 * proves nothing: a single generation stays under the concurrency cap and leaks invisibly.
 *
 * Non-vacuity, and why it is not the usual "assert it grew" shape: with the reclamation
 * reverted, a closed stream's handle is never released, so its flow-control credit is never
 * returned either. After the 8th cycle the client cannot start another stream, `live_streams()`
 * stays pinned at its post-handshake baseline + 8, and the settle loop below times out. The
 * assertion reds rather than hanging because it never waits on the CLIENT to make progress —
 * only on the server's count to come back down.
 */
void test_peer_stream_cycling_is_bounded() {
    std::printf("WebTransport — cycling peer streams does not grow the endpoint (#1163):\n");
    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
    raw_wt_client_t cli(listener.local_port());

    // Establish the session first, so the baseline includes the H3 face and the CONNECT
    // stream and the cycled streams are the only thing that can move the count.
    auto* connect = cli.open_bidi();
    cli.write(connect, connect_frame("127.0.0.1:0"));
    check(wait_session(listener, 3000ms), "session established (baseline is now stable)");

    const std::size_t base = listener.live_streams();
    check(base > 0, "the live session holds its own streams (a zero baseline would be vacuous)");

    // 40 cycles is 5x the concurrency cap: enough that a leak cannot hide inside the credit
    // window, and enough that the pre-fix build is starved long before the last round.
    constexpr int kCycles = 40;
    std::size_t peak = base;
    for (int i = 0; i < kCycles; ++i) {
        raw_wt_client_t::wt_stream_t* const s = cli.open_stream(true);
        // One byte of an unknown unidirectional stream type: enough to make the server
        // classify and hold a ctx, without pretending to be a stream it would adopt.
        const std::uint8_t kGreaseType = 0x21;
        cli.write(s, std::span<const std::uint8_t>(&kGreaseType, 1));
        std::this_thread::sleep_for(10ms);
        peak = std::max(peak, listener.live_streams());
        cli.close_stream(s);
    }

    // Reclamation is a callback on an msquic worker, so settle rather than sample once.
    const auto deadline = std::chrono::steady_clock::now() + 5000ms;
    while (listener.live_streams() > base && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);

    const std::size_t after = listener.live_streams();
    std::printf("  baseline=%zu peak=%zu after=%zu over %d cycles\n", base, peak, after, kCycles);
    check(after <= base, "every cycled stream was reclaimed — the count returned to baseline");
    check(peak < base + kCycles, "the count never accumulated one entry per stream ever opened");
    check(listener.session_up(), "and the session survived the cycling (stream-scoped, not fatal)");
}

/**
 * @brief #1101 — a `defer_rx` DIAL link delivers NOTHING until start_receiving(), so a frame
 *        the server pushes the instant the session comes up cannot land in an empty sink.
 *
 * The window is `transport_vertex_t::make_connection`: it installs the receiver several steps
 * after the link object exists, and this transport owns no receive thread to withhold —
 * msquic's worker drives every RECEIVE, and the DIAL constructor opens the frame channel
 * itself, so a server writing back on it reaches `rx->deliver` before the constructor even
 * returns. An empty `receiver_slot_t` drops that frame silently: no `dropped_rx()`, no
 * `malformed_rx()`, a healthy-looking session.
 *
 * The hold is msquic's own per-stream receive window (ADR-0081 §2) — nothing is parked
 * library-side, which is the standing no-library-internal-buffers commitment (ADR-0042 §2).
 *
 * The guard does not race the window, and it is not vacuous:
 *
 *  - the dialer's sink is installed IMMEDIATELY after construction, so the silence below can
 *    only be the un-armed link, never a missing sink;
 *  - the server pushes only AFTER a frame has travelled dialer→server, which is proof the
 *    server has ADOPTED the frame channel (that frame arrives through the very ctx that sets
 *    `frame_stream`) — so its push cannot be a send dropped for want of a stream;
 *  - SILENCE for a generous window, with both counters still 0;
 *  - the FRAME once `start_receiving()` runs — the positive control that keeps the silence
 *    from being vacuous: the bytes were really there, held in msquic's window, and really
 *    decodable.
 *
 * Reverting the gate (constructing with `defer_rx = false`, or removing the zero-byte drain
 * in `on_stream_rx`) reddens this in one run: the pushed frame is delivered inside the
 * silence window instead of after the arming.
 */
void test_push_on_session_waits_for_start_receiving() {
    std::printf("WebTransport — a push-on-session frame waits for the sink (#1101):\n");
    // The sinks outlive the transports that deliver to them (this file's destruction idiom).
    frame_sink_t at_server, at_dialer;
    auto server_rx = [&](std::span<const std::byte> f) { at_server.push(f); };
    auto dialer_rx = [&](std::span<const std::byte> f) { at_dialer.push(f); };

    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
    check(listener.ok(), "listener started (ALPN h3, ephemeral port, dev cert)");
    listener.set_receiver(server_rx);
    {
        webtransport_transport_t dialer("127.0.0.1", listener.local_port(), "/", dev_tls(),
                                        &tr::mem::heap_backend(), /*max_frame=*/0,
                                        /*defer_rx=*/true);
        check(dialer.ok(), "the deferred dialer still completed CONNECT + 200 in its constructor");
        check(dialer.session_up(),
              "the H3 handshake ran to completion while delivery was held (the gate is on "
              "delivery, never on the state machine)");
        // Installed BEFORE the window: what holds the push back below is the closed gate.
        dialer.set_receiver(dialer_rx);

        // Adoption proof: the server's send() drops until it has adopted the 0x41 stream, so
        // one frame the other way first — it arrives through that very ctx.
        const auto ping = test_frame(4, 0x01);
        dialer.send(ping);
        check(at_server.wait_for_count(1, 3000ms),
              "a dialer->server frame arrived: the server has adopted the frame channel");

        const auto pushed = test_frame(11, 0x70);
        listener.send(pushed);

        // Orders of magnitude more than msquic needs to indicate a frame already on the wire.
        std::this_thread::sleep_for(500ms);
        check(at_dialer.count() == 0,
              "the deferred link delivered NOTHING for the whole window: the bytes wait in "
              "msquic's flow-control window");
        check(dialer.dropped_rx() == 0 && dialer.malformed_rx() == 0,
              "and they were not shed under another name either (both counters still 0)");

        dialer.start_receiving();
        check(at_dialer.wait_for_count(1, 4000ms),
              "after start_receiving() the pushed frame was DELIVERED, not dropped");
        check(at_dialer.count() == 1 && at_dialer.at(0) == pushed,
              "carrying the pushed frame's own bytes");
        check(dialer.dropped_rx() == 0 && dialer.malformed_rx() == 0,
              "delivered with both counters still 0");

        // Idempotent: a second arming on an armed link leaves it delivering.
        dialer.start_receiving();
        const auto after = test_frame(7, 0x90);
        listener.send(after);
        check(at_dialer.wait_for_count(2, 3000ms),
              "a second start_receiving() is inert — the link keeps delivering");
        check(at_dialer.count() == 2 && at_dialer.at(1) == after, "and the second frame is intact");
    }
}

/**
 * @brief #1101 — `start_receiving()` is inert on every link that has no gate to open.
 *
 * `transport_vertex_t::make_connection` calls it unconditionally on every link it wires, so
 * the three shapes that are NOT a deferred DIAL must all be safe: a ONE-PHASE dialer (never
 * held), a LISTENER (its peer's frame channel is not gated), and a dial that NEVER CAME UP
 * (no frame stream to re-enable — the ADR-0081 idempotence requirement).
 */
void test_start_receiving_is_inert_where_it_must_be() {
    std::printf("WebTransport — start_receiving() is inert where it must be (#1101):\n");
    frame_sink_t at_server, at_dialer;
    auto server_rx = [&](std::span<const std::byte> f) { at_server.push(f); };
    auto dialer_rx = [&](std::span<const std::byte> f) { at_dialer.push(f); };

    webtransport_transport_t listener(std::uint16_t{0}, g_cert, g_key);
    listener.set_receiver(server_rx);
    listener.start_receiving();  // a LISTEN link: nothing to arm
    listener.start_receiving();
    {
        // A ONE-PHASE dialer (the historical shape): armed or not, it delivers.
        webtransport_transport_t dialer("127.0.0.1", listener.local_port(), "/", dev_tls());
        check(dialer.ok(), "one-phase dialer up");
        dialer.set_receiver(dialer_rx);
        dialer.start_receiving();
        dialer.start_receiving();

        const auto f1 = test_frame(5, 0x21);
        dialer.send(f1);
        check(at_server.wait_for_count(1, 3000ms), "the un-gated dialer still delivers upstream");
        const auto f2 = test_frame(6, 0x22);
        listener.send(f2);
        check(at_dialer.wait_for_count(1, 3000ms),
              "and the armed listener still delivers downstream");
        check(at_dialer.count() == 1 && at_dialer.at(0) == f2, "with the frame intact");
    }
    // A dial that NEVER CAME UP: the wrong-CA vector (#918) refuses the session, so this link
    // has no frame stream at all. Arming it must be a safe no-op, not a null-handle call.
    // Its own listener — a webtransport endpoint carries ONE session at a time, the reason
    // test_spec_dial_trust_keys stands up one listener per leg.
    webtransport_transport_t listener2(std::uint16_t{0}, g_cert, g_key);
    webtransport_transport_t dead("127.0.0.1", listener2.local_port(), "/",
                                  webtransport_dial_tls_t{.ca_file = g_other_cert},
                                  &tr::mem::heap_backend(), /*max_frame=*/0, /*defer_rx=*/true);
    check(!dead.ok(), "the wrong-CA dial did not come up (no session, no frame stream)");
    dead.start_receiving();
    dead.start_receiving();
    check(!dead.session_up(), "arming a link that never came up changed nothing");
}

}  // namespace

int main() {
    // Generate the self-signed dev pair once (tools/gen-dev-cert.sh) — the
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
    // A second, unrelated pair for the wrong-CA-bundle vector (#918).
    const std::string other_dir = std::string(dir) + "/other";
    const std::string cmd2 = std::string("sh ") + LIBTRACER_DEV_CERT_SCRIPT + " " + other_dir;
    if (std::system(cmd2.c_str()) != 0) {
        std::printf("FAIL: gen-dev-cert.sh (second pair)\n");
        return 1;
    }
    g_other_cert = other_dir + "/cert.pem";

    test_wt_h3_varint();
    test_wt_h3_huffman();
    test_wt_h3_field_sections();
    test_session_and_raw_duplex();
    test_big_frame_chunking();
    test_send_overload_parity();
    test_tx_drop_counters();
    test_view_delivery_and_backpressure();
    test_fwd_read_round_trip();
    test_config_constructed_webtransport();
    test_spec_dial_trust_keys();
    test_spec_dial_connect_path();
    test_frame_stream_adoption_gate();
    test_unknown_h3_frames_skipped();
    test_peer_stream_cycling_is_bounded();
    test_push_on_session_waits_for_start_receiving();
    test_start_receiving_is_inert_where_it_must_be();
    return tr::testing::summary("webtransport");
}
