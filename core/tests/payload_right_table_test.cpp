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
 *
 * The same registration carries the endpoint's other control declaration, its RFC-0014
 * Amendment 3 `conn:schema` catalog (#1815): a module with none answers the empty `SETTINGS`;
 * a module that declares one has it served inside that `SETTINGS`, byte-exact, and refuses a
 * `SPEC` whose config does not conform — each with its ablation on a catalog-less module.
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
    {
        auto hooks = g.hooks();
        hooks.subject_resolver = {caller_is_subject, nullptr};
        g.set_hooks(hooks);
    }

    // The handler counts what reached it, so a check can tell "the gate refused" from
    // "the gate passed and the handler declined".
    int executed = 0;
    tr::graph::handlers_t handlers;
    auto handlers_on_write = [&executed](
                                 const tr::graph::value_t&,
                                 const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        ++executed;
        return {};
    };
    handlers.on_write = tr::graph::thunk(handlers_on_write);
    const payload_right_t rights[] = {
        payload_right_t{type_t::SPEC, acl_right_t::CREATE},
        payload_right_t{type_t::NAME, acl_right_t::WRITE},
    };
    const vertex_handle_t v =
        g.register_vertex(path_t("/ctl"), role_t::HANDLER, handlers, {}, rights);
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
    {
        auto hooks = g.hooks();
        hooks.subject_resolver = {caller_is_subject, nullptr};
        g.set_hooks(hooks);
    }
    tr::graph::handlers_t handlers;
    auto handlers_on_write2 = [](const tr::graph::value_t&,
                                 const tr::graph::write_ctx_t&) -> tr::graph::result_t<void> {
        return {};
    };
    handlers.on_write = tr::graph::thunk(handlers_on_write2);
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
    {
        auto hooks = node.hooks();
        hooks.subject_resolver = {caller_is_subject, nullptr};
        node.set_hooks(hooks);
    }
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

/** @brief How many times the catalog tests' factory ran — a refusal must leave it unmoved. */
int g_catalog_factory_runs = 0;

/** @brief A `label` record member, as a module's verbatim descriptor bytes. */
constexpr std::byte kTlsLabel[] = {
    std::byte{0x02}, std::byte{0x00}, std::byte{0x05}, std::byte{0x00},  // NAME, len 5
    std::byte{'l'},  std::byte{'a'},  std::byte{'b'},  std::byte{'e'},  std::byte{'l'},
    std::byte{0x02}, std::byte{0x00}, std::byte{0x03}, std::byte{0x00},  // NAME, len 3
    std::byte{'T'},  std::byte{'L'},  std::byte{'S'},
};

/** @brief A module's static catalog: two universal keys and one kind-private key. */
constexpr tr::net::conn_key_t kCatalog[] = {
    {.name = "addr", .dtype = tr::net::conn_dtype_t::UTF8, .required = true},
    {.name = "port", .dtype = tr::net::conn_dtype_t::U16, .required = true},
    {.name = "tls", .dtype = tr::net::conn_dtype_t::UTF8, .descriptor = kTlsLabel},
};

/** @brief A second, different catalog — for the one-catalog-per-endpoint refusal. */
constexpr tr::net::conn_key_t kOtherCatalog[] = {
    {.name = "addr", .dtype = tr::net::conn_dtype_t::UTF8},
};

/** @brief Register the counting `cat` kind and declare `cat-client` with @p catalog. */
tr::graph::result_t<void> declare_catalog_module(transport_vertex_t& net,
                                                 tr::net::conn_catalog_t catalog) {
    net.register_transport_type(
        "cat",
        [](const tr::net::conn_settings_t&,
           const tr::wire::tlv_t*) -> tr::graph::result_t<std::unique_ptr<tr::net::transport_t>> {
            ++g_catalog_factory_runs;
            return std::make_unique<dead_sock_t>();
        },
        tr::net::transport_kind_traits_t{.self_heal_dial = false, .delivers_ropes = false});
    return net.register_module("cat-client", "cat", conn_role_t::DIAL, catalog);
}

/** @brief The flattened bytes of a successful read, or empty. */
std::vector<std::byte> read_bytes(graph_t& node, const char* path) {
    const auto r = node.read(path_t(path));
    if (!r) return {};
    const tr::view::view_t flat = (**r).flatten();
    const std::span<const std::byte> b = flat.bytes();
    return {b.begin(), b.end()};
}

/** @brief Whether a write was refused with exactly @p want. */
bool refused(const tr::graph::result_t<void>& r, status_t want) {
    return !r.has_value() && r.error() == want;
}

/**
 * @brief #1815: a declared catalog is served inside the Amendment 3 envelope, byte-exact, as
 *        one RFC-0013 §B per-key record per key.
 */
void test_declared_catalog_is_served() {
    std::printf("\nAmendment 3: a declared catalog is the content of the SETTINGS:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    check(declare_catalog_module(net, kCatalog).has_value(), "the module declares its catalog");

    // Built independently of the encoder under test, from the RFC-0013 §B record shape.
    const auto record = [](std::vector<std::byte>& out, std::string_view key,
                           std::string_view dtype, bool required,
                           std::span<const std::byte> extra) {
        std::vector<std::byte> body;
        tr::wire::emit_name(body, "dtype");
        tr::wire::emit_name(body, dtype);
        if (required) {
            tr::wire::emit_name(body, "required");
            const std::byte one{1};
            tr::wire::emit_tlv(body, type_t::VALUE, opt_t{}, std::span<const std::byte>(&one, 1));
        }
        body.insert(body.end(), extra.begin(), extra.end());
        tr::wire::emit_name(out, key);
        tr::wire::emit_tlv(out, type_t::SETTINGS, opt_t{.pl = true}, body);
    };
    std::vector<std::byte> catalog;
    record(catalog, "addr", "utf8", true, {});
    record(catalog, "port", "u16", true, {});
    record(catalog, "tls", "utf8", false, kTlsLabel);
    std::vector<std::byte> point_body;
    tr::wire::emit_name(point_body, "conn");
    tr::wire::emit_tlv(point_body, type_t::SETTINGS, opt_t{.pl = true}, catalog);
    std::vector<std::byte> expected;
    tr::wire::emit_tlv(expected, type_t::POINT, opt_t{.pl = true}, point_body);

    check(read_bytes(node, "/net/cat-client/conn:schema") == expected,
          "read conn:schema = POINT{NAME \"conn\", SETTINGS{addr, port, tls records}}");
}

/**
 * @brief #1815: a `SPEC` whose config fails the declared catalog is refused `TYPE_MISMATCH`
 *        at the write, before any factory runs; a conforming one creates.
 */
void test_declared_catalog_validates_the_spec() {
    std::printf("\nRFC-0014 §2: a SPEC is validated against the declared catalog:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    (void)declare_catalog_module(net, kCatalog);
    const auto endpoint = node.find(path_t::parse("/net/cat-client/conn")->key());
    check(endpoint.has_value(), "the endpoint exists");
    if (!endpoint) return;
    g_catalog_factory_runs = 0;

    const auto write = [&](tr::net::conn_spec_t& spec) {
        return node.write(*endpoint, spec.view());
    };

    {
        tr::net::conn_spec_t spec("missing");
        spec.kind("cat").addr("203.0.113.1");
        check(refused(write(spec), status_t::TYPE_MISMATCH),
              "a required key absent (port) => TYPE_MISMATCH");
    }
    {
        tr::net::conn_spec_t spec("wide");
        spec.kind("cat").addr("203.0.113.1").u32("port", 9);
        check(refused(write(spec), status_t::TYPE_MISMATCH),
              "a catalogued key in the wrong width (u32 port) => TYPE_MISMATCH");
    }
    {
        tr::net::conn_spec_t spec("private");
        spec.kind("cat").addr("203.0.113.1").port(9).u8("tls", 1);
        check(refused(write(spec), status_t::TYPE_MISMATCH),
              "a kind-private catalogued key in the wrong type (VALUE tls) => TYPE_MISMATCH");
    }
    check(g_catalog_factory_runs == 0, "no refused SPEC reached the factory");
    check(!node.find(path_t::parse("/net/cat-client/missing")->key()).has_value() &&
              !node.find(path_t::parse("/net/cat-client/wide")->key()).has_value() &&
              !node.find(path_t::parse("/net/cat-client/private")->key()).has_value(),
          "and none of them mounted a connection");

    {
        tr::net::conn_spec_t spec("ok");
        spec.kind("cat").addr("203.0.113.1").port(9).text("tls", "lab").u32("future", 7);
        check(write(spec).has_value(),
              "a conforming SPEC creates — an uncatalogued key (future) stays ignored");
    }
    check(g_catalog_factory_runs == 1 &&
              node.find(path_t::parse("/net/cat-client/ok")->key()).has_value(),
          "and /net/cat-client/ok was built by the factory");
}

/**
 * @brief The ablation: the same malformed configs on a catalog-less module are NOT refused —
 *        undeclared means unvalidated, exactly as before #1815.
 */
void test_undeclared_catalog_validates_nothing() {
    std::printf("\nablation: a catalog-less module validates nothing:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    (void)declare_catalog_module(net, {});
    const auto endpoint = node.find(path_t::parse("/net/cat-client/conn")->key());
    if (!endpoint) return;
    tr::net::conn_spec_t spec("wide");
    spec.kind("cat").addr("203.0.113.1").u32("port", 9).u8("tls", 1);
    check(node.write(*endpoint, spec.view()).has_value(),
          "the u32 port and VALUE tls create anyway (the walk reads them as absent)");

    const std::vector<std::byte> b = read_bytes(node, "/net/cat-client/conn:schema");
    const auto decoded = tr::wire::decode(b);
    check(decoded.has_value() && decoded->children.size() == 2 &&
              decoded->children[1].type == type_t::SETTINGS && decoded->children[1].payload.empty(),
          "and its conn:schema is still the empty SETTINGS");
}

/**
 * @brief One catalog per endpoint: re-declaring the module with the same table or none is
 *        idempotent; a different table is refused and changes nothing.
 */
void test_catalog_is_fixed_per_endpoint() {
    std::printf("\nthe catalog is the endpoint's, fixed when it is minted:\n");
    graph_t node;
    fwd_router_t router(node);
    transport_vertex_t net(node, router);
    (void)declare_catalog_module(net, kCatalog);
    const std::vector<std::byte> before = read_bytes(node, "/net/cat-client/conn:schema");
    check(net.register_module("cat-client", "cat", conn_role_t::DIAL, kCatalog).has_value(),
          "the same triple and the same table again is idempotent");
    check(net.register_module("cat-client", "cat2", conn_role_t::DIAL).has_value(),
          "a second kind naming no catalog makes no claim");
    check(refused(net.register_module("cat-client", "cat3", conn_role_t::DIAL, kOtherCatalog),
                  status_t::PATH_IN_USE),
          "a different table under the same module => PATH_IN_USE");
    check(!net.module_for("cat3", conn_role_t::DIAL).has_value(),
          "and the refused declaration was not recorded");
    check(read_bytes(node, "/net/cat-client/conn:schema") == before,
          "the served catalog is unchanged");
}

}  // namespace

int main() {
    test_declared_table_selects_the_right();
    test_undeclared_handler_is_unchanged();
    test_creator_endpoint_splits_create_from_remove();
    test_catalog_envelope_is_an_empty_settings();
    test_declared_catalog_is_served();
    test_declared_catalog_validates_the_spec();
    test_undeclared_catalog_validates_nothing();
    test_catalog_is_fixed_per_endpoint();
    return tr::testing::summary("payload_right_table");
}
