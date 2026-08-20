/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — one test decides a hop's whole behaviour: does the leading `dst`
 *        route segment name a CHILD?
 *
 * A node does not classify frames into "local" and "remote" traffic, and a `dst` carries no
 * flag saying which it is. Every inbound `FWD` is put to the same question — is the leading
 * `dst` route segment one of my registered children? Yes ⇒ forward it (dst-shrink / src-grow, see
 * route_dst_is_source_route). No ⇒ **I am the terminus**: resolve the whole remaining `dst` as
 * a local address and apply the op (RFC-0004 §D, ADR-0035).
 *
 * The consequence worth internalising: the SAME bytes are a forward at one node and a terminus
 * at the next, decided entirely by each node's own child table. `/sensor/temp` is a route while
 * a child is called `sensor`, and an address the moment that child is gone. Addressing and
 * routing share one namespace on purpose (`CONTEXT.md` §Path-as-route).
 *
 * The terminus arm also has a failure mode that is NOT a drop: a `dst` that resolves to no
 * local vertex answers `FWD{REPLY}` with `kind=ERROR`, source-routed home along the `src` the
 * request accumulated. A forwarder that silently swallowed unroutable frames would make every
 * addressing typo a timeout instead of an error.
 *
 * Runs under ctest as `example_route_terminus_or_forward`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::reply_kind_t;
using tr::graph::role_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/** @brief A `transport_t` that keeps every frame handed to it — the "wire", made inspectable. */
struct recording_link_t : tr::net::transport_t {
    std::vector<std::vector<std::byte>> sent; /**< @brief Frames emitted on this link, in order. */
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    void send(std::span<const std::span<const std::byte>> iov) override {
        std::vector<std::byte> flat;
        for (const auto part : iov) flat.insert(flat.end(), part.begin(), part.end());
        sent.push_back(std::move(flat));
    }
};

/** @brief A `PATH` TLV over @p segs — RFC-0018 packed segment records, `opt.PL` clear. */
std::vector<std::byte> path_tlv(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (std::string_view s : segs) (void)tr::wire::emit_path_segment(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{}, body);
    return out;
}

/** @brief A one-byte `VALUE` TLV carrying @p v. */
std::vector<std::byte> value_tlv(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief `FWD[.pl]{ VALUE op, PATH dst, PATH src, VALUE payload }` (RFC-0004 §B child order). */
std::vector<std::byte> fwd_write(std::initializer_list<std::string_view> dst,
                                 std::initializer_list<std::string_view> src) {
    std::vector<std::byte> body = value_tlv(static_cast<std::uint8_t>(fwd_op_t::WRITE));
    for (const auto& part : {path_tlv(dst), path_tlv(src), value_tlv(0x2A)}) {
        body.insert(body.end(), part.begin(), part.end());
    }
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief The `kind` of a `FWD{REPLY}` frame, or `std::nullopt` if @p frame is not one. */
std::optional<reply_kind_t> reply_kind(std::span<const std::byte> frame) {
    const auto tlv = tr::wire::decode(frame);
    // Child order is `VALUE op, PATH dst, PATH src, VALUE kind, …` — the kind is child 3.
    if (!tlv || tlv->children.size() < 4 || tlv->children[3].payload.size() != 1)
        return std::nullopt;
    return static_cast<reply_kind_t>(tlv->children[3].payload[0]);
}

}  // namespace

int main() {
    bool ok = true;
    graph_t g;
    tr::net::fwd_router_t router(g);

    // One child, named "b"; one local vertex, at /sensor/temp. Both spellings below are
    // ordinary paths — nothing marks one of them as "remote".
    recording_link_t to_b, to_client;
    if (!router.add_child("b", to_b) || !router.add_child("cli", to_client)) {
        std::fprintf(stderr, "route_terminus_or_forward: add_child failed — nothing registered\n");
        return 1;
    }
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    // Arm 1 — the leading route segment names a child: FORWARD. Nothing is written here.
    router.on_frame("cli", fwd_write({"b", "sensor", "temp"}, {}));
    check(ok, to_b.sent.size() == 1, "\"b\" names a child, so the frame left on that child");
    check(ok, !g.read(path_t("/sensor/temp")).has_value(),
          "and the identically-named LOCAL vertex was not touched");

    // Arm 2 — the leading route segment names no child: TERMINUS. The same trailing route
    // segments are now an address, and the write lands in this node's graph.
    to_b.sent.clear();
    router.on_frame("cli", fwd_write({"sensor", "temp"}, {}));
    check(ok, to_b.sent.empty(), "\"sensor\" names no child, so nothing was forwarded");
    check(ok, g.read(path_t("/sensor/temp")).has_value(),
          "this node was the terminus: the op applied to the LOCAL vertex");

    // Arm 3 — terminus, but the address resolves to nothing. The refusal is ADDRESSED, not
    // dropped: a REPLY with kind=ERROR goes back down the src the request accumulated.
    to_client.sent.clear();
    router.on_frame("cli", fwd_write({"no", "such", "vertex"}, {"app"}));
    check(ok, to_client.sent.size() == 1,
          "an unresolvable dst ANSWERS on the inbound link — it is not swallowed");
    check(ok, !to_client.sent.empty() && reply_kind(to_client.sent[0]) == reply_kind_t::ERROR,
          "and the answer is FWD{REPLY} kind=ERROR, so the caller sees a status, not a timeout");

    std::printf(
        "one test, two behaviours: \"b\" is a child (forward), \"sensor\" is not (terminus)\n");
    return ok ? 0 : 1;
}
