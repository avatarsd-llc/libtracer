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
#include "libtracer/byteorder.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_webtransport.hpp"

// The raw msquic H3 client below crosses the same non-TSan-instrumented library
// boundary as transport_webtransport.cpp: restate msquic's StreamSend ->
// SEND_COMPLETE and callback-serialization happens-before edges for TSan (see
// the matching annotations + rationale in src/msquic_endpoint.hpp). No-ops
// outside TSan.
#if defined(__SANITIZE_THREAD__)
#define LIBTRACER_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define LIBTRACER_TSAN 1
#endif
#endif
#ifdef LIBTRACER_TSAN
#include <sanitizer/tsan_interface.h>
#endif

namespace {

/** @brief Publish everything written so far to whoever acquires @p p next. */
inline void tsan_release(void* p) {
#ifdef LIBTRACER_TSAN
    __tsan_release(p);
#else
    (void)p;
#endif
}

/** @brief Take the writes published by the last release on @p p. */
inline void tsan_acquire(void* p) {
#ifdef LIBTRACER_TSAN
    __tsan_acquire(p);
#else
    (void)p;
#endif
}

/** @brief RAII edge for one msquic callback invocation: acquire in, release out. */
struct tsan_cb_guard_t {
    void* p; /**< @brief The object the edge is annotated on. */
    explicit tsan_cb_guard_t(void* ptr) : p(ptr) { tsan_acquire(p); }
    ~tsan_cb_guard_t() { tsan_release(p); }
};

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

/**
 * @brief A collecting sink: frames delivered on an msquic worker thread, read from the test thread.
 */
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
    // Sinks + named receiver lambdas BEFORE the transports: the slot binds the
    // callable by address, and the transport dtor drains msquic callbacks.
    sink_t at_listener, at_dialer;
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
    sink_t sink;
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
    sink_t sink;
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
 *        port, kind=webtransport [, addr] [, cert, key] [, ca] [, insecure] } }.
 *
 * The quic_test conn_spec shape, including the DIAL-side trust pair
 * `ca`/`insecure` (#918).
 */
view_t conn_spec(std::string_view type, std::string_view name, tr::net::conn_role_t role,
                 std::uint16_t port, std::string_view addr = {}, std::string_view cert = {},
                 std::string_view key = {}, std::string_view hijack_key = {},
                 std::string_view ca = {}, std::optional<bool> insecure = std::nullopt) {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(role)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    tr::wire::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);
    tr::wire::emit_name(cfg, "kind");
    tr::wire::emit_name(cfg, "webtransport");
    if (!addr.empty()) {
        tr::wire::emit_name(cfg, "addr");
        tr::wire::emit_name(cfg, addr);
    }
    if (!cert.empty()) {
        tr::wire::emit_name(cfg, "cert");
        tr::wire::emit_name(cfg, cert);
        tr::wire::emit_name(cfg, "key");
        tr::wire::emit_name(cfg, key);
    }
    if (!hijack_key.empty()) {
        // #927: a forward-compat pair a node that predates `hint` must skip WHOLE. Its
        // string VALUE spells `key`, so the every-offset scan re-read it as a key and
        // bound the FOLLOWING child as the private-key path, last-match-wins overriding
        // the legitimate one above.
        tr::wire::emit_name(cfg, "hint");
        tr::wire::emit_name(cfg, "key");
        tr::wire::emit_name(cfg, hijack_key);
        tr::wire::emit_name(cfg, "pem");
    }
    if (!ca.empty()) {
        tr::wire::emit_name(cfg, "ca");
        tr::wire::emit_name(cfg, ca);
    }
    if (insecure) {
        tr::wire::emit_name(cfg, "insecure");
        const std::byte iv{static_cast<std::uint8_t>(*insecure ? 1 : 0)};
        tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&iv, 1));
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
        conn_spec("listener", "a", tr::net::conn_role_t::LISTEN, 47133, {}, g_cert, g_key));
    check(wb.has_value(),
          "B: SPEC{listener, kind=webtransport, port, cert, key} constructs the listener");
    check(router_b.registry().by_name("net/webtransport-server/a") != nullptr,
          "B: the endpoint is wired into the router");

    // A missing cert/key on a webtransport LISTEN is a TYPE_MISMATCH.
    const auto bad = node_b.write(path_t("/net:children[]"),
                                  conn_spec("listener", "bad", tr::net::conn_role_t::LISTEN, 0));
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
                     conn_spec("listener", "hj", tr::net::conn_role_t::LISTEN, 47134, {}, g_cert,
                               g_key, "/nonexistent/libtracer-attacker-key.pem"));
    check(hj.has_value(),
          "B: a forward-compat `hint=\"key\"` pair does NOT re-point the private key (#927)");
    check(router_b.registry().by_name("net/webtransport-server/hj") != nullptr,
          "B: the hijack-vector listener is wired in — it loaded the REAL dev key");

    // A: a webtransport CLIENT dialing B — a synchronous session from config.
    // The dial VERIFIES B's certificate (#918): B serves the self-signed dev cert,
    // so this names that very file as its `ca` bundle. Without a trust key the
    // session would be refused — asserted in test_spec_dial_trust_keys.
    const auto wa =
        node_a.write(path_t("/net:children[]"), conn_spec("client", "b", tr::net::conn_role_t::DIAL,
                                                          47133, "127.0.0.1", {}, {}, {}, g_cert));
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
            conn_spec("listener", nm, tr::net::conn_role_t::LISTEN, port, {}, g_cert, g_key));
        listening = listening && w.has_value();
    }
    check(listening, "B: five self-signed dev-cert listeners are up");

    // 1. No trust key: verification is the default, the dev cert validates against
    //    nothing, the session is REFUSED. This write SUCCEEDED before the fix.
    const auto plain =
        node_a.write(path_t("/net:children[]"),
                     conn_spec("client", "verify", tr::net::conn_role_t::DIAL, 47160, "127.0.0.1"));
    check(!plain.has_value() && plain.error() == tr::graph::status_t::NOT_FOUND,
          "A: a SPEC dial carrying no trust key is REFUSED — the peer cert does not validate");
    check(router_a.registry().by_name("net/webtransport-client/verify") == nullptr,
          "A: the refused dial leaves no endpoint behind");

    // 2. `insecure = 1` — the explicit DEV-ONLY opt-out reaches the dialer.
    const auto insec = node_a.write(path_t("/net:children[]"),
                                    conn_spec("client", "insec", tr::net::conn_role_t::DIAL, 47161,
                                              "127.0.0.1", {}, {}, {}, {}, true));
    check(insec.has_value(), "A: `insecure = 1` connects to that same unvalidatable peer");

    // 3. `ca = <the peer's own cert>` — verification stays ON, against a private bundle.
    const auto with_ca = node_a.write(path_t("/net:children[]"),
                                      conn_spec("client", "ca", tr::net::conn_role_t::DIAL, 47162,
                                                "127.0.0.1", {}, {}, {}, g_cert));
    check(with_ca.has_value(), "A: `ca = <the peer's cert>` connects with verification ON");

    // 4. `insecure = 0` is the explicit "verify" spelling, not a weaker opt-out.
    const auto zero = node_a.write(path_t("/net:children[]"),
                                   conn_spec("client", "zero", tr::net::conn_role_t::DIAL, 47163,
                                             "127.0.0.1", {}, {}, {}, {}, false));
    check(!zero.has_value(), "A: `insecure = 0` still verifies — the dial is REFUSED");

    // 5. `ca = <an UNRELATED bundle>` — the bundle is genuinely consulted, not
    //    merely accepted: one that does not certify this peer still refuses.
    const auto wrong_ca = node_a.write(
        path_t("/net:children[]"), conn_spec("client", "wrongca", tr::net::conn_role_t::DIAL, 47164,
                                             "127.0.0.1", {}, {}, {}, g_other_cert));
    check(!wrong_ca.has_value(), "A: `ca = <an unrelated CA>` is REFUSED — the bundle is applied");
}

// ---- a raw msquic H3 client: the test's hand on the LISTEN-side classifier ----

/** @brief A reserved (GREASE) H3 frame type — RFC 9114 §7.2.8's `0x1f * N + 0x21`. */
constexpr std::uint64_t grease_type(std::uint64_t n) { return 0x1f * n + 0x21; }

/** @brief The WebTransport bidirectional-stream preamble: 0x41 ++ the session id
 *         (draft-ietf-webtrans-http3 §4.2 — that id IS the CONNECT stream's id). */
std::vector<std::uint8_t> wt_preamble(std::uint64_t session_id) {
    std::vector<std::uint8_t> p;
    tr::net::wt_h3::append_varint(p, tr::net::wt_h3::kFrameWtStream);
    tr::net::wt_h3::append_varint(p, session_id);
    return p;
}

/** @brief One length-prefixed record on the frame channel: u32-LE len ++ payload. */
std::vector<std::uint8_t> record(std::span<const std::uint8_t> payload) {
    std::vector<std::byte> prefix;
    tr::detail::append_le(prefix, static_cast<std::uint32_t>(payload.size()));
    std::vector<std::uint8_t> out;
    for (const std::byte b : prefix) out.push_back(static_cast<std::uint8_t>(b));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

/** @brief A payload of @p len bytes, the `test_frame` pattern as raw octets. */
std::vector<std::uint8_t> payload(std::size_t len, std::uint8_t seed) {
    std::vector<std::uint8_t> p(len);
    for (std::size_t i = 0; i < len; ++i) p[i] = static_cast<std::uint8_t>(seed + i);
    return p;
}

/** @brief The extended CONNECT request frame (HEADERS) a browser-shaped client sends. */
std::vector<std::uint8_t> connect_frame(std::string_view authority) {
    std::vector<std::uint8_t> out;
    tr::net::wt_h3::append_h3_frame(out, tr::net::wt_h3::kFrameHeaders,
                                    tr::net::wt_h3::encode_connect_field_section(authority, "/"));
    return out;
}

/** @brief One complete GREASE frame: reserved type @p n with a @p body-byte payload. */
std::vector<std::uint8_t> grease_frame(std::uint64_t n, std::size_t body) {
    const std::vector<std::uint8_t> b(body, 0x5A);
    std::vector<std::uint8_t> out;
    tr::net::wt_h3::append_h3_frame(out, grease_type(n), b);
    return out;
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
 * @brief A raw msquic HTTP/3 client — the test's hand on the server's stream
 *        classifier (the `raw_quic_client_t` role, one layer up).
 *
 * The library's own DIAL mode is well-behaved by construction: it opens exactly
 * ONE frame stream, only after the 200, always naming the CONNECT stream's id,
 * and it never emits an unknown H3 frame type. Every #919 and #920 vector is
 * precisely a peer that does one of those things differently, so this client
 * drives QUIC + the H3 face itself and then writes arbitrary bytes on arbitrary
 * streams. Handles are closed only by the destructor.
 */
struct raw_wt_client_t {
    /** @brief One client-opened stream and what the server did with it. */
    struct wt_stream_t {
        HQUIC h = nullptr;    /**< @brief The msquic handle. */
        std::uint64_t id = 0; /**< @brief Its QUIC stream id (the session id). */
        std::atomic<bool> aborted{
            false};                     /**< @brief Server refusal: RESET_STREAM / STOP_SENDING. */
        std::atomic<std::size_t> rx{0}; /**< @brief Bytes the server wrote back. */
    };

    /** @brief One in-flight StreamSend and the bytes msquic borrows for it. */
    struct send_buf_t {
        QUIC_BUFFER buf{};               /**< @brief The descriptor handed to msquic. */
        std::vector<std::uint8_t> bytes; /**< @brief The borrowed payload. */
    };

    const QUIC_API_TABLE* api = nullptr; /**< @brief The msquic API table. */
    HQUIC reg = nullptr;                 /**< @brief Registration handle. */
    HQUIC cfg = nullptr;                 /**< @brief Configuration handle (ALPN h3). */
    HQUIC conn = nullptr;                /**< @brief The connection handle. */
    bool ok = false;                     /**< @brief QUIC up and the H3 face sent. */

    std::mutex m;                  /**< @brief Guards the rendezvous + the handle lists. */
    std::condition_variable cv;    /**< @brief The handshake rendezvous. */
    bool handshake_done = false;   /**< @brief Handshake stage resolved. */
    bool handshake_ok = false;     /**< @brief Handshake stage outcome. */
    std::atomic<bool> shut{false}; /**< @brief The server tore the CONNECTION down. */
    std::vector<std::unique_ptr<wt_stream_t>> streams; /**< @brief Our streams. */
    std::vector<HQUIC> peer_streams; /**< @brief The server's H3 streams (accepted, drained). */

    /** @brief The server's own control/QPACK streams: accepted so msquic has a
     *         handler, contents irrelevant to every vector here. */
    static QUIC_STATUS QUIC_API peer_stream_cb(HQUIC, void*, QUIC_STREAM_EVENT*) {
        return QUIC_STATUS_SUCCESS;
    }

    /** @brief Connection events: the handshake rendezvous, peer-stream adoption,
     *         and the connection-down flag every "was it the STREAM or the
     *         CONNECTION?" assertion keys off. */
    static QUIC_STATUS QUIC_API conn_cb(HQUIC, void* ctx, QUIC_CONNECTION_EVENT* ev) {
        auto* self = static_cast<raw_wt_client_t*>(ctx);
        const tsan_cb_guard_t guard(self);
        switch (ev->Type) {
            case QUIC_CONNECTION_EVENT_CONNECTED: {
                const std::lock_guard lock(self->m);
                self->handshake_done = self->handshake_ok = true;
                self->cv.notify_all();
                return QUIC_STATUS_SUCCESS;
            }
            case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
                self->api->SetCallbackHandler(ev->PEER_STREAM_STARTED.Stream,
                                              reinterpret_cast<void*>(&peer_stream_cb), nullptr);
                const std::lock_guard lock(self->m);
                self->peer_streams.push_back(ev->PEER_STREAM_STARTED.Stream);
                return QUIC_STATUS_SUCCESS;
            }
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
            case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
            case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
                self->shut.store(true, std::memory_order_relaxed);
                const std::lock_guard lock(self->m);
                self->handshake_done = true;
                self->cv.notify_all();
                return QUIC_STATUS_SUCCESS;
            }
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    /** @brief Stream events: byte counting, send-buffer release, and the two
     *         refusal signals a `StreamShutdown(ABORT)` produces on this side. */
    static QUIC_STATUS QUIC_API stream_cb(HQUIC, void* ctx, QUIC_STREAM_EVENT* ev) {
        auto* s = static_cast<wt_stream_t*>(ctx);
        const tsan_cb_guard_t guard(s);
        switch (ev->Type) {
            case QUIC_STREAM_EVENT_RECEIVE:
                for (std::uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i)
                    s->rx.fetch_add(ev->RECEIVE.Buffers[i].Length, std::memory_order_relaxed);
                return QUIC_STATUS_SUCCESS;
            case QUIC_STREAM_EVENT_SEND_COMPLETE:
                tsan_acquire(ev->SEND_COMPLETE.ClientContext);  // pairs with write()'s release
                delete static_cast<send_buf_t*>(ev->SEND_COMPLETE.ClientContext);
                return QUIC_STATUS_SUCCESS;
            case QUIC_STREAM_EVENT_PEER_SEND_ABORTED:
            case QUIC_STREAM_EVENT_PEER_RECEIVE_ABORTED:
                s->aborted.store(true, std::memory_order_relaxed);
                return QUIC_STATUS_SUCCESS;
            default:
                return QUIC_STATUS_SUCCESS;
        }
    }

    explicit raw_wt_client_t(std::uint16_t port) {
        if (QUIC_FAILED(MsQuicOpen2(&api))) {
            api = nullptr;
            return;
        }
        const QUIC_REGISTRATION_CONFIG rc{"wt_test_raw", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
        if (QUIC_FAILED(api->RegistrationOpen(&rc, &reg))) return;
        const QUIC_BUFFER alpn{sizeof("h3") - 1,
                               reinterpret_cast<uint8_t*>(const_cast<char*>("h3"))};
        QUIC_SETTINGS settings{};
        settings.IdleTimeoutMs = 0;
        settings.IsSet.IdleTimeoutMs = TRUE;
        // The server opens three unidirectional streams at us (control + the two
        // QPACK streams); leave headroom so none of them stalls.
        settings.PeerUnidiStreamCount = 8;
        settings.IsSet.PeerUnidiStreamCount = TRUE;
        settings.PeerBidiStreamCount = 4;
        settings.IsSet.PeerBidiStreamCount = TRUE;
        if (QUIC_FAILED(
                api->ConfigurationOpen(reg, &alpn, 1, &settings, sizeof(settings), nullptr, &cfg)))
            return;
        QUIC_CREDENTIAL_CONFIG cred{};
        cred.Type = QUIC_CREDENTIAL_TYPE_NONE;
        cred.Flags = static_cast<QUIC_CREDENTIAL_FLAGS>(
            QUIC_CREDENTIAL_FLAG_CLIENT | QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION);
        if (QUIC_FAILED(api->ConfigurationLoadCredential(cfg, &cred))) return;
        if (QUIC_FAILED(api->ConnectionOpen(reg, &conn_cb, this, &conn))) {
            conn = nullptr;
            return;
        }
        tsan_release(this);  // publish the constructed client to its callbacks
        if (QUIC_FAILED(
                api->ConnectionStart(conn, cfg, QUIC_ADDRESS_FAMILY_UNSPEC, "127.0.0.1", port)))
            return;
        {
            std::unique_lock lock(m);
            cv.wait_for(lock, 5s, [this] { return handshake_done; });
            if (!handshake_ok) return;
        }
        // Our H3 face: the control stream (SETTINGS) + the two QPACK streams,
        // exactly what the library's DIAL side sends.
        wt_stream_t* const ctl = open_stream(true);
        if (ctl->h == nullptr) return;
        write(ctl, tr::net::wt_h3::control_stream_bytes());
        for (const std::uint64_t type :
             {tr::net::wt_h3::kStreamTypeQpackEncoder, tr::net::wt_h3::kStreamTypeQpackDecoder}) {
            wt_stream_t* const q = open_stream(true);
            if (q->h == nullptr) return;
            std::vector<std::uint8_t> b;
            tr::net::wt_h3::append_varint(b, type);
            write(q, b);
        }
        ok = true;
    }

    ~raw_wt_client_t() {
        if (api == nullptr) return;
        // Harvest the server-opened handles UNDER the lock and close them
        // outside it: conn_cb pushes them from an msquic worker, and StreamClose
        // blocks until that stream's callbacks drain — closing while holding the
        // lock a callback also takes would deadlock. (`streams` needs no lock:
        // only this thread ever touches it.)
        std::vector<HQUIC> peers;
        {
            const std::lock_guard lock(m);
            peers.swap(peer_streams);
        }
        for (const auto& s : streams)
            if (s->h != nullptr) api->StreamClose(s->h);
        for (HQUIC h : peers) api->StreamClose(h);
        if (conn != nullptr) {
            api->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
            api->ConnectionClose(conn);
        }
        if (cfg != nullptr) api->ConfigurationClose(cfg);
        if (reg != nullptr) api->RegistrationClose(reg);
        MsQuicClose(api);
        tsan_acquire(this);  // all callbacks drained — take their published writes
    }

    /**
     * @brief Open one stream and read back its QUIC stream id.
     *
     * Always returns a live record, `h == nullptr` when the open failed — a
     * connection the server has already torn down refuses StreamOpen, and every
     * vector here deliberately provokes teardowns. Returning null instead would
     * turn a REDDENING assertion into a segfault in the harness, which is
     * exactly the failure mode a non-vacuity revert must not hit.
     */
    wt_stream_t* open_stream(bool uni) {
        auto s = std::make_unique<wt_stream_t>();
        wt_stream_t* const raw = s.get();
        streams.push_back(std::move(s));
        const QUIC_STREAM_OPEN_FLAGS flags =
            uni ? QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL : QUIC_STREAM_OPEN_FLAG_NONE;
        HQUIC h = nullptr;
        if (QUIC_FAILED(api->StreamOpen(conn, flags, &stream_cb, raw, &h))) return raw;
        raw->h = h;
        tsan_release(raw);  // publish the ctx to its callbacks
        if (QUIC_FAILED(api->StreamStart(h, QUIC_STREAM_START_FLAG_NONE))) {
            api->StreamClose(h);
            raw->h = nullptr;
            return raw;
        }
        std::uint64_t id = 0;
        std::uint32_t sz = sizeof(id);
        if (QUIC_SUCCEEDED(api->GetParam(h, QUIC_PARAM_STREAM_ID, &sz, &id))) raw->id = id;
        return raw;
    }

    /** @brief Open a bidirectional stream (the CONNECT / frame-channel shape). */
    wt_stream_t* open_bidi() { return open_stream(false); }

    /** @brief Write raw bytes on @p s in ONE StreamSend — no framing added. */
    void write(wt_stream_t* s, std::span<const std::uint8_t> bytes) {
        if (s == nullptr || s->h == nullptr) return;  // the connection is already gone
        auto* sb = new send_buf_t{};
        sb->bytes.assign(bytes.begin(), bytes.end());
        sb->buf.Length = static_cast<std::uint32_t>(sb->bytes.size());
        sb->buf.Buffer = sb->bytes.data();
        tsan_release(sb);  // pairs with SEND_COMPLETE's acquire
        if (QUIC_FAILED(api->StreamSend(s->h, &sb->buf, 1, QUIC_SEND_FLAG_NONE, sb))) delete sb;
    }

    /** @brief Block until the server refused @p s (stream-scoped abort). */
    [[nodiscard]] static bool wait_aborted(wt_stream_t* s, std::chrono::milliseconds budget) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (!s->aborted.load(std::memory_order_relaxed)) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    /** @brief Block until the server tore the whole CONNECTION down. */
    [[nodiscard]] bool wait_shutdown(std::chrono::milliseconds budget) const {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (!shut.load(std::memory_order_relaxed)) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }
};

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
        sink_t sink;
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
        check(sink.count() == 1 && sink.at(0).size() == good.size(),
              "guard 1: exactly the legitimate frame arrived — the refused stream fed nothing");
    }

    // Guard 2: a 0x41 stream naming a session id that is not the CONNECT stream's.
    {
        sink_t sink;
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
        check(sink.count() == 1 && sink.at(0).size() == good.size(),
              "guard 2: only the correctly-addressed stream's frame arrived");
    }

    // Guard 3: a SECOND 0x41 stream after adoption — the framer-corruption arm.
    {
        sink_t sink;
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
        check(sink.count() == 3 && sink.at(0).size() == r1.size() &&
                  sink.at(1).size() == r2.size() && sink.at(2).size() == r3.size(),
              "guard 3: the three frames are the first stream's, in order");
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
        sink_t sink;
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

        // A post-CONNECT GREASE frame on the request stream changes nothing.
        cli.write(connect, grease_frame(3, 7));
        auto* frames = cli.open_bidi();
        cli.write(frames, wt_preamble(connect->id));
        const auto good = payload(12, 0x61);
        cli.write(frames, record(good));
        check(sink.wait_for_count(1, 3000ms), "frames flow across the skipped GREASE frames");
        check(sink.count() == 1 && sink.at(0).size() == good.size(), "the frame is intact");
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
    test_view_delivery_and_backpressure();
    test_fwd_read_round_trip();
    test_config_constructed_webtransport();
    test_spec_dial_trust_keys();
    test_frame_stream_adoption_gate();
    test_unknown_h3_frames_skipped();
    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
