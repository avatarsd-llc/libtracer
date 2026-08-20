/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_vertex.hpp"

#include <cstring>
#include <memory>
#include <span>
#include <utility>

#include "libtracer/builtin_transports.hpp"
#include "libtracer/byteorder.hpp"
#include "libtracer/config_reader.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/packed_path.hpp"
#include "libtracer/path.hpp"
#include "libtracer/self_heal_link.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

using graph::path_t;
using graph::result_t;
using graph::status_t;
using graph::vertex_handle_t;
using view::view_t;
using wire::tlv_t;
using wire::type_t;

namespace {

/**
 * @brief The last NAME segment of a canonical PATH-payload key = the connection's NAME.
 *
 * The key is `<...prior NAMEs...><NAME len-prefixed name>`. The record walk is
 * `wire::key_view_t::last_segment` — key_view is the single locus for canonical-key
 * navigation, and this TU hand-rolled a second copy of it (#932). The result is
 * still materialised as an owning `std::string` because the caller needs one.
 */
[[nodiscard]] std::string last_segment(std::span<const std::byte> key) {
    return std::string(detail::as_string_view(wire::key_view_t{key}.last_segment()));
}

/**
 * @brief Parse the optional SPEC `config` SETTINGS (the shared config_reader_t walk): NAME "addr"
 *        NAME <utf8>, NAME "kind" NAME <utf8>, NAME "port" VALUE u16, NAME "role" VALUE u8
 *        (overrides the type default), NAME "keepalive" VALUE u32.
 *
 * ONLY the universal
 * keys land here (ADR-0043 §5 leanness): kind-private pairs (e.g. quic's `cert`/`key`)
 * are the kind's factory's business — it parses them from the raw config TLV it
 * receives. Unknown pairs are ignored (forward-compat).
 */
void parse_config(const tlv_t* config, conn_settings_t& s) {
    const config_reader_t cfg(config);
    if (const auto v = cfg.name("addr")) s.addr = std::string(*v);
    if (const auto v = cfg.name("kind")) s.kind = std::string(*v);
    if (const auto v = cfg.u16("port")) {
        s.port = *v;
        s.port_set = true;
    }
    if (const auto v = cfg.u8("role")) s.role = *v == 0 ? conn_role_t::DIAL : conn_role_t::LISTEN;
    if (const auto v = cfg.u32("keepalive")) s.keepalive_ms = *v;
    if (const auto v = cfg.u32("max_frame")) s.max_frame = *v;
    if (const auto v = cfg.u32("backoff")) s.backoff_ms = *v;
    if (const auto v = cfg.u32("connect_timeout")) s.connect_timeout_ms = *v;
}

/**
 * @brief Re-encode a decoded TLV to owned wire bytes — the S5 engine's copy of the SPEC's
 *        `config` SETTINGS, taken because the decoded tlv_t BORROWS the write's rope,
 *        which is gone by the first re-dial. Trailers are not re-emitted (a config
 *        SETTINGS never carries one; the reader would ignore it anyway).
 */
void reemit_tlv(std::vector<std::byte>& out, const tlv_t& tlv) {
    if (tlv.opt.pl) {
        std::vector<std::byte> body;
        for (const tlv_t& child : tlv.children) reemit_tlv(body, child);
        wire::emit_tlv(out, tlv.type, tlv.opt, body);
        return;
    }
    wire::emit_tlv(out, tlv.type, tlv.opt, tlv.payload);
}

/** @brief A 1-byte link-liveness VALUE TLV (link_state_t) as an owned view. */
[[nodiscard]] view_t link_state_value(link_state_t state) {
    std::vector<std::byte> out;
    const std::byte b{static_cast<std::uint8_t>(state)};
    wire::emit_tlv(out, type_t::VALUE, wire::opt_t{}, std::span<const std::byte>(&b, 1));
    return view::over_bytes(out).value_or(
        view_t{});  // empty view on alloc failure (caller-checked)
}

}  // namespace

// SLIM target ctor (@ref slim_net_t): member init + the graph-side catalog wiring,
// but NO built-in factory registration. This TU-locus deliberately does NOT name
// register_builtin_transports, so a consumer that only ever calls THIS ctor lets the
// linker garbage-collect the udp/tcp/ws factories (and the transport TUs nothing else
// references). The full ctor below delegates here and adds the builtins.
transport_vertex_t::transport_vertex_t(graph::graph_t& graph, fwd_router_t& router,
                                       std::string net_root, mem::mem_backend_t* rx_backend,
                                       slim_net_t, mem::block_source_t* egress_src)
    : graph_(graph),
      router_(router),
      net_root_(std::move(net_root)),
      rx_backend_(rx_backend),
      // The nullptr guard the FULL ctor used to hold, MOVED here (#873): both ctors reach
      // this one line, so a null argument still means the process heap and behaviour is
      // bit-identical for every existing caller. Before this parameter a SLIM node's
      // `egress_source()` answered the process heap unconditionally — it could not be told
      // otherwise, so the accessor lied about that node's store.
      egress_src_(egress_src != nullptr ? egress_src : &mem::heap_source()) {
    // Register the `/net` parent if it isn't already (it is the `:children[]` target).
    if (!graph_.find(path_t::parse(net_root_)->key())) {
        (void)graph_.register_vertex(*path_t::parse(net_root_), graph::role_t::STORED_VALUE);
    }
    // Register the two catalog types on the graph via the #82 seam. Both build a
    // connection leaf; `role` is the type default (a `:settings` `role` may override).
    graph_.register_child_type(
        "client", [this](graph::graph_t&, std::vector<std::byte> key, const tlv_t* config) {
            return make_connection(std::move(key), config, conn_role_t::DIAL);
        });
    graph_.register_child_type(
        "listener", [this](graph::graph_t&, std::vector<std::byte> key, const tlv_t* config) {
            return make_connection(std::move(key), config, conn_role_t::LISTEN);
        });
    // The `quic` kind is NOT a builtin: it lives in the separate libtracer_quic
    // module (ADR-0043), which extends this catalog through register_transport_type
    // (quic_transport_factory) — this file never learns about msquic (open/closed).
    // A slim node likewise registers whatever factories it wants after construction.
}

// FULL (default) ctor: the slim wiring PLUS the built-in transport-factory catalog
// entries (config `kind` selectors) this build compiled in — udp / tcp / ws, each
// from its own TU gated by a per-module CMake option (register_builtin_transports is
// the full-node or CMake-generated dispatcher; see builtin_transports.hpp). Keeping the
// concrete transport references out of this file lets a build DROP a transport without
// a dangling reference — module selection is by compiled TU, no preprocessor macros.
// Delegating to the slim ctor keeps the catalog wiring in one place; the extra call
// here is the ONLY reference to register_builtin_transports, so it (and the builtins)
// stay linked exactly when this ctor is reachable — @ref slim_net_t sheds them.
transport_vertex_t::transport_vertex_t(graph::graph_t& graph, fwd_router_t& router,
                                       std::string net_root, mem::mem_backend_t* rx_backend,
                                       mem::block_source_t* egress_src)
    : transport_vertex_t(graph, router, std::move(net_root), rx_backend, slim_net, egress_src) {
    // The ADR-0079 net-plane egress store (#873 family 1) is now set by the delegated ctor —
    // the nullptr guard lives there, so a SLIM node gets the same treatment this one does
    // and `egress_source()` answers for both. All that is left here is the builtin catalog.
    register_builtin_transports(*this, rx_backend_, egress_src_);
}

void transport_vertex_t::register_transport_type(std::string kind, transport_factory_t factory) {
    // Default traits: eager construction, exactly as every pre-S5 registration behaved.
    register_transport_type(std::move(kind), std::move(factory), transport_kind_traits_t{});
}

void transport_vertex_t::register_transport_type(std::string kind, transport_factory_t factory,
                                                 transport_kind_traits_t traits) {
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    transport_types_.insert_or_assign(std::move(kind), kind_entry_t{std::move(factory), traits});
}

result_t<void> transport_vertex_t::register_module(std::string module, std::string kind,
                                                   conn_role_t role) {
    // Registration is a minting boundary (ADR-0073 §1): the ONE shared segment-validity
    // predicate gates the name here, exactly as path_t::parse gates the local string tier.
    if (!graph::valid_segment(module)) return std::unexpected(status_t::INVALID_PATH);
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    // "Adding a module adds its creator endpoint and catalog" (RFC-0014 §1). Minted BEFORE
    // the declaration is recorded, so a refusal leaves nothing half-declared: a module whose
    // endpoint could not be registered would advertise a (kind, role) the wire has no door to.
    if (auto minted = mint_module_locked(module); !minted) return minted;
    for (module_decl_t& d : modules_) {
        if (d.kind == kind && d.role == role) {
            d.module = std::move(module);
            return {};
        }
    }
    modules_.push_back({std::move(module), std::move(kind), role});
    return {};
}

result_t<std::string> transport_vertex_t::module_for(std::string_view kind,
                                                     conn_role_t role) const {
    // The PUBLIC entry locks (#881); `make_connection` already holds `ctl_m_` — a plain,
    // NON-RECURSIVE std::mutex (ADR-0063 erratum 1) — so it calls the body directly. The
    // fix is a split precisely because a lock added in place would self-deadlock there.
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    return module_for_locked(kind, role);
}

/** @brief `module_for`'s body, for a caller that already holds `ctl_m_`. */
result_t<std::string> transport_vertex_t::module_for_locked(std::string_view kind,
                                                            conn_role_t role) const {
    for (const module_decl_t& d : modules_) {
        if (d.kind == kind && d.role == role) return d.module;
    }
    // Declared-only (ADR-0073 §4): no derived "<kind>-client"/"<kind>-server" fallback —
    // an undeclared (kind, role) is an unsupported catalog entry, the same convention as
    // an unknown SPEC `type`. The application mints every module segment.
    return std::unexpected(status_t::SCHEMA_NOT_FOUND);
}

/** @brief The declaration an endpoint write resolves its (kind, role) through. */
result_t<transport_vertex_t::module_decl_t> transport_vertex_t::declaration_for_locked(
    std::string_view module, std::string_view kind) const {
    const module_decl_t* found = nullptr;
    std::size_t hits = 0;
    for (const module_decl_t& d : modules_) {
        if (d.module != module) continue;
        // A SPEC that DID name a kind must name one this module constructs. Silently
        // creating the module's own kind instead would mount a connection the creator did
        // not ask for — the kind is data the creator supplied, so a mismatch is refused.
        if (!kind.empty() && d.kind != kind) continue;
        ++hits;
        found = &d;
    }
    // Not a (module, kind) this plane declares: the unsupported-catalog-entry convention,
    // the same answer an unknown SPEC `type` and an unregistered transport kind give.
    if (hits == 0) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
    // One module declared for two kinds, and a kind-less SPEC: genuinely ambiguous, so it is
    // refused rather than resolved by declaration order — the same ruling (and the same
    // status) the kind-less `:children[]` spelling gives two stagings sharing a leaf NAME.
    if (hits > 1) return std::unexpected(status_t::TYPE_MISMATCH);
    return *found;
}

result_t<void> transport_vertex_t::mint_module_locked(const std::string& module) {
    // The `<net_root>/<module>` grouping vertex. graph_.find IS the dedupe — the same rule
    // `make_connection_locked`'s lazy mint follows, and for the same reason: a separate
    // seen-set would be a second source of truth for something the graph already knows.
    std::vector<std::byte> mod_key;
    (void)wire::emit_path_segment(mod_key, std::string_view(net_root_).substr(1));
    (void)wire::emit_path_segment(mod_key, module);
    if (!graph_.find(mod_key)) {
        auto mod = graph_.register_vertex_key(mod_key, graph::role_t::STORED_VALUE, {});
        if (!mod) return std::unexpected(mod.error());
    }

    std::vector<std::byte> endpoint_key = mod_key;
    (void)wire::emit_path_segment(endpoint_key, kConnEndpointName);
    if (graph_.find(endpoint_key)) return {};  // a second kind under the same module

    // `role_t::HANDLER` is what makes the endpoint WRITE-ONLY AND VALUELESS (RFC-0014 §2):
    // the graph runs `on_write` and stores no last-known-value, so the write is EXECUTED,
    // never assigned. No `on_read` is installed either — a read answers NOT_FOUND until S3
    // teaches `:schema` to serve the module's config catalog, which is the endpoint's only
    // readable facet.
    //
    // The lambda captures the module by VALUE. The path is the module, so the dispatch never
    // re-derives it from a payload the peer wrote — a creator cannot address one module's
    // endpoint and have the connection mount under another.
    graph::handlers_t handlers;
    handlers.on_write = [this, module](const view::rope_t& value,
                                       const graph::write_ctx_t&) -> result_t<void> {
        return endpoint_write(module, value);
    };
    auto endpoint = graph_.register_vertex_key(std::move(endpoint_key), graph::role_t::HANDLER,
                                               std::move(handlers));
    if (!endpoint) return std::unexpected(endpoint.error());
    // RFC-0014 §3 (S4): `conn` is HIDDEN from `<net_root>/<module>:children[]`, which returns
    // the module's member CONNECTIONS. The endpoint is the control that creates them, not one
    // of them, so a peer walking the listing as a topology of links would descend into a
    // vertex with no peer behind it. It stays registered and addressable — §6 makes `read
    // <module>/conn:schema` the sanctioned creatability probe, so being unlisted is exactly
    // why it must still resolve. Hiding cannot fail on a vertex just registered, and the
    // status is discarded for that reason: a failure would mean the endpoint vanished between
    // two calls under `ctl_m_`, which the graph's insert-only discipline (ADR-0057) excludes.
    (void)graph_.hide_from_enumeration(*endpoint);
    return {};
}

result_t<void> transport_vertex_t::endpoint_write(const std::string& module,
                                                  const view::rope_t& value) {
    // A DEVICE-link payload is permanently un-parsable on the CPU (ADR-0024), so it is a
    // malformed control write rather than a transient one — the same classification
    // `graph_t::write`'s field surface makes.
    if (!value.all_host()) return std::unexpected(status_t::TYPE_MISMATCH);
    mem::mem_backend_t& backend = rx_backend_ != nullptr ? *rx_backend_ : mem::heap_backend();
    // Single-link (every ordinary control write) is zero-copy; a rope that straddled links
    // pays one flatten, and an exhausted backend surfaces as TRANSIENT backpressure rather
    // than being read back as a truncated — and therefore "malformed" — SPEC (#917).
    const auto flat = value.try_materialize(backend);
    if (!flat) return std::unexpected(status_t::BACKPRESSURE);
    const auto payload = wire::decode(*flat);
    if (!payload) return std::unexpected(status_t::TYPE_MISMATCH);

    // ONE critical section for the whole dispatch: parse, declaration lookup, socket
    // construction and routing all happen under `ctl_m_`, so two peers writing the same
    // endpoint cannot interleave into a half-built connection. Head of the declared lock
    // order (this -> fwd_router_t -> graph_t -> the vertex stripe), and the graph holds none
    // of its own locks across `on_write`, so nothing here descends against that order.
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    switch (payload->type) {
        case type_t::SPEC:
            return endpoint_create_locked(module, *payload);
        case type_t::NAME:
            return endpoint_remove_locked(module, detail::as_string_view(payload->payload));
        default:
            // Any other payload — a VALUE, an empty write, a structured TLV that is neither
            // control type. The endpoint NEVER falls through to an ordinary assign
            // (RFC-0014 §2); a creator that meant to write data wrote to the wrong vertex.
            return std::unexpected(status_t::TYPE_MISMATCH);
    }
}

result_t<void> transport_vertex_t::endpoint_create_locked(const std::string& module,
                                                          const tlv_t& spec) {
    // SPEC{ NAME "name" NAME <seg>, NAME "config" SETTINGS{ pairs }? } — no `type` and no
    // `role`: the module in the path already says both (RFC-0014 §1). Read through the ONE
    // pair-consuming walk, exactly as `graph_t::create_child` reads the `:children[]` SPEC.
    // The marker below exempts THIS reader from the connection-config page gate: it walks the
    // SPEC ENVELOPE (`name`, `config`), not a connection config, so its keys are no kind's and
    // belong on no row of docs/modules/connection-config.md. `graph_t::create_child` reads the
    // other door's envelope through the same type and is excluded there for the same reason.
    const wire::config_reader_t pairs(&spec);  // config-keys: not-connection-config
    const std::string_view name = pairs.name("name").value_or(std::string_view{});
    // `name` is REQUIRED and stays required (ADR-0073 §5, #622): an omitted name with a
    // node-assigned fallback would cost retry idempotency — a create retried over a link
    // that dropped its reply would append a SECOND connection instead of answering
    // PATH_IN_USE. The `p<slot>` fallback exists only for creator-LESS inbound peers, where
    // there is no retry because the peer dialed us.
    if (name.empty()) return std::unexpected(status_t::TYPE_MISMATCH);
    // The wire minting boundary runs THE shared segment predicate (ADR-0073 §1) — the same
    // one `graph_t::create_child` runs on the other door — so a name that enters the graph
    // here is expressible in the addressing grammar. Without it the endpoint could mint an
    // enumerable-but-unaddressable connection, which the `:children[]` door cannot.
    if (!graph::valid_segment(name)) return std::unexpected(status_t::INVALID_PATH);
    // The reserved name, refused BEFORE anything is built (RFC-0014 §3 create-side).
    // Falling through would answer PATH_IN_USE anyway — `register_vertex_key` collides with
    // the endpoint's own key — but only after the kind's factory had already dialed or bound
    // a socket, which is the side effect a refusal must not have.
    // §2 used to say `tr::schema::type_mismatch` here, contradicting §3; the RFC now says
    // `tr::path::in_use` in both clauses (#492 S7), leaving `type_mismatch` for the empty
    // name and the SPEC envelope's schema-shape violations. So this early return is a
    // side-effect guard only — it answers exactly what falling through would.
    if (name == kConnEndpointName) return std::unexpected(status_t::PATH_IN_USE);

    const tlv_t* config = pairs.settings("config");
    conn_settings_t settings;
    parse_config(config, settings);

    const auto declared = declaration_for_locked(module, settings.kind);
    if (!declared) return std::unexpected(declared.error());
    // The role is POSITIONAL — it IS the module (RFC-0014 §1/§3) — so the declaration's role
    // OVERWRITES whatever the config's `role` pair said. That pair is the superseded
    // `:children[]` spelling's override, and honouring it here would let a creator mount a
    // LISTEN socket under a module whose path promises DIAL.
    settings.role = declared->role;
    // A kind-less SPEC is the staged-link spelling; the module's declared kind is the kind
    // this endpoint constructs, so recording it keeps `settings_of` honest about what the
    // connection is. `make_connection_locked` still prefers a `provide_link` staging over
    // the factory, so filling this in does not change WHICH link is used.
    if (settings.kind.empty()) settings.kind = declared->kind;

    const auto made =
        make_connection_locked(module, std::string(name), config, std::move(settings));
    if (!made) return std::unexpected(made.error());
    // The endpoint is valueless: the handle the creation produced is the connection's, not
    // this vertex's, and it is deliberately not published anywhere the write can return it.
    return {};
}

result_t<void> transport_vertex_t::endpoint_remove_locked(const std::string& module,
                                                          std::string_view name) {
    // An empty NAME names nothing and is not an "absent" connection — it is a malformed
    // control payload, so it is refused rather than swallowed as a no-op success.
    if (name.empty()) return std::unexpected(status_t::TYPE_MISMATCH);
    // Remove-side reserve (RFC-0014 §2/§3): the endpoint cannot self-destruct. Refused
    // before the lookup, so it never reaches `retire()`.
    if (name == kConnEndpointName) return std::unexpected(status_t::PERMISSION_DENIED);

    std::string qualified(std::string_view(net_root_).substr(1));
    qualified.push_back('/');
    qualified += module;
    qualified.push_back('/');
    qualified += name;
    // An unresolvable name — never created, or already removed — is a NO-OP SUCCESS at this
    // layer (RFC-0014 §2). `retire()`'s own idempotence only covers an already-resolved
    // handle, so the endpoint owns this leg: a retried remove after a dropped reply must
    // answer the same as the first one, or teardown is not retry-safe either.
    if (!conns_.contains(qualified)) return {};
    return remove_connection_locked(qualified);
}

bool transport_vertex_t::is_structural(wire::key_view_t key) const {
    if (key.empty()) return false;  // the graph root is nobody's structural vertex
    // The net root is emitted as ONE NAME segment everywhere in this file (`make_connection`
    // composes the mount key the same way), so the whole predicate is two segment compares
    // over the key bytes — no key is materialised and nothing is allocated.
    const std::string_view root = std::string_view(net_root_).substr(1);
    const wire::key_view_t parent = key.parent();
    const std::string_view leaf = detail::as_string_view(key.last_segment());
    // `<net_root>` itself: the `:children[]` creation target the ctor registers.
    if (parent.empty()) return leaf == root;
    // `<net_root>/<module>`: exactly two segments, the first the root. Anything deeper is a
    // connection (`<net_root>/<module>/<name>`) or below one — the peer's mounted graph.
    if (!parent.parent().empty()) return false;
    if (detail::as_string_view(parent.last_segment()) != root) return false;
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    for (const module_decl_t& d : modules_) {
        if (d.module == leaf) return true;
    }
    // A module reached through the KIND-LESS spelling is never declared — `make_connection`
    // takes its name from the staging key instead (`provide_link`, the test/manual seam), and
    // mints the same `<net_root>/<module>` vertex for it. So the staged set and the live
    // connections are the other two places a module name of this plane can be read back from.
    // No fourth container is added for this (commit `221ed983` deleted exactly that state):
    // these are the ones the class already keeps for creation and teardown.
    std::string under(leaf);
    under.push_back('/');
    for (const auto& [staged_key, staged] : pending_links_) {
        (void)staged;
        if (staged_key.starts_with(under)) return true;  // key is `<module>/<name>`
    }
    std::string qualified_under(root);
    qualified_under.push_back('/');
    qualified_under += under;
    for (const auto& [qualified, conn] : conns_) {
        (void)conn;
        if (qualified.starts_with(qualified_under)) return true;  // `<root>/<module>/<name>`
    }
    return false;
}

void transport_vertex_t::provide_link(std::string module, std::string name, transport_t& link) {
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    std::string key = std::move(module);
    key.push_back('/');
    key += name;
    pending_links_.insert_or_assign(std::move(key), &link);
}

result_t<vertex_handle_t> transport_vertex_t::make_connection(std::vector<std::byte> child_key,
                                                              const tlv_t* config,
                                                              conn_role_t role) {
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    // ORDERING INVARIANT (#688 trace): this function is reached ONLY through the graph's
    // child-type catalog, i.e. from `graph_t::create_child`, which runs THE segment
    // predicate (`graph::valid_segment`, ADR-0073 §1) on the peer-supplied name BEFORE it
    // composes the key or looks up a factory. That matters because `name` becomes the last
    // segment of the router MOUNT below — an unaddressable name here would register a
    // healthy-looking child that no conforming `dst` can reach. The check is NOT repeated
    // here: one predicate, one call site per boundary, is what stops the tiers drifting
    // (the drift is the defect, #681). `transport_vertex_test.cpp`'s
    // `test_wire_name_reaches_add_child` pins the ordering against the production wiring.
    const std::string name = last_segment(child_key);
    if (name.empty()) return std::unexpected(status_t::INVALID_PATH);

    conn_settings_t settings;
    settings.role = role;  // the type default; config may override
    parse_config(config, settings);

    // Which MODULE does this connection mount under (RFC-0014 §1, ADR-0061)? The SPEC decides
    // whenever it can: a `kind` names a declared (kind, role) module (ADR-0073 §4). Only the
    // kind-less SPEC — the `provide_link` spelling — takes its module from the staged set,
    // and then only when the leaf NAME identifies exactly ONE staging. The connection lives
    // at `/net/<module>/<name>` and routes by that same path.
    //
    // The module half of a staging key is now COMPARED, never merely read back out of the
    // first hit (#883). The old scan matched the leaf NAME alone and adopted the module of
    // whichever key came first in map order — the creating SPEC's own intent was the only
    // thing that knew better, and the scan never looked at it. Two silent mis-binds followed:
    // with `mod-a/x` and `mod-b/x` staged, the SPEC meaning `mod-b` mounted at `net/mod-a/x`
    // over `mod-a`'s transport (wrong module AND wrong link); and because the scan ran BEFORE
    // the (kind, role) lookup, a SPEC naming a kind was captured by any staging sharing its
    // leaf NAME — the kind's factory never ran. One defect: a key composed of two halves,
    // matched on one.
    std::string module;
    if (!settings.kind.empty()) {
        auto declared = module_for_locked(settings.kind, settings.role);
        // Declared-only (ADR-0073 §4): a kind the application never mapped to a module
        // fails creation explicitly instead of mounting under a library-derived name.
        if (!declared) return std::unexpected(declared.error());
        module = std::move(*declared);
    } else {
        // Kind-less: the staged set is the ONLY module source, so count the leaf-NAME hits
        // rather than taking the first. Iteration order is lexicographic and carries no
        // intent — two stagings sharing a leaf NAME are genuinely ambiguous here.
        std::size_t hits = 0;
        for (const auto& [key, staged] : pending_links_) {
            const std::size_t slash = key.rfind('/');
            if (slash == std::string::npos) continue;
            if (key.compare(slash + 1, std::string::npos, name) != 0) continue;
            if (++hits > 1) break;
            module.assign(key, 0, slash);
        }
        // Neither a staged link nor a construction kind — nothing can carry the bytes. The
        // config is INCOMPLETE (`kind` is a required field once no staging supplies the
        // module), not an address to a missing thing, so this answers TYPE_MISMATCH like the
        // other missing-required-field gates (a DIAL missing `addr`, either role missing
        // `port`) — never NOT_FOUND, whose wire form `tr::path::not_found` is RFC-0014's
        // reserved "no such creator endpoint" probe answer (#1062).
        if (hits == 0) return std::unexpected(status_t::TYPE_MISMATCH);
        // Ambiguous: refuse instead of picking by map order. The SPEC must carry a `kind`
        // whose declared module says WHICH staging it meant. TYPE_MISMATCH is the config-is-
        // underspecified answer this creation path already uses (a DIAL missing `addr`/`port`
        // answers the same), and it goes out PERMANENT — re-sending this SPEC cannot help.
        if (hits > 1) return std::unexpected(status_t::TYPE_MISMATCH);
    }

    // The module is resolved; everything below is the door-independent half (RFC-0014 S2b
    // split the creator endpoint out as a SECOND door onto exactly this body).
    return make_connection_locked(module, name, config, std::move(settings));
}

result_t<vertex_handle_t> transport_vertex_t::make_connection_locked(const std::string& module,
                                                                     const std::string& name,
                                                                     const tlv_t* config,
                                                                     conn_settings_t settings) {
    // With the module resolved, the staging is a DIRECT lookup: `pending_links_` is keyed by
    // exactly this string (see `provide_link`). No scan, and no way to reach a key whose
    // module half is not the one this connection mounts under.
    std::string staged_key = module;
    staged_key.push_back('/');
    staged_key += name;

    // The routing key IS the mount path (ADR-0061): `net/<module>/<name>`. Keeping the root
    // segment in the key means the registry's precomputed run is exactly the prefix a hop
    // prepends to `src`, so the forward path needs no per-hop assembly.
    std::string qualified(std::string_view(net_root_).substr(1));
    qualified.push_back('/');
    qualified += module;
    qualified.push_back('/');
    qualified += name;
    if (conns_.contains(qualified)) return std::unexpected(status_t::PATH_IN_USE);

    // The #373 first-level shadow guard is GONE, and can be: it existed because a connection
    // NAME was the first `dst` segment, so a link sharing a name with a first-level vertex
    // black-holed every `/name/...` read onto the transport. A routable connection is now
    // addressed `/net/<module>/<name>`, so a first-level vertex cannot shadow one — and
    // keeping the guard would instead wrongly reject a connection merely named after an
    // unrelated local vertex.

    // Compose the mount key: `<net_root>/<module>/<name>`, replacing the flat key the
    // graph's `:children[]` machinery handed us (the endpoint door never had one).
    std::vector<std::byte> mount_key;
    for (std::string_view seg : {std::string_view(net_root_).substr(1), std::string_view(module),
                                 std::string_view(name)}) {
        (void)wire::emit_path_segment(mount_key, seg);
    }

    // The `/net/<module>` structural vertex, created lazily on first use. graph_.find IS the
    // dedupe — a separate seen-set would be a second source of truth (and another container
    // instantiation) for something the graph already knows.
    {
        std::vector<std::byte> mod_key;
        (void)wire::emit_path_segment(mod_key, std::string_view(net_root_).substr(1));
        (void)wire::emit_path_segment(mod_key, module);
        if (!graph_.find(mod_key)) {
            (void)graph_.register_vertex_key(mod_key, graph::role_t::STORED_VALUE, {});
        }
    }

    // Resolve the connection's link. Precedence, WITHIN the module resolved above: a
    // provide_link-staged transport wins (the test/manual seam); otherwise the config `kind`
    // selects a factory and the real socket is CONSTRUCTED here and owned by the connection.
    // A staging under a DIFFERENT module is a different connection and is not considered.
    transport_t* link = nullptr;
    std::unique_ptr<transport_t> owned;
    self_heal_link_t* engine = nullptr;
    const auto pl = pending_links_.find(staged_key);
    if (pl != pending_links_.end()) {
        link = pl->second;
    } else if (!settings.kind.empty()) {
        const auto factory = transport_types_.find(settings.kind);
        // An unregistered kind is an unsupported catalog entry — same convention as an
        // unknown SPEC `type` (SCHEMA_NOT_FOUND, the ENOTTY of creation).
        if (factory == transport_types_.end()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        if (factory->second.traits.self_heal_dial && settings.role == conn_role_t::DIAL) {
            // The RFC-0014 §4 S5 engine (#492): creation constructs NO socket — the vertex
            // is minted DORMANT and the engine dials on demand / self-heals under its own
            // worker. The factory therefore does not run here, so the universal DIAL keys
            // it would have validated are gated NOW (the same predicate `dial_or_listen`
            // applies): creation must refuse a misconfigured SPEC at the write, never
            // defer it to a first dial that answers success today and failure later.
            // Kind-PRIVATE config stays the factory's to refuse, at dial time — the §2
            // catalog validation proper is S3's, pending its own ruling.
            if (settings.addr.empty() || settings.port == 0)
                return std::unexpected(status_t::TYPE_MISMATCH);
            // The engine owns a byte COPY of the raw config: the decoded TLV borrows the
            // write's rope, which is gone by the first re-dial.
            std::vector<std::byte> raw;
            if (config != nullptr) reemit_tlv(raw, *config);
            auto heal = std::make_unique<self_heal_link_t>(factory->second.factory, settings,
                                                           std::move(raw),
                                                           factory->second.traits.delivers_ropes);
            engine = heal.get();
            owned = std::move(heal);
            link = owned.get();
        } else {
            // The raw config TLV rides along so the kind's factory can parse its own
            // kind-private keys (ADR-0043 §5 leanness: they never land in settings).
            auto built = factory->second.factory(settings, config);
            if (!built) return std::unexpected(built.error());
            owned = std::move(*built);
            link = owned.get();
        }
    } else {
        // Neither a staged link nor a construction kind — nothing can carry the bytes.
        // Same missing-required-`kind` refusal as the module-resolution gate above (#1062).
        return std::unexpected(status_t::TYPE_MISMATCH);
    }

    // A BUS link (ADR-0044) serves its currently-audible peers as this vertex's
    // synthesized `:children[]` — a POINT of POINT{NAME <peer>} members built on
    // every read from the transport's live-traffic table. NO vertex is ever
    // created for a peer. Every listed name is also a routable next-hop segment
    // (the registry's peer fallback): CAN's names always were legal segments, and
    // the ws/tcp bus servers now name accepted sessions `p<slot>` (#426, ADR-0073
    // §2) instead of the unaddressable `<ip>:<port>`, so the ADR-0044 promise holds
    // per the enumerable⇒addressable invariant (ADR-0073 §1). Routing a `dst`
    // through a bus link's own connection NAME remains the sharp edge:
    // `transport_ws_server::send` BROADCASTS, so one request draws one reply per
    // peer — rejecting that hop is #741. See reference/14 §Forwarding. Kind-neutral in the one
    // sense that matters here: any transport whose
    // bus() is non-null gets this wiring; point-to-point links keep the plain
    // vertex. The captured facet lives exactly as long as the link (the class's
    // documented lifetime contract — the graph must not outlive this object).
    graph::handlers_t handlers;
    // Asked through `bus_of` (#375 deliverable 3): on a target that closed the bus module out
    // every connection vertex is the plain one, and the synthesis below — with the TLV
    // emission and the `std::function` it captures into — is never compiled.
    if (bus_link_t* const bus = bus_of(*link)) {
        handlers.on_children = [bus]() -> result_t<view_t> {
            std::vector<std::byte> members;
            bus->enumerate_peers([&members](std::string_view peer) {
                std::vector<std::byte> body;
                wire::emit_name(body, peer);
                wire::emit_tlv(members, type_t::POINT, wire::opt_t{.pl = true}, body);
            });
            std::vector<std::byte> out;
            wire::emit_tlv(out, type_t::POINT, wire::opt_t{.pl = true}, members);
            const auto res = view::over_bytes(out);
            if (!res) return std::unexpected(status_t::BACKPRESSURE);
            return *res;
        };
    }

    // Register the identity vertex at the composed /net/<name> key (graph owns addressing).
    // On failure the just-constructed socket (if any) is torn down by `owned`'s destructor.
    result_t<vertex_handle_t> v = graph_.register_vertex_key(
        std::move(mount_key), graph::role_t::STORED_VALUE, std::move(handlers));
    if (!v) return v;  // PATH_IN_USE on a duplicate connection name

    // The engine is the sole writer of this connection's DIAL transitions (RFC-0014 §4):
    // its publisher writes the vertex VALUE directly — deliberately NOT through
    // set_link_state, whose ctl_m_ the engine's worker must never take (the teardown
    // path joins that worker while HOLDING ctl_m_). remove_connection stops the engine
    // before the vertex retires, so no publish lands on a retired vertex.
    if (engine != nullptr) {
        graph::graph_t* const g = &graph_;
        const vertex_handle_t vh = *v;
        engine->set_liveness_publisher(
            [g, vh](link_state_t s) { (void)g->write(vh, link_state_value(s)); });
    }

    const bool constructed = owned != nullptr && engine == nullptr;
    const conn_role_t effective_role = settings.role;
    conns_.insert_or_assign(qualified, conn_t{.vertex = *v,
                                              .settings = std::move(settings),
                                              .owned = std::move(owned),
                                              .engine = engine});

    // Wire the link into the router's child_registry_t — the single owner of the
    // NAME→link demux table (Brick 3a). The `/net/<name>` NAME is exactly the router
    // child name a `dst` routes through.
    //
    // Its `bool` is the WHOLE creation's verdict (#930), not advice. `add_child` answers
    // false when the registry could not grow, and it is the only place that can tell anyone
    // so — the refusal is TOTAL there, so nothing is registered and no receiver is wired.
    // Discarding it published the vertex anyway: a connection reporting UP liveness that no
    // `dst` resolves, no inbound frame lands on, and no `remove_child` can take down. Peer-
    // drivable on a bounded node, by creating connections until the slab exhausts.
    //
    // Roll the creation back in the reverse order it was built — the same order
    // `remove_connection` uses, and for the same reason: retire the identity vertex FIRST so
    // the address stops resolving (and its `:children[]` seam stops naming the link) before
    // erasing the `conns_` entry, which destroys the config-constructed socket. There is
    // nothing to un-route: the registry holds no entry to remove. BACKPRESSURE is the
    // exhausted-resource status the rest of the failable seam answers with (ADR-0065), so a
    // wiring refusal surfaces as an error the peer can retry rather than as a live-looking
    // dead connection.
    if (!router_.add_child(qualified, *link)) {
        (void)graph_.retire(*v);
        conns_.erase(qualified);
        return std::unexpected(status_t::BACKPRESSURE);
    }
    // The staged link is CONSUMED only once the connection is fully wired. Erasing it before
    // the registry call would make the rollback above lossy: the caller's provide_link
    // staging would be gone, so a retry once the pressure clears would no longer find its
    // link and would fail NOT_FOUND instead of succeeding.
    if (pl != pending_links_.end()) pending_links_.erase(pl);
    // LAST WIRING STEP (#1025): the link may now deliver. Everything an inbound frame needs
    // is in place — the registry entry is published and `add_child` has installed the
    // receiver and the down-notifier — so this is the first instant at which a decoded frame
    // has somewhere to land. A transport that started its receive thread in its own
    // constructor takes the base's no-op default; one that can defer it (the built-in `ws`
    // DIAL, which the factory constructs with `defer_recv`) spawns it here, which is what
    // stops a server's push-on-connect message being decoded into an empty sink and dropped.
    // Unconditional by design: the owner should not have to know which kinds defer.
    link->start_receiving();
    // A config-constructed socket is live once built: publish its liveness so an awaiter
    // on /net/<name> sees the bring-up. A DIAL socket is `UP`; a LISTEN socket that bound
    // is `LISTENING` (a bind failure returns an error from the factory above, so a
    // constructed LISTEN is always bound). Provided links report via set_link_state.
    //
    // The role read here is the EFFECTIVE one — the same field the factory was handed —
    // not the catalog type's default. They differ only when a `:children[]` SPEC overrode
    // `role` in its config, and in that case the old read published a `client` type's `UP`
    // over a socket the factory had just BOUND as a listener.
    if (constructed)
        (void)set_link_state_locked(qualified, effective_role == conn_role_t::LISTEN
                                                   ? link_state_t::LISTENING
                                                   : link_state_t::UP);
    // An engine-managed connection is born RESTING (RFC-0014 §4: vertex exists, no
    // socket, refcount 0) — the one initial publish the engine's worker does not own,
    // made here under ctl_m_ before any op can reach the link.
    if (engine != nullptr) (void)set_link_state_locked(qualified, link_state_t::DORMANT);
    return v;
}

result_t<void> transport_vertex_t::remove_connection(std::string_view name) {
    // The PUBLIC entry locks (#881); the RFC-0014 `NAME`-write dispatch already holds
    // `ctl_m_` — a plain, NON-RECURSIVE std::mutex — so it calls the body directly.
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    return remove_connection_locked(name);
}

/** @brief `remove_connection`'s body, for a caller that already holds `ctl_m_`. */
result_t<void> transport_vertex_t::remove_connection_locked(std::string_view name) {
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
    // Un-route BEFORE anything is destroyed: after this the NAME resolves to nothing,
    // so the socket below can go without a forward ever reaching freed memory (#494).
    (void)router_.remove_child(name);
    // Stop the S5 engine BEFORE the vertex retires: its worker publishes liveness by
    // writing that vertex, and stop() joins the worker — after this line nothing can
    // write a retired vertex. Safe under ctl_m_: the engine's worker never takes it
    // (its publisher writes the graph directly, see make_connection_locked).
    if (it->second.engine != nullptr) it->second.engine->stop();
    // Retire the identity vertex (RFC-0009 §B.6): /net/<name> re-virginizes, so a later
    // connection may take the same name — which is exactly the tombstone the registry
    // reuses. Retiring an already-retired or unregistered vertex is a no-op, so a
    // half-built connection tears down cleanly too.
    //
    // #576: this is the peer-driven append site of the value-seam park — but only for a BUS
    // link. The identity vertex bears a value seam iff it was given one above, and that
    // happens only when `link->bus() != nullptr` (CAN; a tcp/ws server wired
    // `peer_named = true`). Tearing down a point-to-point connection — every dial link, UDP,
    // loopback, a default-wired server — parks NOTHING, so a default deployment never needs
    // a collect() point. A bus node parks one ~96 B value_handlers_t per teardown, which is
    // the case #576 exists for. We do NOT collect here even then: we
    // are under ctl_m_ and on whatever thread the teardown arrived on, which is precisely
    // the free location graph_t::collect() exists to take out of the library's hands. The
    // embedder calls collect() where it knows no reader holds a seam.
    const auto retired = graph_.retire(it->second.vertex);
    // Erasing the entry destroys `owned` — the config-constructed socket — which joins
    // its recv thread. A provided link is borrowed and is left untouched.
    conns_.erase(it);
    return retired;
}

result_t<void> transport_vertex_t::set_link_state(std::string_view name, link_state_t state) {
    // The PUBLIC entry locks (#881). This is the liveness door a TRANSPORT thread knocks
    // on for a provided link, while create/remove is wire-driven on a receive thread — so
    // the unguarded find here walked `conns_` mid-rebalance and could return a node
    // `remove_connection` was erasing. `make_connection` publishes creation liveness from
    // inside its own locked section via `set_link_state_locked`, which is why the fix is a
    // split: `ctl_m_` is non-recursive, so it cannot re-enter through this wrapper.
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    return set_link_state_locked(name, state);
}

/** @brief `set_link_state`'s body, for a caller that already holds `ctl_m_`. */
result_t<void> transport_vertex_t::set_link_state_locked(std::string_view name,
                                                         link_state_t state) {
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
    // A write to the vertex value bumps write_seq_ + delivers to subscribers (RFC-0008
    // §D) — so await(/net/<name>) fires and a subscribe streams the transition. Reached
    // under `ctl_m_`, which is the head of the declared lock order (this → fwd_router_t
    // ctl_m_ → graph_t map_mutex_ → the vertex stripe), so descending into the graph from
    // here takes the locks in that order and never against it.
    return graph_.write(it->second.vertex, link_state_value(state));
}

result_t<void> transport_vertex_t::acquire_link(std::string_view name) {
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
    // Lock order: ctl_m_ → the engine's own mutex; the engine never takes ctl_m_ back.
    // A connection without an engine answers success as a no-op (see the header: a
    // LISTEN ignores refcount per RFC-0014 §4, and a manual link's liveness is manual).
    if (it->second.engine != nullptr) it->second.engine->acquire();
    return {};
}

result_t<void> transport_vertex_t::release_link(std::string_view name) {
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
    if (it->second.engine != nullptr) it->second.engine->release();
    return {};
}

const conn_settings_t* transport_vertex_t::settings_of(std::string_view name) const {
    const std::lock_guard ctl(
        ctl_m_);  // ADR-0063 §3 — readers of conns_ race the insert's rebalance
    const auto it = conns_.find(name);
    return it == conns_.end() ? nullptr : &it->second.settings;
}

transport_t* transport_vertex_t::link_of(std::string_view name) const {
    const std::lock_guard ctl(
        ctl_m_);  // ADR-0063 §3 — readers of conns_ race the insert's rebalance
    const auto it = conns_.find(name);
    return it == conns_.end() ? nullptr : it->second.owned.get();
}

}  // namespace tr::net
