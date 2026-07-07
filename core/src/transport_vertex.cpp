/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_vertex.hpp"

#include <cstring>
#include <memory>
#include <span>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/path.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_ws.hpp"

namespace tr::net {

using graph::path_t;
using graph::result_t;
using graph::status_t;
using graph::vertex_handle_t;
using view::view_t;
using wire::tlv_t;
using wire::type_t;

namespace {

// The last NAME segment of a canonical PATH-payload key = the connection's NAME. The
// key is `<...prior NAMEs...><NAME len-prefixed name>`; walk the len-prefixed records.
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

// Parse the optional SPEC `config` SETTINGS: positional NAME-key / value pairs —
// NAME "addr" NAME <utf8>, NAME "kind" NAME <utf8>, NAME "port" VALUE u16, NAME "role"
// VALUE u8 (overrides the type default), NAME "keepalive" VALUE u32. ONLY the universal
// keys land here (ADR-0043 §5 leanness): kind-private pairs (e.g. quic's `cert`/`key`)
// are the kind's factory's business — it parses them from the raw config TLV it
// receives. Unknown pairs are ignored (forward-compat).
void parse_config(const tlv_t* config, conn_settings_t& s) {
    if (config == nullptr) return;
    const std::vector<tlv_t>& ch = config->children;
    for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
        if (ch[i].type != type_t::NAME) continue;
        const std::string_view key = detail::as_string_view(ch[i].payload);
        const tlv_t& val = ch[i + 1];
        if (key == "addr" && val.type == type_t::NAME) {
            s.addr = std::string(detail::as_string_view(val.payload));
        } else if (key == "kind" && val.type == type_t::NAME) {
            s.kind = std::string(detail::as_string_view(val.payload));
        } else if (key == "port" && val.type == type_t::VALUE && !val.payload.empty()) {
            s.port = detail::load_le<std::uint16_t>(val.payload);
        } else if (key == "role" && val.type == type_t::VALUE && !val.payload.empty()) {
            s.role = detail::load_le<std::uint8_t>(val.payload) == 0 ? conn_role_t::DIAL
                                                                     : conn_role_t::LISTEN;
        } else if (key == "keepalive" && val.type == type_t::VALUE && !val.payload.empty()) {
            s.keepalive_ms = detail::load_le<std::uint32_t>(val.payload);
        } else if (key == "max_frame" && val.type == type_t::VALUE && !val.payload.empty()) {
            s.max_frame = detail::load_le<std::uint32_t>(val.payload);
        }
    }
}

// A 1-byte link-state VALUE TLV (0x00 down / 0x01 up) as an owned view.
[[nodiscard]] view_t link_state_value(bool up) {
    std::vector<std::byte> out;
    const std::byte b{static_cast<std::uint8_t>(up ? 1 : 0)};
    wire::emit_tlv(out, type_t::VALUE, wire::opt_t{}, std::span<const std::byte>(&b, 1));
    return view::over_bytes(out).value_or(
        view_t{});  // empty view on alloc failure (caller-checked)
}

// Construct concrete transport `T`, map a failed `ok()` (dial/bind/handshake failure)
// to NOT_FOUND, and upcast to the base handle — the ok()→NOT_FOUND mapping every
// built-in stream factory shares, in one locus (`ok()` is a concrete, non-virtual
// method, so the check is templated on `T`, not called through `transport_t`).
template <class T, class... Args>
[[nodiscard]] result_t<std::unique_ptr<transport_t>> make_checked(Args&&... args) {
    auto t = std::make_unique<T>(std::forward<Args>(args)...);
    if (!t->ok()) return std::unexpected(status_t::NOT_FOUND);
    return t;  // unique_ptr<T> => unique_ptr<transport_t> (upcast move)
}

// The role dispatch every built-in socket factory repeats: DIAL requires `addr` + `port`
// and constructs the dialer; LISTEN requires `port` and constructs the listener
// (`TYPE_MISMATCH` on a missing field). The per-transport construction — and, for ws,
// the fact that DIAL and LISTEN are DIFFERENT concrete types — stays in the thunks.
template <class Dial, class Listen>
[[nodiscard]] result_t<std::unique_ptr<transport_t>> dial_or_listen(const conn_settings_t& s,
                                                                    Dial&& dial, Listen&& listen) {
    if (s.role == conn_role_t::DIAL) {
        if (s.addr.empty() || s.port == 0) return std::unexpected(status_t::TYPE_MISMATCH);
        return dial();
    }
    if (s.port == 0) return std::unexpected(status_t::TYPE_MISMATCH);
    return listen();
}

// Built-in `udp`: DIAL binds an ephemeral local port and targets `addr:port`; LISTEN
// binds `port` peer-less — udp_transport_t then learns the peer from each inbound
// datagram's source (the single-peer UDP-server shape), so replies to a dialing client
// (whose ephemeral port is unknowable in advance) route back. `keepalive` is ignored
// (UDP is connectionless; there is no link to keep alive).
// `rx_backend` is the ADR-0042 §2 receive-segment seam, threaded from the
// transport_vertex_t constructor so config-constructed sockets participate in
// owning view delivery with the host's memory policy.
result_t<std::unique_ptr<transport_t>> make_udp(const conn_settings_t& s,
                                                mem::mem_backend_t* rx_backend) {
    return dial_or_listen(
        s, [&] { return make_checked<udp_transport_t>(0, s.addr, s.port, rx_backend); },
        [&] { return make_checked<udp_transport_t>(s.port, s.addr, 0, rx_backend); });
}

// Built-in `tcp` (M6): DIAL = tcp_transport_t(addr, port) — a SYNCHRONOUS TCP connect
// at creation time (the peer's listener must be up, or the SPEC write fails NOT_FOUND);
// LISTEN = tcp_transport_t(port), accepting ONE inbound peer at a time (the
// transport_ws_server one-peer model). Length-prefix framing is internal to the
// transport. `keepalive` is ignored (TCP's own keepalive/liveness is #66 lifecycle).
// `rx_backend` is the ADR-0042 §2 receive-segment seam, as for `udp`.
result_t<std::unique_ptr<transport_t>> make_tcp(const conn_settings_t& s,
                                                mem::mem_backend_t* rx_backend) {
    return dial_or_listen(
        s, [&] { return make_checked<tcp_transport_t>(s.addr, s.port, rx_backend, s.max_frame); },
        [&] { return make_checked<tcp_transport_t>(s.port, rx_backend, s.max_frame); });
}

// Built-in `ws`: DIAL = transport_ws_client(addr, port) — a SYNCHRONOUS TCP connect +
// RFC 6455 opening handshake at creation time (the peer's server must be up, or the
// SPEC write fails NOT_FOUND); LISTEN = transport_ws_server(port), accepting ONE
// inbound peer (the headline browser↔board link). `keepalive` is ignored by both
// (PING/PONG is handled at the ws protocol layer). Like all built-ins, ws has no
// kind-private config keys, so the raw config TLV is ignored (ADR-0043 §5).
result_t<std::unique_ptr<transport_t>> make_ws(const conn_settings_t& s,
                                               const tlv_t* /*raw_config*/) {
    return dial_or_listen(
        s, [&] { return make_checked<transport_ws_client>(s.addr, s.port); },
        [&] { return make_checked<transport_ws_server>(s.port); });
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
    // The built-in transport-factory catalog entries (config `kind` selectors). The
    // udp and tcp factories close over the injected RX backend (ADR-0042 §2); ws stays
    // span-delivering until its frame assembly is pointed at segments (ADR-0042 §4).
    register_transport_type("udp", [this](const conn_settings_t& s, const tlv_t* /*raw_config*/) {
        return make_udp(s, rx_backend_);
    });
    register_transport_type("tcp", [this](const conn_settings_t& s, const tlv_t* /*raw_config*/) {
        return make_tcp(s, rx_backend_);
    });
    register_transport_type("ws", make_ws);
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
