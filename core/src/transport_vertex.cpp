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
#include "libtracer/mem_heap.hpp"
#include "libtracer/path.hpp"
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
 * The
 * key is `<...prior NAMEs...><NAME len-prefixed name>`; walk the len-prefixed records.
 */
[[nodiscard]] std::string last_segment(std::span<const std::byte> key) {
    std::span<const std::byte> last;
    std::size_t i = 0;
    while (i + 4 <= key.size()) {
        const std::size_t len = detail::load_le<std::uint16_t>(key.subspan(i + 2, 2));
        if (i + 4 + len > key.size()) break;
        last = key.subspan(i + 4, len);
        i += 4 + len;
    }
    return std::string(detail::as_string_view(last));
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
    if (const auto v = cfg.u16("port")) s.port = *v;
    if (const auto v = cfg.u8("role")) s.role = *v == 0 ? conn_role_t::DIAL : conn_role_t::LISTEN;
    if (const auto v = cfg.u32("keepalive")) s.keepalive_ms = *v;
    if (const auto v = cfg.u32("max_frame")) s.max_frame = *v;
    if (const auto v = cfg.u32("backoff")) s.backoff_ms = *v;
    if (const auto v = cfg.u32("connect_timeout")) s.connect_timeout_ms = *v;
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
                                       slim_net_t)
    : graph_(graph), router_(router), net_root_(std::move(net_root)), rx_backend_(rx_backend) {
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
                                       std::string net_root, mem::mem_backend_t* rx_backend)
    : transport_vertex_t(graph, router, std::move(net_root), rx_backend, slim_net) {
    register_builtin_transports(*this, rx_backend_);
}

void transport_vertex_t::register_transport_type(std::string kind, transport_factory_t factory) {
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    transport_types_.insert_or_assign(std::move(kind), std::move(factory));
}

result_t<void> transport_vertex_t::register_module(std::string module, std::string kind,
                                                   conn_role_t role) {
    // Registration is a minting boundary (ADR-0073 §1): the ONE shared segment-validity
    // predicate gates the name here, exactly as path_t::parse gates the local string tier.
    if (!graph::valid_segment(module)) return std::unexpected(status_t::INVALID_PATH);
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
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

    // Which MODULE does this connection mount under (RFC-0014 §1, ADR-0061)? A staged link
    // carries no `kind`, so the module is the one it was staged under; otherwise it follows
    // from (kind, role). The connection then lives at `/net/<module>/<name>` and routes by
    // that same path.
    std::string module;
    std::string staged_key;
    for (const auto& [key, l] : pending_links_) {
        const std::size_t slash = key.rfind('/');
        if (slash != std::string::npos && key.compare(slash + 1, std::string::npos, name) == 0) {
            module = key.substr(0, slash);
            staged_key = key;
            break;
        }
    }
    if (module.empty()) {
        // Neither a staged link nor a construction kind — nothing can carry the bytes
        // (checked before module resolution: a kind-less SPEC has no module to look up).
        if (settings.kind.empty()) return std::unexpected(status_t::NOT_FOUND);
        auto declared = module_for_locked(settings.kind, settings.role);
        // Declared-only (ADR-0073 §4): a kind the application never mapped to a module
        // fails creation explicitly instead of mounting under a library-derived name.
        if (!declared) return std::unexpected(declared.error());
        module = std::move(*declared);
    }

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
    // graph's `:children[]` machinery handed us.
    std::vector<std::byte> mount_key;
    for (std::string_view seg : {std::string_view(net_root_).substr(1), std::string_view(module),
                                 std::string_view(name)}) {
        wire::emit_name(mount_key, seg);
    }
    child_key = std::move(mount_key);

    // The `/net/<module>` grouping vertex, created lazily on first use. graph_.find IS the
    // dedupe — a separate seen-set would be a second source of truth (and another container
    // instantiation) for something the graph already knows.
    {
        std::vector<std::byte> mod_key;
        wire::emit_name(mod_key, std::string_view(net_root_).substr(1));
        wire::emit_name(mod_key, module);
        if (!graph_.find(mod_key)) {
            (void)graph_.register_vertex_key(mod_key, graph::role_t::STORED_VALUE, {});
        }
    }

    // Resolve the connection's link. Precedence: a provide_link-staged transport wins
    // (the test/manual seam); otherwise the config `kind` selects a factory and the
    // real socket is CONSTRUCTED here and owned by the connection.
    transport_t* link = nullptr;
    std::unique_ptr<transport_t> owned;
    const auto pl = staged_key.empty() ? pending_links_.end() : pending_links_.find(staged_key);
    if (pl != pending_links_.end()) {
        link = pl->second;
    } else if (!settings.kind.empty()) {
        const auto factory = transport_types_.find(settings.kind);
        // An unregistered kind is an unsupported catalog entry — same convention as an
        // unknown SPEC `type` (SCHEMA_NOT_FOUND, the ENOTTY of creation).
        if (factory == transport_types_.end()) return std::unexpected(status_t::SCHEMA_NOT_FOUND);
        // The raw config TLV rides along so the kind's factory can parse its own
        // kind-private keys (ADR-0043 §5 leanness: they never land in settings).
        auto built = factory->second(settings, config);
        if (!built) return std::unexpected(built.error());
        owned = std::move(*built);
        link = owned.get();
    } else {
        // Neither a staged link nor a construction kind — nothing can carry the bytes.
        return std::unexpected(status_t::NOT_FOUND);
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
    if (bus_link_t* const bus = link->bus()) {
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
        std::move(child_key), graph::role_t::STORED_VALUE, std::move(handlers));
    if (!v) return v;  // PATH_IN_USE on a duplicate connection name

    const bool constructed = owned != nullptr;
    conns_.insert_or_assign(
        qualified,
        conn_t{.vertex = *v, .settings = std::move(settings), .owned = std::move(owned)});

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
    if (constructed)
        (void)set_link_state_locked(
            qualified, role == conn_role_t::LISTEN ? link_state_t::LISTENING : link_state_t::UP);
    return v;
}

result_t<void> transport_vertex_t::remove_connection(std::string_view name) {
    const std::lock_guard ctl(ctl_m_);  // ADR-0063 §3 control-plane serialization
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
    // Un-route BEFORE anything is destroyed: after this the NAME resolves to nothing,
    // so the socket below can go without a forward ever reaching freed memory (#494).
    (void)router_.remove_child(name);
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
