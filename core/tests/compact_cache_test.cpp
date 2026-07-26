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

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
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

/**
 * @brief `try_encode_compact` emits BYTE-IDENTICAL frames to `encode_compact`.
 *
 * The forwarding leg swapped to the nothrow exact-reserve encoder to drop four
 * growth-doubling allocations per steady-state frame. That is only a safe swap if the two
 * encoders agree on the wire, so this pins it across payload sizes (including empty and
 * >64 KB, where the length field widens) and across label values (including 0 and 0xFFFF).
 *
 * Byte equality is the assertion that matters here, not the allocation count: an encoder that
 * merely produced a *parseable* frame would pass a routing test while breaking a peer.
 */
void test_encoders_agree_byte_for_byte() {
    std::printf("try_encode_compact matches encode_compact byte-for-byte\n");
    int mismatches = 0;
    int failures = 0;
    // 65530 and 70000 are the point of this list: the label child is 6 bytes, so the frame
    // body crosses 0xFFFF (and the length field widens to 4 bytes) at a payload of 65530 —
    // NOT at 65536. Every earlier revision stopped at 65000, a body of 65006, so the
    // "including >64 KB, where the length field widens" claim above was untrue and the
    // widening path was never executed by any test.
    for (const std::size_t n :
         {std::size_t{0}, std::size_t{1}, std::size_t{2}, std::size_t{63}, std::size_t{64},
          std::size_t{512}, std::size_t{4096}, std::size_t{65000}, std::size_t{65529},
          std::size_t{65530}, std::size_t{70000}}) {
        const std::vector<std::byte> payload(n, std::byte{0x5A});
        for (const std::uint16_t label :
             {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x1234}, std::uint16_t{0xFFFF}}) {
            const std::vector<std::byte> a = tr::net::encode_compact(label, payload);
            std::vector<std::byte> b;
            if (!tr::net::try_encode_compact(b, label, payload)) {
                ++failures;
                continue;
            }
            if (a != b) ++mismatches;
        }
    }
    check(failures == 0, "the nothrow encoder succeeds for every size and label");
    check(mismatches == 0, "and its bytes are identical to the throwing encoder's");
}

/**
 * @brief `label_tlv` agrees with the generic emitter — pinned INDEPENDENTLY.
 *
 * Every encoder now routes the label child through `label_tlv`, which is what keeps a
 * scatter-gathered frame head and a built frame from drifting. But a single locus makes the
 * encoders agree with each OTHER even if that locus has the layout wrong — a self-comparison
 * that cannot fail. So this pins its six bytes against `wire::emit_tlv`, which reaches the
 * same shape by a different route, and against the literal wire spelling.
 */
void test_label_tlv_matches_the_generic_emitter() {
    std::printf("label_tlv matches wire::emit_tlv byte-for-byte\n");
    int mismatches = 0;
    for (const std::uint16_t label : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x00FF},
                                      std::uint16_t{0x1234}, std::uint16_t{0xFFFF}}) {
        std::array<std::byte, 2> raw{};
        tr::detail::store_le<std::uint16_t>(raw, label);
        std::vector<std::byte> want;
        tr::wire::emit_tlv(want, tr::wire::type_t::VALUE, tr::wire::opt_t{}, raw);
        const std::array<std::byte, 6> got = tr::net::label_tlv(label);
        if (want.size() != got.size() || !std::equal(want.begin(), want.end(), got.begin()))
            ++mismatches;
    }
    check(mismatches == 0, "label_tlv equals the generic VALUE emitter for every label");

    // And the literal wire spelling, so a change to BOTH emitters still trips something:
    // {VALUE, opt=0, len=2 (u16 LE), label (u16 LE)}.
    const std::array<std::byte, 6> got = tr::net::label_tlv(0xBEEF);
    check(got[0] == static_cast<std::byte>(0x01) && got[1] == std::byte{0} &&
              got[2] == std::byte{2} && got[3] == std::byte{0} &&
              got[4] == static_cast<std::byte>(0xEF) && got[5] == static_cast<std::byte>(0xBE),
          "and its bytes are the literal opaque 2-byte VALUE the wire specifies");
}

/**
 * @brief The SCATTER-GATHERED egress emits exactly what the built encoder would have.
 *
 * The forwarding hop and the producer-side `send_compact` no longer build a frame: they write
 * a 12-byte head on the stack and hand the transport `{head, payload}`, referencing the
 * payload instead of copying it twice. That is only a safe swap if the concatenation of the
 * gathered spans is byte-identical to `encode_compact`, so this drives the REAL router door
 * and reassembles what the link was handed.
 *
 * Sizes span the LL-widening boundary for the same reason as the encoder test above: the
 * gathered head computes its own length field, so a widening disagreement would show up here
 * and nowhere else.
 */
void test_gathered_egress_matches_the_built_frame() {
    std::printf("scatter-gathered COMPACT egress matches encode_compact byte-for-byte\n");
    // A link that RECORDS the gathered spans instead of writing them, so the test can compare
    // the concatenation. It must also prove the gather form was the one taken.
    struct recording_link_t : tr::net::transport_t {
        std::vector<std::byte> last;
        std::size_t gathers = 0;
        std::size_t contiguous = 0;
        void send(std::span<const std::byte> b) override {
            ++contiguous;
            last.assign(b.begin(), b.end());
        }
        void send(std::span<const std::span<const std::byte>> iov) override {
            ++gathers;
            last.clear();
            for (const std::span<const std::byte> s : iov)
                last.insert(last.end(), s.begin(), s.end());
        }
    };

    graph_t g;
    tr::net::fwd_router_t router(g);
    recording_link_t link;
    router.add_child("net/ws-server/down", link);

    int mismatches = 0;
    for (const std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{64}, std::size_t{4096},
                                std::size_t{65529}, std::size_t{65530}, std::size_t{70000}}) {
        const std::vector<std::byte> payload(n, std::byte{0x5A});
        for (const std::uint16_t label :
             {std::uint16_t{0}, std::uint16_t{0x1234}, std::uint16_t{0xFFFF}}) {
            router.send_compact("net/ws-server/down", label, payload);
            if (link.last != tr::net::encode_compact(label, payload)) ++mismatches;
        }
    }
    check(mismatches == 0, "every gathered frame equals the built one, across the LL boundary");
    check(link.contiguous == 0 && link.gathers > 0,
          "and the egress took the GATHER form — a contiguous send would mean it still built");
}

/**
 * @brief The default `send(iov)` gather DROPS on an exhausted heap — it must never abort.
 *
 * `transport_t::send(iov)`'s base implementation concatenates into a temporary before handing
 * it to the span `send`. It used a throwing `reserve` + `insert`, so under `-fno-exceptions`
 * (every ESP build) an exhausted heap ABORTED the node instead of shedding the frame — the
 * exact crash class #477 closed everywhere else on the writer side.
 *
 * It is reachable on the FORWARD hot path: `route_fwd_forward` scatter-gathers into
 * `send(iov)`, and a transport that does not override it — `transport_can`, and any
 * embedder's — lands in the base gather.
 *
 * Exercised through the `probe_fail_hook` OOM-injection seam, since the host heap cannot be
 * genuinely exhausted on demand.
 */
void test_iov_gather_drops_on_oom() {
    std::printf("default send(iov) gather soft-fails (#477)\n");
    struct counting_link_t : tr::net::transport_t {
        // The base send(iov) is deliberately NOT overridden — it is the case under test — so
        // it must be un-hidden by the span overload declared here.
        using tr::net::transport_t::send;
        std::size_t sends = 0;
        void send(std::span<const std::byte>) override { ++sends; }
    };
    counting_link_t link;
    const std::vector<std::byte> a(8, std::byte{0xA1});
    const std::vector<std::byte> b(8, std::byte{0xB2});
    const std::array<std::span<const std::byte>, 2> iov{std::span<const std::byte>(a),
                                                        std::span<const std::byte>(b)};

    link.send(std::span<const std::span<const std::byte>>(iov));
    check(link.sends == 1, "with a healthy heap the gathered frame is sent");

    // Now fail every probe: the gather must return without sending and without aborting.
    tr::detail::probe_fail_hook = [](std::size_t) noexcept { return false; };
    link.send(std::span<const std::span<const std::byte>>(iov));
    tr::detail::probe_fail_hook = nullptr;
    check(link.sends == 1, "an exhausted heap DROPS the frame — no send, and no abort");

    link.send(std::span<const std::span<const std::byte>>(iov));
    check(link.sends == 2, "and the link still works once the heap recovers");
}

}  // namespace

int main() {
    test_warm_terminus_delivers();
    test_retire_invalidates_cached_handle();
    test_link_teardown_invalidates_cached_slot();
    test_corrupt_crc_compact_is_dropped();
    test_encoders_agree_byte_for_byte();
    test_label_tlv_matches_the_generic_emitter();
    test_gathered_egress_matches_the_built_frame();
    test_iov_gather_drops_on_oom();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
