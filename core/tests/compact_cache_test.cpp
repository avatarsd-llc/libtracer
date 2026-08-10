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
#include "test_support.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::fwd_router_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

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
    const tr::view::view_t flat = (*r)->flatten();
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
    (void)router.add_child("net/ws-client/up", up);

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
    (void)router.add_child("net/ws-client/up", up);

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
    (void)router.add_child("net/ws-client/up", up);
    (void)router.add_child("net/ws-server/down", down);

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
    (void)router.add_child("net/ws-client/up", up);
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

/** @brief A link that concatenates whatever it is handed, and remembers WHICH send it was. */
struct gather_rec_link_t : tr::net::transport_t {
    std::vector<std::byte> last; /**< @brief The concatenation of the last send. */
    std::size_t gathers = 0;     /**< @brief Calls that took the iov overload. */
    std::size_t contiguous = 0;  /**< @brief Calls that took the span overload. */
    void send(std::span<const std::byte> b) override {
        ++contiguous;
        last.assign(b.begin(), b.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        ++gathers;
        last.clear();
        for (const std::span<const std::byte> s : iov) last.insert(last.end(), s.begin(), s.end());
    }
};

/**
 * @brief The gathered ADVERTISE the router emits equals what `encode_advertise` would build.
 *
 * #885: the four ADVERTISE emission sites stopped building a frame and started writing a
 * 12-byte head on the stack, referencing the route. Three of the four are peer-provoked, which
 * is why the built form had to go — but the swap is only safe if the concatenation of the
 * gathered spans is byte-identical to what the builder produced, so this drives the REAL
 * producer door and reassembles what the link was handed.
 *
 * The sizes are the point: the label child is 6 bytes, so the ADVERTISE body crosses 0xFFFF —
 * and the length field widens from u16 to u32 — at a ROUTE of 65530 bytes, not at 65536. The
 * gathered head computes that length itself, so a widening disagreement between
 * `stack_writer::header` and `wire::emit_header` would show up here and nowhere else.
 *
 * What this case does NOT cover: the LABEL values. `advertise` mints its own label from the
 * egress table, so a test driving the door cannot choose one; the label child's bytes are
 * pinned independently, for 0 and 0xFFFF among others, by
 * @ref test_label_tlv_matches_the_generic_emitter, and both the gathered head and the builder
 * take those six bytes from that one locus.
 */
void test_gathered_advertise_matches_the_built_frame() {
    std::printf("scatter-gathered ADVERTISE egress matches encode_advertise byte-for-byte\n");
    graph_t g;
    fwd_router_t router(g);
    gather_rec_link_t link;
    (void)router.add_child("net/ws-server/down", link);

    int mismatches = 0;
    int no_label = 0;
    for (const std::size_t n : {std::size_t{1}, std::size_t{4}, std::size_t{64}, std::size_t{4096},
                                std::size_t{65529}, std::size_t{65530}, std::size_t{70000}}) {
        // Distinct bytes per size, so `ensure_egress`'s route compare cannot alias two cases
        // onto one label and quietly halve the coverage.
        std::vector<std::byte> route(n, static_cast<std::byte>(n & 0xFF));
        route[0] = std::byte{0x06};  // PATH type byte, so the bytes read as a route in a dump
        const std::uint16_t label = router.advertise("net/ws-server/down", route);
        if (label == 0) {
            ++no_label;
            continue;
        }
        if (link.last != tr::net::encode_advertise(label, route)) ++mismatches;
    }
    check(no_label == 0, "every advertise minted a label");
    check(mismatches == 0, "every gathered ADVERTISE equals the built one, across the LL boundary");
    check(link.contiguous == 0 && link.gathers > 0,
          "and the egress took the GATHER form — a contiguous send would mean it still built");
}

/**
 * @brief The FORWARDING hop's re-advertise is gathered too, and carries the stripped route.
 *
 * The third of #885's ADVERTISE sites, and the one a peer provokes most directly: an inbound
 * ADVERTISE whose leading segment names a child link makes this node strip that segment, mint
 * its own downstream label, and re-advertise (the MPLS-style swap). That emission ran on the
 * inbound link's receive thread and built its frame with the throwing encoder.
 *
 * The out-label is read back INDEPENDENTLY rather than parsed out of the frame under test:
 * `ensure_egress` reuses one label per (link, route), so calling the producer door with the
 * same stripped route hands back exactly the label the hop minted. Comparing the recorded
 * frame against a build over THAT label pins the header, the length and the route bytes
 * without taking any of them from the frame itself.
 */
void test_forwarding_hop_advertise_is_gathered() {
    std::printf("the forwarding hop's re-advertise is gathered and carries the stripped route\n");
    graph_t g;
    fwd_router_t router(g);
    gather_rec_link_t up;
    gather_rec_link_t down;
    (void)router.add_child("up", up);
    (void)router.add_child("down", down);

    router.on_frame("up", tr::net::encode_advertise(7, path_tlv({"down", "sensor"})));
    check(down.gathers == 1 && down.contiguous == 0,
          "the hop re-advertised GATHERED — a contiguous send would mean it built the frame");
    const std::vector<std::byte> emitted = down.last;

    const std::vector<std::byte> stripped = path_tlv({"sensor"});
    const std::uint16_t out_label = router.advertise("down", stripped);
    check(out_label != 0, "the stripped route resolves to a bound downstream label");
    check(emitted == tr::net::encode_advertise(out_label, stripped),
          "and the frame it sent is byte-for-byte the ADVERTISE the builder would have made");
}

/**
 * @brief The stale-label HANDLE_NACK equals what `encode_handle_nack` would build.
 *
 * The other half of #885, and the one a hostile peer reaches for free: a COMPACT naming a label
 * this node never bound. The answer is a fixed ten bytes, so the router writes them into a
 * stack buffer and sends them contiguous — the frame has no variable-length child, so unlike
 * ADVERTISE and COMPACT there is nothing to gather and the transport must see the same SPAN
 * send it saw before. Both halves are asserted: the bytes, and that the send stayed contiguous.
 *
 * Unlike the ADVERTISE case above, the label here is chosen by the PEER, so this does pin the
 * boundary values 0 and 0xFFFF through the real receive path.
 */
void test_gathered_handle_nack_matches_the_built_frame() {
    std::printf("stack-built HANDLE_NACK matches encode_handle_nack byte-for-byte\n");
    graph_t g;
    fwd_router_t router(g);
    gather_rec_link_t link;
    (void)router.add_child("net/ws-client/up", link);

    int mismatches = 0;
    int silent = 0;
    for (const std::uint16_t label :
         {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{0x1234}, std::uint16_t{0xFFFF}}) {
        // Nothing is bound on this router, so EVERY label is stale and every COMPACT draws
        // the NACK — the arm under test.
        const std::vector<std::byte> frame = tr::net::encode_compact(label, value_tlv(0x5A));
        link.last.clear();
        router.on_frame("net/ws-client/up", frame);
        if (link.last.empty()) {
            ++silent;
            continue;
        }
        if (link.last != tr::net::encode_handle_nack(label)) ++mismatches;
    }
    check(silent == 0, "every stale COMPACT drew a HANDLE_NACK back over the inbound link");
    check(mismatches == 0, "and its ten bytes are identical to the built encoder's");
    check(link.gathers == 0 && link.contiguous > 0,
          "the NACK went out CONTIGUOUS — it has no variable child to gather");
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
    (void)router.add_child("net/ws-server/down", link);

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

/**
 * @brief An ADVERTISE whose route is an ILLEGALLY-SPELLED PATH must bind nothing (#681).
 *
 * `wire::path_key` re-emitted EVERY child's payload through `wire::emit_name` with no type
 * check, so a peer's `PATH{VALUE "sink"}` composed the same key bytes as the legal
 * `PATH{NAME "sink"}` and bound a label to `/sink`. The arena tier, given the identical bytes,
 * answers `INVALID_PATH` (`op_resolve_walk.hpp`'s `path_lookup_key` rejects a non-NAME child) —
 * so the two tiers disagreed, which is the #436 shape one layer up.
 *
 * Driven through `on_frame` rather than by calling `path_key` directly: the defect lives in what
 * `resolve_route_vertex` accepts off the wire, and the RFC-0014 lesson here was that two silent
 * misroutes shipped because no test used the production wiring.
 */
void test_advertise_with_non_name_child_binds_nothing() {
    std::printf("an illegally-spelled ADVERTISE route binds no label (#681)\n");
    graph_t g;
    (void)g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    fwd_router_t router{g};
    rec_link_t up;
    (void)router.add_child("net/ws-client/up", up);

    // PATH{ VALUE "sink" } — same payload bytes as the legal PATH{ NAME "sink" }, illegal child
    // type. `docs/reference/05-protocol-tlvs.md` states the rule: "Each child MUST be a NAME TLV
    // (type=0x02); other types are invalid in PATH context."
    std::vector<std::byte> body;
    const std::string_view seg = "sink";
    tr::wire::emit_tlv(
        body, type_t::VALUE, opt_t{},
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(seg.data()), seg.size()));
    std::vector<std::byte> bad_route;
    tr::wire::emit_tlv(bad_route, type_t::PATH, opt_t{.pl = true}, body);

    router.on_frame("net/ws-client/up", tr::net::encode_advertise(21, bad_route));
    router.on_frame("net/ws-client/up", tr::net::encode_compact(21, value_tlv(7)));

    check(stored_byte(g, "/sink") != 7,
          "a PATH{VALUE} route does not resolve /sink and delivers nothing");

    // The legal spelling still works, so the fix rejects the malformed child rather than the op.
    router.on_frame("net/ws-client/up", tr::net::encode_advertise(22, path_tlv({"sink"})));
    router.on_frame("net/ws-client/up", tr::net::encode_compact(22, value_tlv(5)));
    check(stored_byte(g, "/sink") == 5, "the legal PATH{NAME} spelling still binds and delivers");
}

int main() {
    test_warm_terminus_delivers();
    test_retire_invalidates_cached_handle();
    test_link_teardown_invalidates_cached_slot();
    test_corrupt_crc_compact_is_dropped();
    test_label_tlv_matches_the_generic_emitter();
    test_gathered_egress_matches_the_built_frame();
    test_gathered_advertise_matches_the_built_frame();
    test_forwarding_hop_advertise_is_gathered();
    test_gathered_handle_nack_matches_the_built_frame();
    test_iov_gather_drops_on_oom();
    test_advertise_with_non_name_child_binds_nothing();

    return tr::testing::summary("compact_cache");
}
