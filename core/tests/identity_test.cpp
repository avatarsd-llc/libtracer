/**
 * @file
 * @brief #406 / RFC-0011 — the node identity facet: `read <vertex>:identity`.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Asserts the four properties the facet exists for:
 *   - the record is BYTE-EXACT per RFC-0011 §B (the 60-byte ed25519 form), pinned here
 *     because the same bytes are the cross-path dedup key, the TOFU pin and (later) the
 *     ACL subject — three consumers that must agree without a translation table;
 *   - EVERY vertex answers identically (§C.1) — the node-scoped invariant that makes the
 *     record a valid cross-path key at all;
 *   - the read is PRE-AUTH (§C.2) — served through a closed ACL, to `anonymous`, because
 *     the public key is exactly what an unauthenticated peer needs to pin and to verify
 *     the ADR-0045 challenge; gating it would deadlock first contact;
 *   - a keyless node keeps SCHEMA_NOT_FOUND (§C.3) — the surface is ABSENT, not empty.
 *
 * No crypto is exercised, and none is involved: the facet stores and serves a claim
 * (ADR-0045 decision 3 — "the public key *is* the identity"). Proving a node HOLDS the
 * key is authentication and lives elsewhere.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/security_acl.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"

namespace {

using tr::graph::ace_t;
using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::wire::type_t;

int g_failures = 0;

void check(bool ok, std::string_view what) {
    std::printf("  [%s] %.*s\n", ok ? "PASS" : "FAIL", static_cast<int>(what.size()), what.data());
    if (!ok) ++g_failures;
}

std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The test resolver (ADR-0018): caller bytes are the subject; empty = trusted/local. */
std::optional<subject_token_t> caller_is_subject(std::string_view caller) {
    if (caller.empty()) return std::nullopt;
    return as_bytes(caller);
}

tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

/** @brief A recognisable 32-byte ed25519-shaped key: 0x00,0x01,…,0x1F. */
std::array<std::byte, 32> demo_key() {
    std::array<std::byte, 32> k{};
    for (std::size_t i = 0; i < k.size(); ++i) k[i] = static_cast<std::byte>(i);
    return k;
}

std::string hex(std::span<const std::byte> b) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(b.size() * 2);
    for (std::byte x : b) {
        out.push_back(d[std::to_integer<unsigned>(x) >> 4]);
        out.push_back(d[std::to_integer<unsigned>(x) & 0xF]);
    }
    return out;
}

/** @brief Read `path:field` as @p caller — the handle+field overload the FWD resolver uses. */
tr::graph::result_t<tr::view::rope_t> read_as(graph_t& g, const char* path,
                                              std::string_view caller) {
    const auto p = path_t::parse(path);
    if (!p) return std::unexpected(status_t::INVALID_PATH);
    const auto v = g.find(p->key());
    if (!v) return std::unexpected(status_t::NOT_FOUND);
    return g.read(*v, p->field(), caller);
}

/** @brief Read `path:identity` and return the flattened wire bytes (empty on error). */
std::vector<std::byte> read_identity_bytes(graph_t& g, const char* path, std::string_view caller) {
    const auto r = read_as(g, path, caller);
    if (!r) return {};
    const tr::view::view_t flat = r->flatten();
    const auto span = flat.bytes();
    return std::vector<std::byte>(span.begin(), span.end());
}

/**
 * @brief RFC-0011 §B: the record is byte-exact, and it is 60 bytes for ed25519.
 *
 * Pinned literally. These bytes are simultaneously the topology walk's dedup key, a
 * TOFU peer's pin, and (once ADR-0045's login lands) the ACL subject — so a drift here
 * silently desynchronises three consumers that are supposed to name one identity.
 */
void test_record_is_byte_exact() {
    std::printf("RFC-0011 §B: the ed25519 identity record is byte-exact (60 bytes):\n");
    graph_t g;
    const auto key = demo_key();
    check(g.set_identity(0x01, key).has_value(), "set_identity(ed25519, 32 bytes) installs");
    const auto v = g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)v;

    const auto bytes = read_identity_bytes(g, "/sensor/temp:identity", {});
    check(bytes.size() == 60, "the record is exactly 60 bytes (4 header + 56 payload)");

    // SETTINGS(0x0B, PL=1, len=56){ NAME "kind" VALUE u8=1, NAME "key" VALUE <32> }
    const std::string expect =
        "0b4038"
        "00"                // SETTINGS header: type=0x0B opt=0x40(PL) len=0x0038 LE
        "020004006b696e64"  // NAME "kind"
        "0100010001"        // VALUE u8 = 0x01 (ed25519)
        "020003006b6579"    // NAME "key"
        "01002000"          // VALUE, len=0x0020 (32)
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";  // the key
    check(hex(bytes) == expect, "the bytes are exactly the RFC-0011 §B worked example");

    // And it decodes to the shape §B fixes: two members, in the fixed order.
    const auto dec = tr::wire::decode(bytes);
    check(dec && dec->type == type_t::SETTINGS && dec->opt.pl, "it is a structured SETTINGS");
    check(dec && dec->children.size() == 4, "four positional children: kind/<u8>, key/<bytes>");
    if (dec && dec->children.size() == 4) {
        check(tr::detail::as_string_view(dec->children[0].payload) == "kind" &&
                  dec->children[1].type == type_t::VALUE && dec->children[1].payload.size() == 1 &&
                  std::to_integer<unsigned>(dec->children[1].payload[0]) == 0x01,
              "member 1 is NAME \"kind\" VALUE u8 = 0x01 (ed25519)");
        check(tr::detail::as_string_view(dec->children[2].payload) == "key" &&
                  dec->children[3].type == type_t::VALUE && dec->children[3].payload.size() == 32,
              "member 2 is NAME \"key\" VALUE of exactly 32 bytes");
    }
}

/** @brief RFC-0011 §C.1: node-scoped — every vertex serves the identical record. */
void test_every_vertex_answers_identically() {
    std::printf("RFC-0011 §C.1: the identity is the NODE's — every vertex answers alike:\n");
    graph_t g;
    check(g.set_identity(0x01, demo_key()).has_value(), "identity installed");
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/actuator/relay/0"), role_t::STORED_VALUE);
    (void)g.register_vertex(path_t("/net"), role_t::STORED_VALUE);

    const auto a = read_identity_bytes(g, "/sensor/temp:identity", {});
    const auto b = read_identity_bytes(g, "/actuator/relay/0:identity", {});
    const auto c = read_identity_bytes(g, "/net:identity", {});
    check(!a.empty() && a == b && b == c,
          "three unrelated vertices return BYTE-IDENTICAL records (the cross-path key)");

    // A vertex registered AFTER the install answers too — the record is not per-vertex state.
    (void)g.register_vertex(path_t("/late/arrival"), role_t::STORED_VALUE);
    check(read_identity_bytes(g, "/late/arrival:identity", {}) == a,
          "a vertex created after the install answers identically (nothing is per-vertex)");
}

/**
 * @brief RFC-0011 §C.2: the read is PRE-AUTH — exempt from the READ gate.
 *
 * The load-bearing test. The default ACL ships closed except the auth subtree, so if
 * `:identity` were READ-gated an unauthenticated peer could never obtain the key it
 * needs to authenticate — first contact would deadlock.
 */
void test_read_is_pre_auth() {
    std::printf("RFC-0011 §C.2: :identity is served through a CLOSED acl (pre-auth):\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    check(g.set_identity(0x01, demo_key()).has_value(), "identity installed");
    const auto v = g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);
    (void)g.write(v, make_value(demo_key()));

    // Close /dev to everyone but "alice": any present non-matching ACE closes the vertex.
    const std::vector<ace_t> grant{
        ace_t{.type = tr::graph::ace_type_t::ALLOW,
              .flags = 0,
              .subject = as_bytes("alice"),
              .access_mask = static_cast<std::uint32_t>(acl_right_t::READ),
              .expires_ns = 0}};
    check(g.write(path_t("/dev:acl"), make_value(tr::graph::encode_acl(grant))).has_value(),
          "an ACL granting only alice is installed on /dev");

    // The control: an ordinary read really is denied for mallory.
    const auto data = read_as(g, "/dev", "mallory");
    check(!data.has_value() && data.error() == status_t::PERMISSION_DENIED,
          "a DATA read by mallory is denied — the vertex is genuinely closed");

    // The subject: :identity is served anyway, to the same caller, on the same vertex.
    const auto id_mallory = read_identity_bytes(g, "/dev:identity", "mallory");
    check(id_mallory.size() == 60, "…yet :identity IS served to mallory through the closed ACL");
    check(id_mallory == read_identity_bytes(g, "/dev:identity", "alice"),
          "and it is the same record the authorized caller sees");

    // :acl itself stays gated — the exemption is narrow and names one field.
    const auto acl_read = read_as(g, "/dev:acl", "mallory");
    check(!acl_read.has_value() && acl_read.error() == status_t::PERMISSION_DENIED,
          ":acl stays READ_ACL-gated — the exemption applies to :identity ALONE");
}

/** @brief RFC-0011 §C.3: no keypair ⇒ SCHEMA_NOT_FOUND — the surface is absent, not empty. */
void test_keyless_node_is_enotty() {
    std::printf("RFC-0011 §C.3: a keyless node keeps SCHEMA_NOT_FOUND (the ENOTTY default):\n");
    graph_t g;
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    const auto r = read_as(g, "/sensor/temp:identity", {});
    check(!r.has_value() && r.error() == status_t::SCHEMA_NOT_FOUND,
          "no keypair => SCHEMA_NOT_FOUND, byte-for-byte the pre-RFC behaviour");

    // Install, read, clear, read: the facet appears and disappears with the keypair.
    check(g.set_identity(0x01, demo_key()).has_value(), "install");
    check(read_identity_bytes(g, "/sensor/temp:identity", {}).size() == 60, "…now it answers");
    g.clear_identity();
    const auto after = read_as(g, "/sensor/temp:identity", {});
    check(!after.has_value() && after.error() == status_t::SCHEMA_NOT_FOUND,
          "clear_identity() returns the node to the absent surface");
}

/** @brief RFC-0011 §B: the kind registry constrains the key length; violations are malformed. */
void test_registry_validation() {
    std::printf("RFC-0011 §B: the kind registry fixes the key length:\n");
    graph_t g;
    const auto key = demo_key();

    const auto bad_kind = g.set_identity(0x00, key);
    check(!bad_kind.has_value() && bad_kind.error() == status_t::TYPE_MISMATCH,
          "kind 0x00 is reserved-invalid => TYPE_MISMATCH");
    const auto unknown = g.set_identity(0x77, key);
    check(!unknown.has_value() && unknown.error() == status_t::TYPE_MISMATCH,
          "an unregistered kind => TYPE_MISMATCH (additions are RFC-gated)");

    std::array<std::byte, 31> short_key{};
    const auto too_short = g.set_identity(0x01, short_key);
    check(!too_short.has_value() && too_short.error() == status_t::TYPE_MISMATCH,
          "ed25519 with 31 bytes => TYPE_MISMATCH (the length must match the kind)");

    std::array<std::byte, 33> long_key{};
    const auto too_long = g.set_identity(0x01, long_key);
    check(!too_long.has_value() && too_long.error() == status_t::TYPE_MISMATCH,
          "ed25519 with 33 bytes => TYPE_MISMATCH");

    // A rejected install leaves the node keyless — never half-configured.
    (void)g.register_vertex(path_t("/x"), role_t::STORED_VALUE);
    const auto r = read_as(g, "/x:identity", {});
    check(!r.has_value() && r.error() == status_t::SCHEMA_NOT_FOUND,
          "a rejected install leaves the node keyless (no partial state)");
}

/** @brief The facet is READ-only: there is no write surface (RFC-0011 §A). */
void test_no_write_surface() {
    std::printf("RFC-0011: :identity has NO write surface:\n");
    graph_t g;
    check(g.set_identity(0x01, demo_key()).has_value(), "identity installed");
    (void)g.register_vertex(path_t("/sensor/temp"), role_t::STORED_VALUE);

    const auto w = g.write(path_t("/sensor/temp:identity"), make_value(demo_key()));
    check(!w.has_value() && w.error() == status_t::SCHEMA_NOT_FOUND,
          "a remote peer cannot write the identity — it is the owner's, via the host API");
    check(read_identity_bytes(g, "/sensor/temp:identity", {}).size() == 60,
          "and the record is unchanged by the attempt");
}

/**
 * @brief RFC-0011 §C.4: the record is served WHOLE — no member addressing, no indexed
 *        addressing — and the refusal is caller-independent.
 *
 * `:identity` is served pre-auth (§C.2), so the whole `identity` field namespace must
 * resolve above the READ gate: anything below it would answer a denied caller
 * PERMISSION_DENIED, contradicting §C.4's "returns `ERROR{tr::schema::not_found}`", which
 * carries no caller qualifier. The discriminating case is therefore a DENIED caller —
 * a granted one took the not_found path even before the namespace was resolved as a whole.
 *
 * The narrower refusal discloses nothing: `:identity` itself is world-readable by design,
 * so there is no secret for a not_found-vs-denied distinction to protect.
 */
void test_record_has_no_sub_addressing() {
    std::printf("RFC-0011 §C.4: served whole — no member or indexed addressing:\n");
    graph_t g;
    g.set_subject_resolver(caller_is_subject);
    check(g.set_identity(0x01, demo_key()).has_value(), "identity installed");
    (void)g.register_vertex(path_t("/dev"), role_t::STORED_VALUE);

    // Close /dev to everyone but alice — mallory is the caller the ACL denies.
    const std::vector<ace_t> grant{
        ace_t{.type = tr::graph::ace_type_t::ALLOW,
              .flags = 0,
              .subject = as_bytes("alice"),
              .access_mask = static_cast<std::uint32_t>(acl_right_t::READ),
              .expires_ns = 0}};
    check(g.write(path_t("/dev:acl"), make_value(tr::graph::encode_acl(grant))).has_value(),
          "an ACL granting only alice is installed on /dev");

    // The bare field still serves, to both — the §C.2 exemption is untouched.
    check(read_identity_bytes(g, "/dev:identity", "mallory").size() == 60,
          "the bare :identity still serves pre-auth (§C.2 unchanged)");

    for (const char* caller : {"mallory", "alice"}) {
        const auto member = read_as(g, "/dev:identity.key", caller);
        check(!member.has_value() && member.error() == status_t::SCHEMA_NOT_FOUND,
              ":identity.key is SCHEMA_NOT_FOUND — no member addressing, either caller");
        const auto indexed = read_as(g, "/dev:identity[0]", caller);
        check(!indexed.has_value() && indexed.error() == status_t::SCHEMA_NOT_FOUND,
              ":identity[0] is SCHEMA_NOT_FOUND — the record has no indexed surface");
    }
}

}  // namespace

int main() {
    test_record_is_byte_exact();
    test_every_vertex_answers_identically();
    test_read_is_pre_auth();
    test_keyless_node_is_enotty();
    test_registry_validation();
    test_no_write_surface();
    test_record_has_no_sub_addressing();

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES", g_failures,
                g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
