/**
 * @file
 * @brief The built-in `ws` transport-factory registration.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Compiled only when the
 * LIBTRACER_TRANSPORT_WS module option (and the net plane) is ON; the full-node /
 * generated register_builtin_transports() calls register_ws_transport only then, so a
 * WS-less build carries no reference to the ws transports. See builtin_transports.hpp.
 */
#include "libtracer/builtin_transports.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer/transport_ws.hpp"

namespace tr::net {

void register_ws_transport(transport_vertex_t& vertex, mem::mem_backend_t* /*rx_backend*/) {
    // Built-in `ws`: DIAL = transport_ws_client(addr, port) — a SYNCHRONOUS TCP connect +
    // RFC 6455 opening handshake at creation time (the peer's server must be up, or the
    // SPEC write fails NOT_FOUND); LISTEN = transport_ws_server(port), accepting ONE
    // inbound peer (the headline browser<->board link). `keepalive` is ignored by both
    // (PING/PONG is handled at the ws protocol layer). Like all built-ins, ws has no
    // kind-private config keys, so the raw config TLV is ignored (ADR-0043 §5). ws stays
    // span-delivering until its frame assembly is pointed at segments (ADR-0042 §4), so
    // it does not draw from the rx_backend seam.
    vertex.register_transport_type(
        "ws", [](const conn_settings_t& s, const wire::tlv_t* /*raw_config*/) {
            return dial_or_listen(
                s, [&] { return make_checked<transport_ws_client>(s.addr, s.port); },
                [&] { return make_checked<transport_ws_server>(s.port); });
        });
}

}  // namespace tr::net
