/**
 * @file
 * @brief The TWO-MOUNT route (#419): a `dst` that crosses two `net/<module>/<name>` mounts.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Until now **every** FWD test in the tree had exactly ONE forwarder, and the conformance
 * vectors are static encode/decode fixtures that route nothing (see `tests/conformance/HARNESS.md`
 * — a vector gates the codec and only the codec). So no test could ever have caught a
 * disagreement about what a *multi*-hop `dst` looks like, which is precisely how the #419
 * three-way contradiction went unnoticed. This is the missing shape:
 *
 * ```
 *   client ──▶ A ──(A: net/uplink/b)──▶ B ──(B: net/uplink/c)──▶ C  [/sensor/temp]
 * ```
 *
 * A single `dst=/net/uplink/b/net/uplink/c/sensor/temp` is consumed by TWO different routers
 * before its residual resolves at a third node's terminus. The conformance vector
 * `fwd/fwd-routed-two-mount` carries exactly these origin bytes; this test is the behavioural
 * binding HARNESS.md requires for them — it fails if either strip-K descent changes.
 *
 * Production wiring throughout (the RFC-0014 lesson — two silent misroutes shipped because no
 * test used it): every link is bound by an in-band `write /net/<module>/conn <- SPEC{...}`
 * through @ref tr::net::transport_vertex_t, so each mount key is composed by
 * `transport_vertex.cpp` itself and never hand-spelled into `add_child`.
 *
 * Asserted:
 *   - hop 1 at A consumes `strip_k = 3` (8 dst segments in, 5 out) and forwards
 *     `dst=/net/uplink/c/sensor/temp`, `src=/net/downlink/cli/reply-ep` — byte-exact;
 *   - hop 2 at B consumes `strip_k = 3` (5 in, 2 out) and forwards `dst=/sensor/temp`,
 *     `src=/net/downlink/a/net/downlink/cli/reply-ep` — byte-exact, so `src` grew by the FULL
 *     mount run at both hops, never a bare NAME (the ADR-0061 erratum);
 *   - the terminus at C serves the stored VALUE and the REPLY source-routes home over both
 *     mounts, arriving at the client with `dst` fully consumed to `/reply-ep`.
 */

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fwd_frame_builder.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/loopback.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::tlv_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::mailbox_t;

/** @brief A heap-owned @ref tr::view::view_t over @p bytes (the graph stores owning views). */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief Canonical `PATH{ NAME… }` bytes for @p segs, via the production emit helpers. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief Canonical `VALUE` bytes holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, p);
    return out;
}

using tr::testing::b_fwd;

/**
 * @brief A connection-creation SPEC (ADR-0027 / reference/05) with no transport `kind`.
 *
 * `kind` is omitted on purpose: every link here is staged by
 * @ref tr::net::transport_vertex_t::provide_link, which takes precedence over factory
 * construction — so the SPEC carries only the NAME. The module the connection mounts under is
 * the creator endpoint it is written to, not a pair in these bytes.
 */
view_t conn_spec(std::string_view name) { return tr::net::conn_spec(name, 0); }

/** @brief The creator endpoint of @p module — the one wire door that creates a connection. */
path_t conn_endpoint(std::string_view module) {
    std::string text = "/net/";
    text += module;
    text += "/conn";
    return path_t(text);
}

/**
 * @brief The `dst`/`src` of the first FWD a node saw on ONE named inbound link.
 *
 * Filled from an @ref tr::net::fwd_router_t::on_raw observer, which fires on a transport
 * receive thread before dispatch — so the snapshot is the frame the PREVIOUS hop actually put
 * on the wire, not a re-encoding of it.
 */
struct hop_probe_t {
    std::mutex m;
    std::condition_variable cv;
    std::string link;           /**< @brief The inbound link NAME to latch on. */
    bool seen = false;          /**< @brief Whether a FWD has been captured. */
    std::vector<std::byte> dst; /**< @brief Re-encoded `dst` PATH bytes. */
    std::vector<std::byte> src; /**< @brief Re-encoded `src` PATH bytes. */
    std::size_t dst_segs = 0;   /**< @brief `dst` segment count (the strip-K witness). */

    /** @brief Latch the first FWD arriving on @ref link. */
    void observe(std::string_view on, std::span<const std::byte> frame) {
        if (on != link) return;
        const auto dec = tr::wire::decode(frame);
        if (!dec || dec->type != type_t::FWD || dec->children.size() < 3) return;
        {
            const std::lock_guard lock(m);
            if (seen) return;
            dst = tr::wire::encode(dec->children[1]);
            src = tr::wire::encode(dec->children[2]);
            // Packed records (RFC-0018): count `[u8 len][bytes]` records in the PATH body.
            dst_segs = 0;
            for (std::size_t at = 0; at < dec->children[1].payload.size();) {
                const auto len = static_cast<std::size_t>(
                    static_cast<std::uint8_t>(dec->children[1].payload[at]));
                if (len == 0 || at + 1 + len > dec->children[1].payload.size()) break;
                ++dst_segs;
                at += 1 + len;
            }
            seen = true;
        }
        cv.notify_all();
    }

    /** @brief Block until a FWD was latched, or @p budget elapses. */
    bool wait(std::chrono::milliseconds budget) {
        std::unique_lock lock(m);
        return cv.wait_for(lock, budget, [this] { return seen; });
    }
};

constexpr auto kBudget = 5000ms;

/** @brief Read a 1-byte VALUE child as a `u8` (the `op`/`kind` discriminators). */
std::uint8_t value_u8(const tlv_t& v) { return tr::detail::load_le<std::uint8_t>(v.payload); }

}  // namespace

int main() {
    std::printf("Two-mount FWD route (#419): client -> A -> B -> C, one dst crossing TWO mounts\n");

    // ----- the four nodes. Each owns a graph, a router and a /net vertex; every link
    //       is bound through the production `/net/<module>/conn` SPEC door.
    graph_t g_cli;
    fwd_router_t r_cli(g_cli);
    transport_vertex_t net_cli(g_cli, r_cli);

    graph_t g_a;
    fwd_router_t r_a(g_a);
    transport_vertex_t net_a(g_a, r_a);

    graph_t g_b;
    fwd_router_t r_b(g_b);
    transport_vertex_t net_b(g_b, r_b);

    graph_t g_c;
    fwd_router_t r_c(g_c);
    transport_vertex_t net_c(g_c, r_c);

    // The terminus value, three nodes away from the originator.
    const auto sensor = path_t::parse("/sensor");
    const auto sensor_temp = path_t::parse("/sensor/temp");
    (void)g_c.register_vertex(*sensor, role_t::STORED_VALUE);
    const tr::graph::vertex_handle_t vC = g_c.register_vertex(*sensor_temp, role_t::STORED_VALUE);
    const std::uint32_t kStored = 0xC0FFEE01u;
    (void)g_c.write(vC, owned(b_value_u32(kStored)));

    // ----- the three loopback channels, staged and then created as connections.
    tr::net::loopback_channel_t ch_cli;  // client <-> A
    tr::net::loopback_channel_t ch_ab;   // A <-> B
    tr::net::loopback_channel_t ch_bc;   // B <-> C

    net_cli.provide_link("uplink", "a", ch_cli.a());
    net_a.provide_link("downlink", "cli", ch_cli.b());
    net_a.provide_link("uplink", "b", ch_ab.a());
    net_b.provide_link("downlink", "a", ch_ab.b());
    net_b.provide_link("uplink", "c", ch_bc.a());
    net_c.provide_link("downlink", "b", ch_bc.b());

    // One module declaration per staged link, each with a `kind` of its own: a declaration is
    // keyed on (kind, role), so two modules on one node that shared a kind would collapse into
    // one and the second would silently rename the first. Nothing constructs from these kinds —
    // a staged link outranks a factory — so the module's own name serves as its kind.
    (void)net_cli.register_module("uplink", "uplink", conn_role_t::DIAL);
    (void)net_a.register_module("downlink", "downlink", conn_role_t::DIAL);
    (void)net_a.register_module("uplink", "uplink", conn_role_t::DIAL);
    (void)net_b.register_module("downlink", "downlink", conn_role_t::DIAL);
    (void)net_b.register_module("uplink", "uplink", conn_role_t::DIAL);
    (void)net_c.register_module("downlink", "downlink", conn_role_t::DIAL);

    const auto mk = [](graph_t& g, std::string_view module, std::string_view name) {
        return g.write(conn_endpoint(module), conn_spec(name));
    };
    check(mk(g_cli, "uplink", "a").has_value() && mk(g_a, "downlink", "cli").has_value() &&
              mk(g_a, "uplink", "b").has_value() && mk(g_b, "downlink", "a").has_value() &&
              mk(g_b, "uplink", "c").has_value() && mk(g_c, "downlink", "b").has_value(),
          "all six connections created through the /net/<module>/conn creator endpoint "
          "(production wiring)");

    // ----- the per-hop probes and the client's reply sink.
    hop_probe_t at_b;
    at_b.link = "net/downlink/a";  // B's mount for the link from A
    r_b.on_raw([](void* ctx, std::string_view on,
                  std::span<const std::byte> f) { static_cast<hop_probe_t*>(ctx)->observe(on, f); },
               &at_b);

    hop_probe_t at_c;
    at_c.link = "net/downlink/b";  // C's mount for the link from B
    r_c.on_raw([](void* ctx, std::string_view on,
                  std::span<const std::byte> f) { static_cast<hop_probe_t*>(ctx)->observe(on, f); },
               &at_c);

    mailbox_t inbox;
    r_cli.on_reply(
        [](void* ctx, const tr::view::rope_t& reply) {
            const view_t mat = reply.materialize();
            const auto b = mat.bytes();
            static_cast<mailbox_t*>(ctx)->push(std::vector<std::byte>(b.begin(), b.end()));
        },
        &inbox);

    // ===== the origin frame — byte-identical to fwd/fwd-routed-two-mount's input.bin =====
    const std::vector<std::byte> origin = b_fwd(
        fwd_op_t::READ, b_path({"net", "uplink", "b", "net", "uplink", "c", "sensor", "temp"}),
        b_path({"reply-ep"}));
    // The vector's input.bin, pinned here so a divergence between b_fwd's construction and the
    // published bytes fails this test rather than only the Rust encode pin.
    static constexpr std::string_view kVectorHex =
        "0f403c00010001000006002600036e65740675706c696e6b0162036e65740675706c696e6b0163067365"
        "6e736f720474656d7006000900087265706c792d6570";
    std::vector<std::byte> expected_bytes;
    for (size_t i = 0; i + 1 < kVectorHex.size(); i += 2) {
        expected_bytes.push_back(
            static_cast<std::byte>(std::stoi(std::string(kVectorHex.substr(i, 2)), nullptr, 16)));
    }
    check(origin == expected_bytes,
          "the origin frame is byte-identical to fwd/fwd-routed-two-mount's input.bin (64 B)");
    ch_cli.a().send(origin);

    // ===== hop 1, at A: strip_k = 3 =====================================================
    std::printf("Hop 1 (A strips its mount net/uplink/b):\n");
    check(at_b.wait(kBudget), "B received the frame A forwarded");
    check(at_b.dst_segs == 5, "A consumed exactly 3 of the 8 dst segments (strip_k = 3)");
    check(at_b.dst == b_path({"net", "uplink", "c", "sensor", "temp"}),
          "B saw dst=/net/uplink/c/sensor/temp — byte-exact");
    check(at_b.src == b_path({"net", "downlink", "cli", "reply-ep"}),
          "B saw src grown by A's FULL mount run net/downlink/cli — byte-exact");

    // ===== hop 2, at B: strip_k = 3 =====================================================
    std::printf("Hop 2 (B strips its own mount net/uplink/c):\n");
    check(at_c.wait(kBudget), "C received the frame B forwarded");
    check(at_c.dst_segs == 2, "B consumed exactly 3 of the 5 remaining dst segments (strip_k = 3)");
    check(at_c.dst == b_path({"sensor", "temp"}),
          "C saw dst=/sensor/temp — the residual, byte-exact");
    check(at_c.src == b_path({"net", "downlink", "a", "net", "downlink", "cli", "reply-ep"}),
          "C saw src grown by BOTH hops' mount runs, in reverse-route order — byte-exact");

    // ===== the terminus, at C, and the reply home over both mounts ======================
    std::printf("Terminus at C, reply source-routed home over both mounts:\n");
    const auto reply = inbox.wait(kBudget);
    check(reply.has_value(), "the client received a REPLY three nodes away");
    if (reply) {
        const auto dec = tr::wire::decode(*reply);
        check(dec.has_value() && dec->type == type_t::FWD, "the REPLY decodes as a FWD");
        const tlv_t& r = *dec;
        check(r.children.size() == 5, "REPLY has 5 children (op, dst, src, kind, value)");
        check(value_u8(r.children[0]) == static_cast<std::uint8_t>(fwd_op_t::REPLY), "op == REPLY");
        check(value_u8(r.children[3]) == static_cast<std::uint8_t>(reply_kind_t::RESULT),
              "kind == RESULT (the terminus resolved /sensor/temp, no error)");
        check(tr::wire::encode(r.children[1]) == b_path({"reply-ep"}),
              "REPLY dst fully consumed to /reply-ep — both mounts stripped on the way back");
        check(r.children[4].type == type_t::VALUE && r.children[4].payload.size() == 4 &&
                  tr::detail::load_le<std::uint32_t>(r.children[4].payload) == kStored,
              "REPLY payload == C's exact stored VALUE u32=0xC0FFEE01 (byte-exact end to end)");
    }

    ch_bc.shutdown();
    ch_ab.shutdown();
    ch_cli.shutdown();

    return tr::testing::summary("fwd_two_mount");
}
