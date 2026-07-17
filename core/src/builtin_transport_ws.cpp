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
#include <cstddef>

#include "libtracer/builtin_transports.hpp"
#include "libtracer/config_reader.hpp"
#include "libtracer/transport_vertex.hpp"
#include "libtracer/transport_ws.hpp"

namespace tr::net {

void register_ws_transport(transport_vertex_t& vertex, mem::mem_backend_t* /*rx_backend*/) {
    // Built-in `ws`: DIAL = transport_ws_client(addr, port) — a SYNCHRONOUS TCP connect +
    // RFC 6455 opening handshake at creation time (the peer's server must be up, or the
    // SPEC write fails NOT_FOUND); LISTEN = transport_ws_server(port), serving MANY
    // concurrent inbound peers (#362). `keepalive` is ignored by both (PING/PONG is
    // handled at the ws protocol layer). ws stays span-delivering until its frame
    // assembly is pointed at segments (ADR-0042 §4), so it does not draw from the
    // rx_backend seam.
    //
    // The two LISTEN-side ws-private keys are parsed HERE from the raw config TLV — the
    // ADR-0043 §5 leanness ruling (as `quic` does for cert/key and `can` for ifname/node):
    // neither lands in the shared conn_settings_t, because neither is universal across
    // kinds. Both are ignored on a DIAL (a client has exactly one peer, itself):
    //  - `peer_named` (VALUE u8, nonzero = true; default false) exposes the bus_link_t
    //    facet (ADR-0044), so each inbound peer gets its own return-route identity and
    //    the connection's `:children[]` synthesizes the live peer listing. WITHOUT it a
    //    SPEC-created listener has a null bus() and is a broadcast link — send() fans out
    //    to every open peer and no peer enumeration is reachable in-band at all, which
    //    left ADR-0044's peer-listing story creatable only by direct construction (#408).
    //  - `max_peers` (VALUE u32; default 0 = unbounded, host-bounded per RFC-0006) is the
    //    concurrent-peer admission cap.
    vertex.register_transport_type(
        "ws", [](const conn_settings_t& s, const wire::tlv_t* raw_config) {
            const config_reader_t cfg(raw_config);
            const bool peer_named = cfg.flag("peer_named").value_or(false);
            const auto max_peers = static_cast<std::size_t>(cfg.u32("max_peers").value_or(0));
            return dial_or_listen(
                s, [&] { return make_checked<transport_ws_client>(s.addr, s.port); },
                [&] { return make_checked<transport_ws_server>(s.port, max_peers, peer_named); });
        });
}

}  // namespace tr::net
