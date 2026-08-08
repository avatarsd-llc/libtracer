/**
 * @file
 * @brief #974 — a label-compacted (COMPACT) delivery is ACL-gated under the INBOUND LINK's
 *        name, exactly like the full-route `FWD{WRITE}` it is the compaction of.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * @section defect The defect
 *
 * `graph_t::acl_allows` settles the EMPTY caller context as fully trusted before it invokes
 * the subject resolver (#905) — that arm is the LOCAL API call. Both terminus write arms of
 * `fwd_router_t::on_compact` used to reach the graph with the DEFAULT (empty) caller, so a
 * peer whose flow was auto-promoted to COMPACT (RFC-0004 §E.1) wrote an ACL-protected vertex
 * with no ACE evaluated at all — while the equivalent full-route `FWD{WRITE}` from the same
 * peer, to the same vertex, was denied. Two delivery forms of one write, disagreeing about
 * whether a policy applies. RFC-0004 §E.1 makes a `COMPACT` the terminus expansion of the
 * bound route "and applies the write (delivery-is-a-write, §D)", and §F gates the target
 * vertex's `:acl` at the final hop — so the gate is what the RFC already asks for, and no
 * byte of the wire surface changes here (a denied COMPACT is dropped, as an unwritable one
 * always was). ADR-0062 had already ruled the shape — "a binding caches the address, never
 * the authorization … a label must not become a capability that outlives the grant that
 * created it" — so this restores a documented commitment rather than inventing a policy.
 *
 * @section arms The two arms are proven SEPARATELY
 *
 * `on_compact`'s terminus has two write sites and a test that exercised only one would leave
 * the other free to regress:
 *
 *  - the COLD/STALE arm (`deliver_local`) — decode the bound route, `graph_.find`, write;
 *  - the WARM arm — the memoized `vertex_handle_t`, written with no decode and no find.
 *
 * @ref test_compact_denied_on_the_cold_arm reaches the cold arm only (its first COMPACT is
 * denied, so nothing is ever memoized). @ref test_compact_denied_on_the_warm_arm first lets
 * one ALLOWED delivery land — which is what memoizes the resolution — then revokes the right
 * and sends another COMPACT on the same link and label. Reverting one production hunk at a
 * time reddens exactly one of the two, which is the evidence that each is really on the arm
 * it names.
 *
 * @section positive Every denial is paired with a positive control
 *
 * A dropped delivery is invisible, so "the value did not change" is also what a wedged
 * router, an unbound label, or a mistyped path produces. Each case therefore ends (or
 * begins) with the SAME flow landing when the ACL permits it, and
 * @ref test_no_resolver_still_delivers pins the default configuration — no subject resolver
 * installed, no enforcement, a COMPACT delivers exactly as before this change.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/route_handle.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::fwd_op_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::net::fwd_router_t;
using tr::net::transport_t;
using tr::wire::opt_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

/** @brief A link that records the frames the router sends back to it. */
class rec_link_t : public transport_t {
   public:
    void send(std::span<const std::byte> frame) override {
        sent.emplace_back(frame.begin(), frame.end());
    }
    std::vector<std::vector<std::byte>> sent; /**< @brief Frames the router pushed here. */
};

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    if (!s.empty()) std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief A `PATH` TLV over the given `/`-segments. */
std::vector<std::byte> b_path(std::initializer_list<std::string_view> segs) {
    std::vector<std::byte> body;
    for (const std::string_view s : segs) tr::wire::emit_name(body, s);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::PATH, opt_t{.pl = true}, body);
    return out;
}

/** @brief An opaque `VALUE` TLV holding a little-endian `u32`. */
std::vector<std::byte> b_value_u32(std::uint32_t v) {
    std::vector<std::byte> raw(4);
    tr::detail::store_le(std::span<std::byte>(raw), v, 4);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, raw);
    return out;
}

/** @brief An opaque `VALUE` TLV holding one byte. */
std::vector<std::byte> b_value_u8(std::uint8_t v) {
    const std::byte b{v};
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, std::span<const std::byte>(&b, 1));
    return out;
}

/** @brief A `FWD` frame with the RFC-0004 §B child order. */
std::vector<std::byte> b_fwd(fwd_op_t op, const std::vector<std::byte>& dst,
                             const std::vector<std::byte>& src,
                             const std::vector<std::byte>& payload) {
    std::vector<std::byte> body;
    const std::vector<std::byte> opv = b_value_u8(static_cast<std::uint8_t>(op));
    body.insert(body.end(), opv.begin(), opv.end());
    body.insert(body.end(), dst.begin(), dst.end());
    body.insert(body.end(), src.begin(), src.end());
    body.insert(body.end(), payload.begin(), payload.end());
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::FWD, opt_t{.pl = true}, body);
    return out;
}

/** @brief A rope over @p bytes (single link) — the shape `graph_t::write` takes. */
tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief The `u32` a vertex currently holds, or `nullopt` if it holds nothing usable. */
std::optional<std::uint32_t> stored_u32(const graph_t& g, vertex_handle_t v) {
    const auto ref = g.read(v);  // the trusted local context — never gated
    if (!ref || !*ref) return std::nullopt;
    if ((*ref)->total_length() == 0 || (*ref)->link_count() != 1) return std::nullopt;
    const auto tlv = tr::wire::decode((*ref)->only());
    if (!tlv || tlv->payload.size() != 4) return std::nullopt;
    return tr::detail::load_le<std::uint32_t>(tlv->payload);
}

/**
 * @brief The test resolver (ADR-0018): the caller context IS the subject token.
 *
 * The empty (local) context never reaches a resolver — `graph_t::acl_allows` settles it as
 * trusted before invoking one (#905) — so the error arm here would mean DENY, and is unused.
 */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(std::string_view caller) {
    return as_bytes(caller);
}

/** @brief An `ACL` TLV granting @p right to @p subject alone (ALLOW-only, ADR-0020). */
std::vector<std::byte> acl_allowing(std::string_view subject, acl_right_t right) {
    const std::vector<tr::graph::ace_t> aces{
        tr::graph::ace_t{.type = tr::graph::ace_type_t::ALLOW,
                         .flags = 0,
                         .subject = as_bytes(subject),
                         .access_mask = static_cast<std::uint32_t>(right),
                         .expires_ns = 0}};
    return tr::graph::encode_acl(aces);
}

/** @brief Install @p acl at `<vertex>:acl` under the trusted local context. */
bool install_acl(graph_t& g, std::string_view vertex_acl_path, const std::vector<std::byte>& acl) {
    const auto p = path_t::parse(vertex_acl_path);
    if (!p) return false;
    return g.write(*p, make_value(acl)).has_value();
}

constexpr std::uint16_t kLabel = 0x0311u;
constexpr std::uint32_t kSeed = 0x11111111u;
constexpr std::uint32_t kAttack = 0x22222222u;
constexpr std::uint32_t kAllowed = 0x33333333u;

// --- the COLD arm: deliver_local ----------------------------------------------------

/**
 * @brief A COMPACT from a link the target's `:acl` does not grant WRITE is DROPPED.
 *
 * The cold/stale arm: nothing was ever memoized for this label (the very first COMPACT on it
 * is the denied one), so the write goes through `deliver_local`. Pre-fix that call reached
 * `graph_t::write` with the default empty caller — the local-trusted short-circuit — and the
 * attacker's value landed on a vertex whose ACL names only `peer-z`.
 */
void test_compact_denied_on_the_cold_arm() {
    std::printf("a COMPACT from an ACL-denied link does not write (cold arm):\n");
    graph_t g;
    const vertex_handle_t sink = g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    check(g.write(sink, make_value(b_value_u32(kSeed))).has_value(),
          "the target holds a seeded value (trusted local write, before any ACL)");
    check(install_acl(g, "/sink:acl", acl_allowing("peer-z", acl_right_t::WRITE)),
          "an ACL granting WRITE to `peer-z` ALONE is installed");
    g.set_subject_resolver(caller_is_subject);  // enforcement on

    fwd_router_t router(g);
    rec_link_t hostile;  // NOT `peer-z`
    rec_link_t friendly;
    router.add_child("peer-h", hostile);
    router.add_child("peer-z", friendly);

    // The attack shape: bind a label to the protected route, then stream COMPACT.
    router.on_frame("peer-h", tr::net::encode_advertise(kLabel, b_path({"sink"})));
    router.on_frame("peer-h", tr::net::encode_compact(kLabel, b_value_u32(kAttack)));
    check(stored_u32(g, sink) == kSeed,
          "the denied link's COMPACT did NOT overwrite the value (ACL evaluated)");

    // Positive control #1 — the identical flow from the GRANTED link lands, so the denial
    // above is attributable to the ACL and not to a wedged router or an unbound label.
    router.on_frame("peer-z", tr::net::encode_advertise(kLabel, b_path({"sink"})));
    router.on_frame("peer-z", tr::net::encode_compact(kLabel, b_value_u32(kAllowed)));
    check(stored_u32(g, sink) == kAllowed,
          "instrument: the SAME COMPACT flow from the granted link DOES deliver");

    // Positive control #2 — the full-route form of the same write, from the same denied
    // link, to the same vertex. This is the disagreement #974 named: after the fix the two
    // forms agree, and this check is what would catch them diverging again.
    router.on_frame("peer-h", b_fwd(fwd_op_t::WRITE, b_path({"sink"}), b_path({"peer-h"}),
                                    b_value_u32(kAttack)));
    check(stored_u32(g, sink) == kAllowed,
          "and the full-route FWD{WRITE} from that link is denied too — the forms agree");
}

// --- the WARM arm: the memoized vertex handle ---------------------------------------

/**
 * @brief Authorization is re-evaluated per COMPACT, not memoized with the resolution.
 *
 * The warm arm writes a cached `vertex_handle_t` with no decode and no `graph_.find`. Getting
 * there requires one delivery that SUCCEEDS (that is what calls `cache_resolution`), so this
 * case grants the right, delivers, then REVOKES it and sends another COMPACT on the same link
 * and label. Two properties at once: the warm arm is gated at all, and it is gated against
 * the CURRENT ACL rather than the one in force when the flow warmed.
 */
void test_compact_denied_on_the_warm_arm() {
    std::printf("a COMPACT on a WARM binding is re-authorized per frame:\n");
    graph_t g;
    const vertex_handle_t sink = g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    check(g.write(sink, make_value(b_value_u32(kSeed))).has_value(), "the target is seeded");
    check(install_acl(g, "/sink:acl", acl_allowing("peer-z", acl_right_t::WRITE)),
          "an ACL granting WRITE to `peer-z` is installed");
    g.set_subject_resolver(caller_is_subject);

    fwd_router_t router(g);
    rec_link_t z;
    router.add_child("peer-z", z);

    router.on_frame("peer-z", tr::net::encode_advertise(kLabel, b_path({"sink"})));
    router.on_frame("peer-z", tr::net::encode_compact(kLabel, b_value_u32(kAllowed)));
    check(stored_u32(g, sink) == kAllowed,
          "the granted link's first COMPACT delivers — which MEMOIZES the resolution");

    // Revoke: the same vertex, an ACL that now names somebody else. The label, the link and
    // the binding are untouched, so the next COMPACT takes the WARM arm.
    check(install_acl(g, "/sink:acl", acl_allowing("peer-q", acl_right_t::WRITE)),
          "the ACL is rewritten to grant WRITE to `peer-q` instead");
    router.on_frame("peer-z", tr::net::encode_compact(kLabel, b_value_u32(kAttack)));
    check(stored_u32(g, sink) == kAllowed,
          "the next COMPACT on the WARM binding is denied — the value is unchanged");

    // Positive control — restore the grant and the same warm flow resumes, so the guard
    // drops deliveries the ACL forbids rather than wedging the binding.
    check(install_acl(g, "/sink:acl", acl_allowing("peer-z", acl_right_t::WRITE)),
          "the grant is restored");
    router.on_frame("peer-z", tr::net::encode_compact(kLabel, b_value_u32(kAttack)));
    check(stored_u32(g, sink) == kAttack, "and the warm flow delivers again once re-granted");
}

// --- the default configuration: no resolver, no enforcement -------------------------

/**
 * @brief With no subject resolver installed, a COMPACT delivers exactly as before.
 *
 * Enforcement is opt-in (ADR-0018): `acl_allows` returns true on its first line when no
 * resolver is set, whatever the caller. Carrying the inbound link's name must therefore
 * change nothing at all for the default configuration — including on a vertex that HOLDS a
 * restrictive ACL, which is stored but not enforced.
 */
void test_no_resolver_still_delivers() {
    std::printf("no subject resolver => a COMPACT delivers, ACL present or not:\n");
    graph_t g;
    const vertex_handle_t sink = g.register_vertex(*path_t::parse("/sink"), role_t::STORED_VALUE);
    check(g.write(sink, make_value(b_value_u32(kSeed))).has_value(), "the target is seeded");
    check(install_acl(g, "/sink:acl", acl_allowing("peer-z", acl_right_t::WRITE)),
          "a restrictive ACL is stored (but no resolver is installed)");

    fwd_router_t router(g);
    rec_link_t any;
    router.add_child("peer-h", any);
    router.on_frame("peer-h", tr::net::encode_advertise(kLabel, b_path({"sink"})));
    router.on_frame("peer-h", tr::net::encode_compact(kLabel, b_value_u32(kAllowed)));
    check(stored_u32(g, sink) == kAllowed, "the cold COMPACT delivers (enforcement disabled)");
    router.on_frame("peer-h", tr::net::encode_compact(kLabel, b_value_u32(kAttack)));
    check(stored_u32(g, sink) == kAttack, "and so does the warm one");
}

}  // namespace

int main() {
    std::printf("COMPACT delivery is ACL-gated under the inbound link (#974)\n\n");

    test_compact_denied_on_the_cold_arm();
    std::printf("\n");
    test_compact_denied_on_the_warm_arm();
    std::printf("\n");
    test_no_resolver_still_delivers();

    std::printf("\n%s\n", g_failures == 0 ? "all checks passed" : "FAILURES");
    return g_failures == 0 ? 0 : 1;
}
