/**
 * @file
 * @brief RFC-0014 Amendment 2 (§5.1) — a handler vertex declares a payload-type → required-
 *        ACL-right table, and the ONE write gate demands what it says (#492 S2c).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two halves, each with its ablation:
 *
 *  - the GENERAL contract on a plain handler vertex — a declared type takes the declared
 *    right, an undeclared type falls back to `WRITE`, and a handler that declares NOTHING
 *    gates everything on `WRITE` exactly as before (the positive control: remove the
 *    declaration and every check below flips);
 *  - the FIRST USER — the transport creator endpoint declaring §5's own mapping
 *    (`SPEC`⇒`CREATE`, `NAME`⇒`WRITE`), so a `CREATE`-only peer creates but cannot remove
 *    and a `WRITE`-only peer removes but cannot create.
 *
 * Every refusal is also counted: the gate did not move, so the single-sited
 * `delivery_drops_t::denied` is what tallies a right-table denial too.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "libtracer/conn_spec.hpp"
#include "libtracer/security_acl.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "test_support.hpp"
#include "test_values.hpp"

namespace {

using tr::graph::acl_right_t;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::payload_right_t;
using tr::graph::role_t;
using tr::graph::status_t;
using tr::graph::subject_token_t;
using tr::graph::vertex_handle_t;
using tr::net::conn_role_t;
using tr::net::fwd_router_t;
using tr::net::transport_vertex_t;
using tr::wire::opt_t;
using tr::wire::type_t;

using tr::testing::check;
using tr::testing::make_value;

/** @brief `caller` bytes as a subject token — the ADR-0018 test resolver. */
std::vector<std::byte> as_bytes(std::string_view s) {
    std::vector<std::byte> out(s.size());
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/** @brief The test resolver: the caller context IS the subject (acl_test's shape). */
std::expected<subject_token_t, tr::wire::err_t> caller_is_subject(void*, std::string_view caller) {
    return as_bytes(caller);
}

/** @brief One ALLOW ACE per (subject, mask) pair, encoded as a whole `:acl` payload. */
std::vector<std::byte> allow(std::initializer_list<std::pair<std::string_view, acl_right_t>> g) {
    std::vector<tr::graph::ace_t> aces;
    aces.reserve(g.size());
    for (const auto& [subject, right] : g)
        aces.push_back(tr::graph::ace_t{.type = tr::graph::ace_type_t::ALLOW,
                                        .flags = 0,
                                        .subject = as_bytes(subject),
                                        .access_mask = static_cast<std::uint32_t>(right),
                                        .expires_ns = 0});
    return tr::graph::encode_acl(aces);
}

/** @brief A `SPEC{}` payload with an empty body — a declared TYPE and nothing else. */
tr::view::view_t spec_payload() {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, std::span<const std::byte>{});
    return make_value(out);
}

/** @brief A bare `NAME{x}` payload — the other declared type. */
tr::view::view_t name_payload() {
    std::vector<std::byte> out;
    tr::wire::emit_name(out, "x");
    return make_value(out);
}

/** @brief A `VALUE{0x01}` payload — a type NO table below declares. */
tr::view::view_t value_payload() {
    std::vector<std::byte> out;
    const std::byte one[1] = {std::byte{0x01}};
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, one);
    return make_value(out);
}

/** @brief True iff @p r is the gate's refusal. */
bool denied(const tr::graph::result_t<void>& r) {
    return !r.has_value() && r.error() == status_t::PERMISSION_DENIED;
}

/**
 * @brief The general contract: a declared type takes the declared right; an undeclared one
 *        falls back to `WRITE`.
 */
void test_declared_table_selects_the_right() {
    std::printf("Amendment 2: a handler's declared payload type takes its declared right:\n");
    graph_t g;
    g.configure_subject_resolver(caller_is_subject, nullptr);

    // The handler counts what reached it, so a check can tell "the gate refused" from
    // "the gate passed and the handler declined".
    int executed = 0;
    tr::graph::handlers_t handlers;
    handlers.on_write = [&executed](const tr::view::rope_t&,
                                    const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        ++executed;
        return {};
    };
    handlers.payload_rights = {
        payload_right_t{type_t::SPEC, acl_right_t::CREATE},
        payload_right_t{type_t::NAME, acl_right_t::WRITE},
    };
    const vertex_handle_t v = g.register_vertex(path_t("/ctl"), role_t::HANDLER, handlers);
    check(g.write(path_t("/ctl:acl"), make_value(allow({{"peer-c", acl_right_t::CREATE},
                                                        {"peer-w", acl_right_t::WRITE}})))
              .has_value(),
          "the endpoint authorizes peer-c to CREATE and peer-w to WRITE");

    // SPEC is declared CREATE: the CREATE-only peer passes, the WRITE-only peer does not.
    check(g.write(v, spec_payload(), "peer-c").has_value(), "SPEC accepted from the CREATE peer");
    check(executed == 1, "and it reached the handler");
    check(denied(g.write(v, spec_payload(), "peer-w")), "SPEC REFUSED from the WRITE-only peer");
    check(executed == 1, "the refused SPEC never reached the handler");

    // NAME is declared WRITE: the mirror image.
    check(g.write(v, name_payload(), "peer-w").has_value(), "NAME accepted from the WRITE peer");
    check(executed == 2, "and it reached the handler");
    check(denied(g.write(v, name_payload(), "peer-c")), "NAME REFUSED from the CREATE-only peer");

    // An UNDECLARED type is not ungated — it falls back to WRITE.
    check(g.write(v, value_payload(), "peer-w").has_value(),
          "an undeclared VALUE takes the WRITE fallback");
    check(denied(g.write(v, value_payload(), "peer-c")),
          "and the CREATE-only peer cannot write it — undeclared means WRITE, not open");

    // The gate did not move, so its counter still counts.
    check(g.delivery_drops().denied == 3, "every refusal counted into the one denied counter");
}

/**
 * @brief The ablation: the SAME vertex without the declaration gates everything on `WRITE`.
 */
void test_undeclared_handler_is_unchanged() {
    std::printf("\nablation — a handler that declares no table gates every type on WRITE:\n");
    graph_t g;
    g.configure_subject_resolver(caller_is_subject, nullptr);
    tr::graph::handlers_t handlers;
    handlers.on_write = [](const tr::view::rope_t&,
                           const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        return {};
    };
    const vertex_handle_t v = g.register_vertex(path_t("/ctl"), role_t::HANDLER, handlers);
    check(g.write(path_t("/ctl:acl"), make_value(allow({{"peer-c", acl_right_t::CREATE},
                                                        {"peer-w", acl_right_t::WRITE}})))
              .has_value(),
          "same ACL as above");

    check(g.write(v, spec_payload(), "peer-w").has_value(),
          "SPEC passes on WRITE alone — the previous check's refusal was the TABLE's doing");
    check(denied(g.write(v, spec_payload(), "peer-c")),
          "and CREATE alone no longer suffices for SPEC");
}

/** @brief A socket that does nothing — creation under test, never traffic. */
struct dead_sock_t final : tr::net::transport_t {
    void send(std::span<const std::byte>) override {}
};

/** @brief Register a one-kind DIAL module whose factory always succeeds. */
void declare_fake_module(transport_vertex_t& net) {
    net.register_transport_type(
        "fake",
        [](const tr::net::conn_settings_t&,
           const tr::wire::tlv_t*) -> tr::graph::result_t<std::unique_ptr<tr::net::transport_t>> {
            return std::make_unique<dead_sock_t>();
        },
        tr::net::transport_kind_traits_t{.self_heal_dial = false, .delivers_ropes = false});
    (void)net.register_module("fake-client", "fake", conn_role_t::DIAL);
}

/** @brief The creating SPEC, endpoint-door spelling. */
tr::view::view_t fake_spec(std::string_view name) {
    tr::net::conn_spec_t spec(name);
    spec.kind("fake").addr("203.0.113.1").port(9);
    return spec.view();
}

/**
 * @brief §5 discharged: the creator endpoint's own declaration splits create from remove.
 */
void test_creator_endpoint_splits_create_from_remove() {
    std::printf("\n§5: the creator endpoint declares SPEC=>CREATE, NAME=>WRITE:\n");
    graph_t node;
    node.configure_subject_resolver(caller_is_subject, nullptr);
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_module(net);

    const auto ep = node.find(path_t::parse("/net/fake-client/conn")->key());
    check(ep.has_value(), "the creator endpoint exists");
    if (!ep) return;
    const vertex_handle_t endpoint = *ep;
    check(node.write(path_t("/net/fake-client/conn:acl"),
                     make_value(
                         allow({{"peer-c", acl_right_t::CREATE}, {"peer-w", acl_right_t::WRITE}})))
              .has_value(),
          "the endpoint authorizes peer-c to CREATE and peer-w to WRITE");

    // CREATE-only peer: creates, cannot remove.
    check(node.write(endpoint, fake_spec("a"), "peer-c").has_value(),
          "the CREATE-only peer creates the connection via SPEC");
    check(node.find(path_t::parse("/net/fake-client/a")->key()).has_value(),
          "and /net/fake-client/a exists");
    check(denied(node.write(endpoint, tr::net::conn_remove("a"), "peer-c")),
          "the CREATE-only peer cannot REMOVE it — NAME demands WRITE");
    check(node.find(path_t::parse("/net/fake-client/a")->key()).has_value(),
          "so the connection is still there");

    // WRITE-only peer: removes, cannot create.
    check(denied(node.write(endpoint, fake_spec("b"), "peer-w")),
          "the WRITE-only peer cannot CREATE — SPEC demands CREATE");
    check(!node.find(path_t::parse("/net/fake-client/b")->key()).has_value(),
          "and no /net/fake-client/b was minted");
    check(node.write(endpoint, tr::net::conn_remove("a"), "peer-w").has_value(),
          "the WRITE-only peer removes via NAME");
    check(!node.find(path_t::parse("/net/fake-client/a")->key()).has_value(),
          "and the connection is gone");

    check(node.delivery_drops().denied == 2, "both refusals counted at the one gate");
}

/**
 * @brief Amendment 3: the endpoint's catalog read answers the POINT envelope with an EMPTY
 *        `SETTINGS`, never `SCHEMA_NOT_FOUND`.
 */
void test_catalog_envelope_is_an_empty_settings() {
    std::printf("\nAmendment 3: a catalog-less module answers POINT{NAME, SETTINGS{}}:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    declare_fake_module(net);

    check(node.find(path_t::parse("/net/fake-client/conn")->key()).has_value(),
          "the endpoint resolves — §6's probe target");
    const auto r = node.read(path_t("/net/fake-client/conn:schema"));
    check(r.has_value(), "the catalog read SUCCEEDS (never SCHEMA_NOT_FOUND)");
    if (!r) return;

    // POINT{ NAME "conn", SETTINGS{} } — the Amendment 3 envelope. Checked structurally:
    // the envelope is the claim; its exact framing is read_schema's own banked shape.
    const tr::view::view_t flat = (**r).flatten();
    const std::span<const std::byte> b = flat.bytes();
    const std::vector<std::byte> owned(b.begin(), b.end());
    const auto decoded = tr::wire::decode(owned);
    check(decoded.has_value() && decoded->type == type_t::POINT, "the reply is a POINT");
    if (!decoded) return;
    check(decoded->children.size() == 2, "with exactly two members");
    if (decoded->children.size() != 2) return;
    check(decoded->children[0].type == type_t::NAME &&
              std::string_view(reinterpret_cast<const char*>(decoded->children[0].payload.data()),
                               decoded->children[0].payload.size()) == "conn",
          "the first is NAME \"conn\" — the endpoint's own name");
    check(decoded->children[1].type == type_t::SETTINGS && decoded->children[1].children.empty() &&
              decoded->children[1].payload.empty(),
          "the second is an EMPTY SETTINGS — the conforming degenerate catalog");
}

}  // namespace

int main() {
    test_declared_table_selects_the_right();
    test_undeclared_handler_is_unchanged();
    test_creator_endpoint_splits_create_from_remove();
    test_catalog_envelope_is_an_empty_settings();
    std::printf("\nOK\n");
    return 0;
}
