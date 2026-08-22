/**
 * @file
 * @brief #739 — `fwd_router_t::subscribe_toward`: bind a local producer toward a
 *        mount-path target through the shared strip-K descent (ADR-0061).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The gap this closes: the host surface offered only `subscribe(src, local-target)`
 * (drops for a `/net/...` target) and `subscribe_wire(v, source, return_route, link)`
 * (caller must pre-split) — so every embedder hand-derived the `(link, route)` split,
 * baking in a single-hop assumption and the `net/<module>/<name>` string shape. The
 * helper resolves ONE ordinary mount path through the SAME cached descent the forward
 * path uses, so a bound route and a routed frame cannot disagree about where a mount
 * ends.
 *
 * The observable: after the bind, a producer write must EMIT a full-route
 * `FWD{ WRITE, dst=<residual>, src=<empty> }` on the matched link, with the residual
 * spelled exactly as the target's suffix below the mount — including the nested
 * multi-hop form, where the residual itself begins with the NEXT node's mount run.
 */

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;

/** @brief A span link recording every send — the delivery egress under observation. */
class fake_link_t : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        sent_.emplace_back(frame.begin(), frame.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte> whole;
        for (const auto s : iov) whole.insert(whole.end(), s.begin(), s.end());
        sent_.push_back(std::move(whole));
    }
    std::vector<std::vector<std::byte>>& sent() { return sent_; }

   private:
    std::vector<std::vector<std::byte>> sent_;
};

/** @brief A view_t over fresh owned bytes. */
view_t owned(std::initializer_list<std::uint8_t> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    std::size_t i = 0;
    for (const std::uint8_t b : bytes) seg->bytes[i++] = std::byte{b};
    return view_t::over(std::move(seg));
}

/** @brief The canonical PATH TLV over the given segments (the expected dst spelling). */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief Decode a sent frame; true iff it is FWD{WRITE} whose dst equals @p want_dst.
 *
 * The dst is compared as RAW RECORD BYTES at its position in the body (right after the
 * 5-byte op TLV) — `decode` parses a PL composite's body into `.children`, so a re-encode
 * from `.payload` would compare an empty shell. Byte-position comparison also pins the
 * child ORDER the spec fixes (op, dst, src, payload).
 */
bool is_write_toward(std::span<const std::byte> frame, const std::vector<std::byte>& want_dst) {
    const auto dec = tr::wire::decode(frame);
    if (!dec || dec->type != type_t::FWD || dec->children.size() < 3) return false;
    const auto& op = dec->children[0];
    if (op.type != type_t::VALUE || op.payload.size() != 1 ||
        op.payload[0] != std::byte{std::to_underlying(fwd_op_t::WRITE)})
        return false;
    // Outer FWD header is 4 bytes (u16 length), op TLV is 5 — dst records start at 9.
    constexpr std::size_t kDstAt = 4 + 5;
    if (frame.size() < kDstAt + want_dst.size()) return false;
    return std::equal(want_dst.begin(), want_dst.end(), frame.begin() + kDstAt);
}

void test_single_hop() {
    std::printf("subscribe_toward — direct peer, dst is the residual below the mount:\n");
    graph_t g;
    fwd_router_t router(g);
    fake_link_t b;
    (void)router.add_child("net/ws-client/b", b);
    (void)g.register_vertex(path_t("/light/rgb"), role_t::STORED_VALUE);

    // Negative control FIRST: an unbound producer write emits nothing.
    (void)g.write(path_t("/light/rgb"), owned({0x01, 0x00, 0x01, 0x00, 0x2A}));
    check(b.sent().empty(), "before the bind, a producer write emits nothing");

    const auto s =
        router.subscribe_toward(path_t("/light/rgb"), path_t("/net/ws-client/b/display/val"));
    check(s.has_value(), "the bind resolves the mount and admits the subscription");

    (void)g.write(path_t("/light/rgb"), owned({0x01, 0x00, 0x01, 0x00, 0x2A}));
    check(b.sent().size() == 1, "the producer write emitted exactly one delivery on the link");
    check(!b.sent().empty() && is_write_toward(b.sent().back(), b_path({"display", "val"})),
          "the delivery is FWD{WRITE} with dst = /display/val (the residual, B-rooted)");
}

void test_multi_hop_residual() {
    std::printf("subscribe_toward — nested target, the residual is the NEXT hop's route:\n");
    graph_t g;
    fwd_router_t router(g);
    fake_link_t b;
    (void)router.add_child("net/ws-client/b", b);
    (void)g.register_vertex(path_t("/s/t"), role_t::STORED_VALUE);

    const auto s =
        router.subscribe_toward(path_t("/s/t"), path_t("/net/ws-client/b/net/can/c/leaf"));
    check(s.has_value(), "a nested /net/A/net/.../x target binds");
    (void)g.write(path_t("/s/t"), owned({0x01, 0x00, 0x01, 0x00, 0x07}));
    check(
        !b.sent().empty() && is_write_toward(b.sent().back(), b_path({"net", "can", "c", "leaf"})),
        "dst carries the whole residual — B's own descent resolves the next mount");
}

void test_rejections() {
    std::printf("subscribe_toward — rejections, by value:\n");
    graph_t g;
    fwd_router_t router(g);
    fake_link_t b;
    (void)router.add_child("net/ws-client/b", b);
    (void)g.register_vertex(path_t("/p"), role_t::STORED_VALUE);

    const auto no_vertex = router.subscribe_toward(path_t("/nope"), path_t("/net/ws-client/b/x"));
    check(!no_vertex.has_value() && no_vertex.error() == status_t::NOT_FOUND,
          "an unknown producer => NOT_FOUND");

    const auto no_mount = router.subscribe_toward(path_t("/p"), path_t("/local/thing"));
    check(!no_mount.has_value() && no_mount.error() == status_t::INVALID_PATH,
          "a target that routes through no mount => INVALID_PATH");

    const auto exact = router.subscribe_toward(path_t("/p"), path_t("/net/ws-client/b"));
    check(!exact.has_value() && exact.error() == status_t::INVALID_PATH,
          "a target naming the mount exactly (nothing below it) => INVALID_PATH");
}

/** @brief A SUBSCRIBER TLV whose PATH child spells @p target — the wire mount-routed form. */
std::vector<std::byte> b_subscriber_toward(std::initializer_list<std::string_view> target) {
    std::vector<std::byte> body;
    std::vector<std::byte> segs;
    for (const std::string_view s : target) (void)tr::wire::emit_path_segment(segs, s);
    tr::wire::emit_tlv(body, type_t::PATH, opt_t{}, segs);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SUBSCRIBER, opt_t{.pl = true}, body);
    return out;
}

/** @brief A view_t over an owned copy of @p bytes. */
view_t owned_bytes(const std::vector<std::byte>& bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); ++i) seg->bytes[i] = bytes[i];
    return view_t::over(std::move(seg));
}

/**
 * @brief #1437 — the two un-carried subscribe doors now carry the MOUNT's token.
 *
 * The observable is `graph_t::link_index_name_lookups`, the carry's own counter: it counts
 * index inserts that had to fall back to the graph's NAME door, which is a linear scan of the
 * live link slots. Both doors here key the index on a mount's name — `subscribe_toward`
 * because that is the only link it binds, the wire door because RFC-0021 §4.B.1 REBINDS the
 * key from the arrival link to the mount — so before this change both scanned, once per
 * subscribe, and the count rose by one every time.
 *
 * Driven at a link count on the wrong side of the crossover the trade was argued at (the door
 * is cheaper than the hash it replaced up to ~32 live links and ~2x dearer at 65), and driven
 * REPEATEDLY, because the complaint is a steady-state per-subscribe cost and not a one-off.
 * The arrival link is deliberately a DIFFERENT mount from the delivery one, which is the whole
 * shape of §4.B.1: a correct carried token that names the wrong key must not be trusted, and
 * before the mount's own token was plumbed through there was nothing to replace it with.
 */
void test_mount_carry_never_scans() {
    std::printf("subscribe_toward / mount-routed wire door — the mount's token is carried:\n");
    constexpr std::size_t kLinks = 64;  // past the ~32-link crossover the scan is priced at
    graph_t g;
    fwd_router_t router(g);
    std::deque<fake_link_t> links(kLinks);
    std::vector<std::string> names(kLinks);
    for (std::size_t i = 0; i < kLinks; ++i) {
        names[i] = "net/ws-client/p" + std::to_string(i);
        (void)router.add_child(names[i], links[i]);
    }
    (void)g.register_vertex(path_t("/light/rgb"), role_t::STORED_VALUE);
    const std::optional<tr::graph::vertex_handle_t> prod = g.find(path_t("/light/rgb").key());
    check(prod.has_value(), "the producer resolves");

    // Every mount is interned up front, so the counter below cannot be flattered by a link the
    // index has simply never heard of: all 64 slots are live and a scan would have work to do.
    for (std::size_t i = 0; i < kLinks; ++i) (void)g.intern_link(names[i]);
    const std::size_t before_toward = g.link_index_name_lookups();

    // Door 1: the host-side bind, over every mount, twice — a fresh subscribe and a repeat.
    bool bound = true;
    for (int round = 0; round < 2; ++round)
        for (std::size_t i = 0; i < kLinks; ++i)
            bound = bound && router
                                 .subscribe_toward(path_t("/light/rgb"),
                                                   path_t("/net/" + names[i].substr(4) + "/d/v"))
                                 .has_value();
    check(bound, "every subscribe_toward bound through its mount");
    check(g.link_index_name_lookups() == before_toward,
          "subscribe_toward indexed under the mount with NO name-door scan, at any link count");

    // Door 2: the wire `:subscribers[]` append whose PATH child routes through a mount. The
    // subscribe ARRIVES over p0 and carries p0's token, correctly — and the index is keyed on
    // the mount the TARGET names, so that token is the wrong one and is rightly rejected.
    // Before the mount's own token was plumbed through, there was nothing to put in its place.
    const tr::graph::link_id_t arrival = g.intern_link(names[0]);
    check(arrival.valid(), "the arrival link is interned, so a real token is carried in");
    const std::vector<std::byte> route = b_path({"d", "v"});
    const std::size_t before_wire = g.link_index_name_lookups();
    bool admitted = true;
    for (std::size_t i = 1; i < kLinks; ++i) {
        const std::vector<std::byte> sub =
            b_subscriber_toward({"net", "ws-client", "p" + std::to_string(i), "d", "v"});
        admitted = admitted && g.subscribe_wire(*prod, owned_bytes(sub), owned_bytes(route),
                                                names[0], view_t{}, {}, arrival)
                                   .has_value();
    }
    check(admitted, "every mount-routed wire subscribe is admitted");
    check(g.link_index_name_lookups() == before_wire,
          "the §4.B.1 rebind swapped in the MOUNT's token instead of degrading to the scan");
}

}  // namespace

int main() {
    std::printf("fwd_router_t::subscribe_toward (#739, ADR-0061 shared descent):\n");
    test_single_hop();
    test_multi_hop_residual();
    test_rejections();
    test_mount_carry_never_scans();
    return tr::testing::summary("subscribe_toward");
}
