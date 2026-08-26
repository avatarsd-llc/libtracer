/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — a transport KIND is a run-time NAME resolved through two registries
 *        the application fills, so a kind the core has never heard of mounts on exactly the
 *        same terms as a built-in one, and a kind nobody registered is REFUSED rather than
 *        defaulted to something plausible.
 *
 * Nothing in the routing plane knows the word `tcp`. A connection is created by writing a
 * SPEC to a module's creator endpoint `/net/<module>/conn`, and the `kind = <name>` that
 * SPEC carries is looked up twice:
 *
 *  1. the FACTORY catalog — `register_transport_type(kind, factory)` — which decides what
 *     object gets constructed. `udp`, `tcp` and `ws` are pre-registered by the default
 *     `transport_vertex_t` constructor only because they are compiled into the core;
 *     `can`, `quic` and `webtransport` ship their own factories (`can_transport_factory()`,
 *     `quic_transport_factory()`) and are registered by the application, through this
 *     identical call. An embedder's own kind is a third case of the same one.
 *  2. the MODULE declaration — `register_module(module, kind, role)` — which decides WHERE
 *     the connection mounts, `/net/<module>/<name>`. The library declares NONE of these,
 *     not even for its built-ins (ADR-0073 §4): `kTcpClientSuggestedModule` is a suggestion
 *     a header offers, never a registration a constructor performs.
 *
 * The module declaration is also why the SPEC carries no `type` and no `role`. The retired
 * `/net:children[]` door needed both, because a flat write had nothing else to say what was
 * being built or which way it faced; the endpoint door puts that in the PATH — a module is
 * declared for exactly one `(kind, role)`, so writing to `/net/tcp-client/conn` already
 * means "a tcp DIAL". Two data fields the creator could contradict became one addressed
 * door it cannot (RFC-0014 S7).
 *
 * Both halves are open and both are strict, which is the actual claim: this example
 * registers a kind that exists nowhere in the library, creates a connection of it with an
 * ordinary SPEC written to that kind's module endpoint, and watches the routing plane wire
 * it up — and then asks for a kind nobody registered and gets `SCHEMA_NOT_FOUND` instead of
 * a default.
 *
 * `quic` and `webtransport` are the shipped instances of the out-of-tree case: they live in
 * a separate `libtracer_quic` target that needs msquic, and nothing in the core references
 * them (ADR-0043). What they use to join a node is this page and nothing more.
 *
 * Needs the net plane (`LIBTRACER_NET_PLANE`, on by default) for `transport_vertex_t`; no
 * sockets are opened, because the kind registered here is an in-process one. Runs under
 * ctest as `example_net_kind_catalog`; returns non-zero on any failed check.
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "libtracer/conn_spec.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_vertex.hpp"

namespace {

using tr::graph::graph_t;
using tr::graph::path_t;
using tr::net::conn_role_t;

/** @brief Report expectation @p what and record a failure on @p ok. */
void check(bool& ok, bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "ok" : "FAIL", what);
    ok = ok && cond;
}

/**
 * @brief The whole of a new transport kind: a `transport_t` that keeps what it is given.
 *
 * A real kind opens something. This one does not, which is the cleanest way to show that
 * the catalog cares about nothing except that a factory answers with a `transport_t`.
 */
class demo_link_t final : public tr::net::transport_t {
   public:
    /** @brief How many frames this link was asked to emit. */
    std::size_t sent = 0;
    void send(std::span<const std::byte> frame) override {
        (void)frame;
        ++sent;
    }
};

}  // namespace

int main() {
    bool ok = true;

    graph_t g;
    tr::net::fwd_router_t router(g);
    // The default constructor registers the factories for the kinds this BUILD compiled —
    // udp/tcp/ws. It registers no module names at all, for any of them.
    tr::net::transport_vertex_t net(g, router);

    // 1. A compiled-in kind is still unusable until the application says where it mounts.
    // This refusal is the declared-only rule: the catalog is the app's, not the library's.
    std::printf("the library declares no modules, not even for its own kinds:\n");
    const auto before = net.module_for("tcp", conn_role_t::DIAL);
    check(ok, !before.has_value(), "module_for('tcp', DIAL) is refused before the app declares it");
    check(ok, !before.has_value() && before.error() == tr::graph::status_t::SCHEMA_NOT_FOUND,
          "…as SCHEMA_NOT_FOUND — an absent catalog entry, not a malformed request");

    const auto declared = net.register_module(std::string(tr::net::kTcpClientSuggestedModule),
                                              "tcp", conn_role_t::DIAL);
    check(ok, declared.has_value(),
          "the application declares the module, adopting the header's "
          "suggested name");
    const auto after = net.module_for("tcp", conn_role_t::DIAL);
    check(ok, after.has_value() && *after == "tcp-client", "…and now the kind resolves to it");

    // 2. A kind the library has never heard of. Two calls — the same two calls `quic` makes.
    std::printf("a kind the core does not contain:\n");
    net.register_transport_type(
        "demo",
        [](const tr::net::conn_settings_t& settings, const tr::wire::tlv_t* raw_config)
            -> tr::graph::result_t<std::unique_ptr<tr::net::transport_t>> {
            // A real factory parses its kind-PRIVATE keys out of `raw_config` here (quic's
            // cert/key PEM paths are the shipped example); the universal keys are already
            // parsed into `settings`. This one needs neither, and says so.
            (void)settings;
            (void)raw_config;
            return std::unique_ptr<tr::net::transport_t>(std::make_unique<demo_link_t>());
        });
    check(ok, net.register_module("demo-client", "demo", conn_role_t::DIAL).has_value(),
          "register_module accepts a kind that exists only in this file");

    // 3. Create one, the ordinary way: a SPEC written to the module's creator endpoint.
    // Nothing in this write is kind-specific except the four letters of the name — and no
    // `type`/`role` pair, because `/net/demo-client/conn` already says both.
    const auto created =
        g.write(path_t("/net/demo-client/conn"), tr::net::conn_spec("one", /*port=*/0, "demo"));
    check(ok, created.has_value(), "SPEC{ name=one, kind=demo } created a connection");
    check(ok, router.registry().by_name("net/demo-client/one") != nullptr,
          "…and the routing plane wired it in under /net/<module>/<name>");
    check(ok, g.read(path_t("/net/demo-client/one")).has_value(),
          "…with a connection vertex addressable in the graph");

    // 4. And a kind nobody registered. The refusal is the point: an unresolvable kind must
    // not fall back to a default transport, because "some link came up" is indistinguishable
    // from the right one until traffic silently goes nowhere.
    // Declaring a module for it is allowed and does not help: `register_module` records
    // where a kind WOULD mount, it does not promise anybody can build one. The factory
    // catalog is the half that constructs, and it is consulted at the write.
    std::printf("a kind nobody registered:\n");
    check(ok, net.register_module("nosuch-client", "nosuch", conn_role_t::DIAL).has_value(),
          "a module may be declared for a kind no factory answers for");
    const auto refused =
        g.write(path_t("/net/nosuch-client/conn"), tr::net::conn_spec("two", /*port=*/0, "nosuch"));
    check(ok, !refused.has_value(), "SPEC{ kind=nosuch } was refused");
    check(ok, !refused.has_value() && refused.error() == tr::graph::status_t::SCHEMA_NOT_FOUND,
          "…as SCHEMA_NOT_FOUND — the same verdict a missing module gives");
    check(ok, router.registry().by_name("net/nosuch-client/two") == nullptr,
          "…and nothing was registered on the way to refusing");

    std::printf("catalog: 1 kind declared by the app, 1 kind invented here, 1 refused\n");
    return ok ? 0 : 1;
}
