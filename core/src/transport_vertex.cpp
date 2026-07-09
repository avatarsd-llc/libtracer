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
}

/** @brief A 1-byte link-state VALUE TLV (0x00 down / 0x01 up) as an owned view. */
[[nodiscard]] view_t link_state_value(bool up) {
    std::vector<std::byte> out;
    const std::byte b{static_cast<std::uint8_t>(up ? 1 : 0)};
    wire::emit_tlv(out, type_t::VALUE, wire::opt_t{}, std::span<const std::byte>(&b, 1));
    return view::over_bytes(out).value_or(
        view_t{});  // empty view on alloc failure (caller-checked)
}

}  // namespace

transport_vertex_t::transport_vertex_t(graph::graph_t& graph, fwd_router_t& router,
                                       std::string net_root, mem::mem_backend_t* rx_backend)
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
    // Register the built-in transport-factory catalog entries (config `kind` selectors)
    // that this build compiled in — udp / tcp / ws, each from its own translation unit
    // gated by a per-module CMake option (register_builtin_transports is the full-node or
    // CMake-generated dispatcher; see builtin_transports.hpp). Keeping the concrete
    // transport references out of this file lets a build DROP a transport without leaving
    // a dangling reference — module selection is by compiled TU, no preprocessor macros.
    register_builtin_transports(*this, rx_backend_);
    // The `quic` kind is NOT a builtin: it lives in the separate libtracer_quic
    // module (ADR-0043), which extends this catalog through register_transport_type
    // (quic_transport_factory) — this file never learns about msquic (open/closed).
}

void transport_vertex_t::register_transport_type(std::string kind, transport_factory_t factory) {
    transport_types_.insert_or_assign(std::move(kind), std::move(factory));
}

void transport_vertex_t::provide_link(std::string name, transport_t& link) {
    pending_links_.insert_or_assign(std::move(name), &link);
}

result_t<vertex_handle_t> transport_vertex_t::make_connection(std::vector<std::byte> child_key,
                                                              const tlv_t* config,
                                                              conn_role_t role) {
    const std::string name = last_segment(child_key);
    if (name.empty()) return std::unexpected(status_t::INVALID_PATH);
    if (conns_.contains(name)) return std::unexpected(status_t::PATH_IN_USE);

    conn_settings_t settings;
    settings.role = role;  // the type default; config may override
    parse_config(config, settings);

    // Resolve the connection's link. Precedence: a provide_link-staged transport wins
    // (the test/manual seam); otherwise the config `kind` selects a factory and the
    // real socket is CONSTRUCTED here and owned by the connection.
    transport_t* link = nullptr;
    std::unique_ptr<transport_t> owned;
    const auto pl = pending_links_.find(name);
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
    // created for a peer, and each listed name doubles as a routable next-hop
    // segment (the registry's peer fallback). Kind-neutral: any transport whose
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
        name, conn_t{.vertex = *v, .settings = std::move(settings), .owned = std::move(owned)});
    if (pl != pending_links_.end()) pending_links_.erase(pl);

    // Wire the link into the router's child_registry_t — the single owner of the
    // NAME→link demux table (Brick 3a). The `/net/<name>` NAME is exactly the router
    // child name a `dst` routes through.
    router_.add_child(name, *link);
    // A config-constructed socket is live once built: write its link state up so an
    // awaiter on /net/<name> sees the bring-up. Provided links report via set_link_state.
    if (constructed) (void)set_link_state(name, true);
    return v;
}

result_t<void> transport_vertex_t::set_link_state(std::string_view name, bool up) {
    const auto it = conns_.find(name);
    if (it == conns_.end()) return std::unexpected(status_t::NOT_FOUND);
    // A write to the vertex value bumps write_seq_ + notifies — so await(/net/<name>) fires.
    return graph_.write(it->second.vertex, link_state_value(up));
}

const conn_settings_t* transport_vertex_t::settings_of(std::string_view name) const {
    const auto it = conns_.find(name);
    return it == conns_.end() ? nullptr : &it->second.settings;
}

}  // namespace tr::net
