/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

#include "libtracer/transport_vertex.hpp"

#include <cstring>
#include <span>
#include <utility>

#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/path.hpp"
#include "libtracer/tlv_emit.hpp"

namespace tr::net {

using graph::path_t;
using graph::result_t;
using graph::status_t;
using graph::vertex_t;
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
// NAME "addr" NAME <utf8>, NAME "port" VALUE u16, NAME "role" VALUE u8 (overrides the
// type default), NAME "keepalive" VALUE u32. Unknown pairs are ignored (forward-compat).
void parse_config(const tlv_t* config, conn_settings_t& s) {
    if (config == nullptr) return;
    const std::vector<tlv_t>& ch = config->children;
    for (std::size_t i = 0; i + 1 < ch.size(); ++i) {
        if (ch[i].type != type_t::NAME) continue;
        const std::string_view key = detail::as_string_view(ch[i].payload);
        const tlv_t& val = ch[i + 1];
        if (key == "addr" && val.type == type_t::NAME) {
            s.addr = std::string(detail::as_string_view(val.payload));
        } else if (key == "port" && val.type == type_t::VALUE && !val.payload.empty()) {
            s.port = detail::load_le<std::uint16_t>(val.payload);
        } else if (key == "role" && val.type == type_t::VALUE && !val.payload.empty()) {
            s.role = detail::load_le<std::uint8_t>(val.payload) == 0 ? conn_role_t::DIAL
                                                                     : conn_role_t::LISTEN;
        } else if (key == "keepalive" && val.type == type_t::VALUE && !val.payload.empty()) {
            s.keepalive_ms = detail::load_le<std::uint32_t>(val.payload);
        }
    }
}

// A 1-byte link-state VALUE TLV (0x00 down / 0x01 up) as an owned view.
[[nodiscard]] view_t link_state_value(bool up) {
    std::vector<std::byte> out;
    const std::byte b{static_cast<std::uint8_t>(up ? 1 : 0)};
    detail::emit_tlv(out, type_t::VALUE, wire::opt_t{}, std::span<const std::byte>(&b, 1));
    return view::over_bytes(out);
}

}  // namespace

transport_vertex_t::transport_vertex_t(graph::graph_t& graph, fwd_router_t& router,
                                       std::string net_root)
    : graph_(graph), router_(router), net_root_(std::move(net_root)) {
    // Register the `/net` parent if it isn't already (it is the `:children[]` target).
    if (graph_.find(path_t::parse(net_root_)->key()) == nullptr) {
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
}

void transport_vertex_t::provide_link(std::string name, transport_t& link) {
    pending_links_.insert_or_assign(std::move(name), &link);
}

result_t<vertex_t*> transport_vertex_t::make_connection(std::vector<std::byte> child_key,
                                                        const tlv_t* config, conn_role_t role) {
    const std::string name = last_segment(child_key);
    if (name.empty()) return std::unexpected(status_t::INVALID_PATH);

    // The link must have been supplied at setup (Stage-1 (A): no socket construction).
    const auto pl = pending_links_.find(name);
    if (pl == pending_links_.end()) return std::unexpected(status_t::NOT_FOUND);
    transport_t* const link = pl->second;

    // Register the identity vertex at the composed /net/<name> key (graph owns addressing).
    result_t<vertex_t*> v =
        graph_.register_vertex_key(std::move(child_key), graph::role_t::STORED_VALUE);
    if (!v) return v;  // PATH_IN_USE on a duplicate connection name

    conn_settings_t settings;
    settings.role = role;  // the type default; config may override
    parse_config(config, settings);

    conns_.insert_or_assign(name, conn_t{.vertex = *v, .link = link, .settings = settings});
    pending_links_.erase(pl);

    // Stage-1: wire the link into the router so bytes still flow the tested FWD path.
    // The `/net/<name>` NAME is exactly the router child name a `dst` routes through.
    router_.add_child(name, *link);
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
