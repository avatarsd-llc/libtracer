/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a mount name is any number of route segments, and a hop consumes the
 *        WHOLE width in one step (RFC-0014 strip-K).
 *
 * A child's name does not have to be one route segment. It is a mount PATH — `"up"`,
 * `"ws-server/up"`, the RFC-0014 shape `"net/<module>/<name>"`, or something deeper — and the
 * forward path matches it as a unit: one pass over the registry, each slot tested against the
 * prefix of that slot's OWN width, longest match wins (`child_registry_t::longest_prefix`,
 * ADR-0061). There is no compile-time bound on K to raise; the width is bounded only by the
 * path-depth budget every address already spends from (#523).
 *
 * The half that is easy to get wrong is the OTHER direction. Strip-K on `dst` must be matched
 * by grow-K on `src`: the hop prepends the full mount path of the link the frame arrived on,
 * not a single route segment and not a truncation of it. Prepending less would produce a return
 * route that no longer names the inbound link once names are per-module scoped — a reply that
 * cannot get home (the ADR-0061 erratum).
 *
 * So the example wires BOTH sides qualified: a three-wide inbound mount and a three-wide
 * outbound one, and checks the emitted frame byte-for-byte. It also checks the property that
 * makes longest-prefix worth having — a deeper mount wins over a shallower one that also
 * matches, which is how two modules keep same-named connections distinct.
 *
 * Runs under ctest as `example_route_qualified_mount`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::fwd_op_t;
using tr::graph::graph_t;
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

}  // namespace

int main() {
    bool ok = true;
    graph_t g;
    tr::net::fwd_router_t router(g);

    // Both links are RFC-0014 qualified mounts, three route segments each. `net/<module>/<name>`
    // is a convention, not a parse: the registry stores the name and matches it as route segments.
    recording_link_t downstream, upstream;
    if (!router.add_child("net/ws-client/b", downstream) ||
        !router.add_child("net/ws-server/cli", upstream)) {
        std::fprintf(stderr, "route_qualified_mount: add_child failed — nothing registered\n");
        return 1;
    }

    router.on_frame("net/ws-server/cli",
                    fwd_write({"net", "ws-client", "b", "sensor", "temp"}, {}));
    check(ok, downstream.sent.size() == 1, "the three-wide mount matched and forwarded");
    if (downstream.sent.size() != 1) return 1;

    // K = 3 off dst, K = 3 onto src, in one hop. Anything less on either side is a defect:
    // too little stripped loops the frame, too little grown loses the reply.
    const std::vector<std::byte> expected =
        fwd_write({"sensor", "temp"}, {"net", "ws-server", "cli"});
    check(ok, downstream.sent[0] == expected,
          "dst lost all THREE mount route segments and src gained all three — byte-exact");

    // Longest-prefix, not first-match: a deeper mount that also matches wins. This is what
    // keeps two modules' same-named connections from colliding.
    recording_link_t deeper;
    const bool added_deeper = router.add_child("net/ws-client/b/inner", deeper);
    check(ok, added_deeper, "a deeper mount registers alongside the shallower one");
    downstream.sent.clear();
    router.on_frame("net/ws-server/cli", fwd_write({"net", "ws-client", "b", "inner", "leaf"}, {}));
    check(ok, deeper.sent.size() == 1 && downstream.sent.empty(),
          "the LONGEST matching mount took the frame, not the first one that matched");
    check(
        ok,
        !deeper.sent.empty() && deeper.sent[0] == fwd_write({"leaf"}, {"net", "ws-server", "cli"}),
        "and it stripped its own width — four route segments, not the shallower mount's three");

    std::printf("mount width is per-slot: stripped 3 for /net/ws-client/b, 4 for its /inner\n");
    return ok ? 0 : 1;
}
