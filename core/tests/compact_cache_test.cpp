/**
 * @file
 * @brief ADR-0062 increment 2 — a warm binding is fast, and it invalidates correctly.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Delivery compaction (RFC-0004 §E.1) removed the ROUTE from the wire but not the
 * RESOLUTION from the hop: every COMPACT frame re-decoded the bound route and re-ran
 * `graph_.find`, and `lookup_ingress` copied a `std::string` + a `std::vector` out of the
 * label table before anything even checked. A binding now memoizes what it resolved.
 *
 * Memoization is only safe if it is invalidated, and the two cached forms invalidate by
 * DIFFERENT mechanisms — so both are pinned here, each by the scenario that would
 * otherwise deliver into a freed or re-virginized target:
 *
 *   - **terminus** — a generation stamp (#511). `retire()` re-virginizes a vertex
 *     (RFC-0009 §B.6), so a handle cached before retirement must NOT be written through
 *     afterwards, even though the pointer may still address a live object.
 *   - **forwarding** — the registry tombstone (ADR-0063). Teardown nulls the slot's
 *     `link` in place and slot addresses are permanently stable, so a cached slot reads
 *     `nullptr` after `remove_child`. The tombstone IS the invalidation.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/route_handle.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A link that records the frames it is asked to send. */
struct rec_link_t : tr::net::transport_t {
    std::vector<std::vector<std::byte>> sent;
    void send(std::span<const std::byte> f) override { sent.emplace_back(f.begin(), f.end()); }
};

std::vector<std::byte> path_tlv(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

std::vector<std::byte> value_tlv(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief The stored value's first payload byte, or nullopt when the vertex is empty. */
std::optional<std::uint8_t> stored_byte(graph_t& g, const char* p) {
    const auto v = g.find(path_t::parse(p)->key());
    if (!v) return std::nullopt;
    const auto r = g.read(*v);
    if (!r) return std::nullopt;
    const tr::view::view_t flat = r->flatten();
    const std::span<const std::byte> b = flat.bytes();
    if (b.size() < 3) return std::nullopt;
    return static_cast<std::uint8_t>(b[b.size() - 1]);
}

// --- tests -------------------------------------------------------------------

/** @brief A warm terminus binding still delivers — repeatedly, and byte-exact. */
void test_warm_terminus_delivers() {
    std::printf("warm terminus binding\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router{g};
    rec_link_t up;
    router.add_child("net/ws-client/up", up);

    const std::vector<std::byte> route = path_tlv({"sink"});
    router.on_frame("net/ws-client/up", tr::net::encode_advertise(5, route));

    // First frame resolves cold and memoizes; the rest take the warm path.
    for (std::uint8_t i = 1; i <= 4; ++i)
        router.on_frame("net/ws-client/up", tr::net::encode_compact(5, value_tlv(i)));

    check(stored_byte(g, "/sink") == 4, "every compacted delivery landed, warm path included");
    check(up.sent.empty(), "and no NACK travelled back — the binding stayed resolved");
}

/**
 * @brief A cached handle must NOT survive retirement (#511 generation guard).
 *
 * The confused-deputy scenario: `retire()` re-virginizes the vertex (RFC-0009 §B.6), so a
 * handle resolved beforehand names a target that no longer means what the label agreed it
 * meant. Writing through it would deliver one peer's stream into a re-created vertex.
 */
void test_retire_invalidates_cached_handle() {
    std::printf("retirement invalidates a warm terminus binding\n");
    graph_t g;
    const auto v = g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router{g};
    rec_link_t up;
    router.add_child("net/ws-client/up", up);

    router.on_frame("net/ws-client/up", tr::net::encode_advertise(7, path_tlv({"sink"})));
    router.on_frame("net/ws-client/up", tr::net::encode_compact(7, value_tlv(1)));
    check(stored_byte(g, "/sink") == 1, "warm-up delivery landed");
    const std::uint32_t gen_before = g.retire_generation(v);

    (void)g.retire(v);
    const std::uint32_t gen_after = g.retire_generation(v);
    check(gen_after != gen_before, "retire() bumped the generation the cache compares against");

    // The cached handle is now stale. The hop must NOT write through it blindly.
    router.on_frame("net/ws-client/up", tr::net::encode_compact(7, value_tlv(9)));
    check(stored_byte(g, "/sink") != 9,
          "a stale cached handle is not written through after retirement");
}

/** @brief A cached forwarding slot goes null when the link departs (ADR-0063 tombstone). */
void test_link_teardown_invalidates_cached_slot() {
    std::printf("link teardown invalidates a warm forwarding binding\n");
    graph_t g;
    fwd_router_t router{g};
    rec_link_t up;
    rec_link_t down;
    router.add_child("net/ws-client/up", up);
    router.add_child("net/ws-server/down", down);

    // A route BELOW the down mount ⇒ a forwarding binding, not a terminus.
    router.on_frame("net/ws-client/up",
                    tr::net::encode_advertise(11, path_tlv({"net", "ws-server", "down", "far"})));
    check(down.sent.size() == 1, "the advertise relayed downstream");

    router.on_frame("net/ws-client/up", tr::net::encode_compact(11, value_tlv(1)));
    router.on_frame("net/ws-client/up", tr::net::encode_compact(11, value_tlv(2)));
    check(down.sent.size() == 3, "both compacted frames forwarded (second one warm)");

    // Tear the downstream link down; the cached slot must stop resolving.
    check(router.remove_child("net/ws-server/down"), "the downstream link is removed");
    const std::size_t before = down.sent.size();
    router.on_frame("net/ws-client/up", tr::net::encode_compact(11, value_tlv(3)));
    check(down.sent.size() == before,
          "nothing is sent through the departed link — the cached slot reads its tombstone");
}

/**
 * @brief A COMPACT whose root CRC trailer says the payload is corrupt must be DROPPED.
 *
 * The regression guard for verify-before-apply (CONTEXT.md §Frame integrity, ADR-0041 §1) on
 * the span control arm. That arm used to `wire::decode` the whole frame, which verifies every
 * node's CRC as a side effect; it now reads by offset through `peek_control`, whose default is
 * `crc_check_t::DEFER` because every forward-hop caller wants that. The control arm passes
 * VERIFY explicitly — and nothing else in the suite would notice if that argument were dropped
 * in a later refactor, because a well-formed frame routes identically either way.
 *
 * So this test corrupts a payload byte UNDER a valid CRC and requires the delivery not to
 * land. Without the explicit VERIFY it is silently applied.
 */
void test_corrupt_crc_compact_is_dropped() {
    std::printf("corrupt-CRC COMPACT is dropped (verify-before-apply)\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router{g};
    rec_link_t up;
    router.add_child("net/ws-client/up", up);
    router.on_frame("net/ws-client/up", tr::net::encode_advertise(3, path_tlv({"sink"})));

    // A good frame first — both to warm the binding and to prove the vehicle works.
    router.on_frame("net/ws-client/up", tr::net::encode_compact(3, value_tlv(0x11)));
    check(stored_byte(g, "/sink") == 0x11, "an intact COMPACT lands");

    // Re-emit the same COMPACT carrying a whole-frame CRC-32C trailer.
    const std::vector<std::byte> plain = tr::net::encode_compact(3, value_tlv(0x22));
    tr::wire::tlv_t crc_tlv = *tr::wire::decode(plain);
    crc_tlv.opt.cr = true;
    const std::vector<std::byte> crc_frame = tr::wire::encode(crc_tlv);
    check(crc_frame.size() == plain.size() + 4, "the CRC frame carries a 4-byte trailer");

    // Intact-with-CRC must still deliver — otherwise the next assertion proves nothing.
    router.on_frame("net/ws-client/up", crc_frame);
    check(stored_byte(g, "/sink") == 0x22, "a CRC-carrying COMPACT still delivers when intact");

    // Now corrupt a BODY byte under that trailer: the grammar stays valid, the CRC breaks.
    std::vector<std::byte> corrupt = crc_frame;
    corrupt[corrupt.size() - 5] ^= std::byte{0xFF};
    router.on_frame("net/ws-client/up", corrupt);
    check(stored_byte(g, "/sink") == 0x22,
          "a corrupt-CRC COMPACT is DROPPED — the LKV still holds the last good value");
}

}  // namespace

int main() {
    test_warm_terminus_delivers();
    test_retire_invalidates_cached_handle();
    test_link_teardown_invalidates_cached_slot();
    test_corrupt_crc_compact_is_dropped();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
