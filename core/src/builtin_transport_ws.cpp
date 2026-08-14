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

void register_ws_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend) {
    // Built-in `ws`: DIAL = transport_ws_client(addr, port) — a SYNCHRONOUS TCP connect +
    // RFC 6455 opening handshake at creation time (the peer's server must be up, or the
    // SPEC write fails TRANSPORT_DOWN); LISTEN = transport_ws_server(port), serving MANY
    // concurrent inbound peers (#362). `keepalive` is ignored by both (PING/PONG is
    // handled at the ws protocol layer).
    //
    // Both halves take the SAME (rx_backend, max_frame) seam tcp/quic/webtransport take,
    // in the same position (#872): every reassembled message fragment is drawn from
    // rx_backend, and `max_frame` — the universal `:settings` key parsed centrally into
    // conn_settings_t — is the declared-length bound a peer may not exceed. Passing
    // `s.max_frame` through is what makes the honored cap the CONFIGURED one; a literal
    // here would be the synthetic limit the doctrine forbids.
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
    // A third ws-private key, honored on BOTH halves (#838):
    //  - `liveness_window` (VALUE u32, ms; default 0 = kDefaultLivenessWindowMs) — how long
    //    a peer may fail to take bytes before it is broken. It is the send bound's
    //    provenance: a host has no task watchdog to derive one from the way the MCU link
    //    does (#835), so the number comes from the deployer, exactly as `connect_timeout`
    //    and CAN's `peer_ttl` (ADR-0044) do. Kind-private, not conn_settings_t: it is a
    //    property of a STREAM peer, not of every kind.
    // ws has both a dial and a listen shape, so it is TWO modules (RFC-0014 §1) — but the
    // library registers NEITHER (ADR-0073 §4): the application declares each module under a
    // name IT chooses via register_module (kWsClientSuggestedModule / kWsServerSuggestedModule
    // in transport_ws.hpp are the suggested defaults).
    vertex.register_transport_type("ws", [rx_backend](const conn_settings_t& s,
                                                      const wire::tlv_t* raw_config) {
        const config_reader_t cfg(raw_config);
        const bool peer_named = cfg.flag("peer_named").value_or(false);
        const auto max_peers = static_cast<std::size_t>(cfg.u32("max_peers").value_or(0));
        const std::uint32_t liveness_window = cfg.u32("liveness_window").value_or(0);
        return dial_or_listen(
            s,
            [&] {
                // DEFERRED recv thread (#1025): the connection is dialled and handshaken here,
                // but `transport_vertex_t::make_connection` only wires the receiver a few
                // steps later (register the vertex, then `fwd_router_t::add_child`). A server
                // that pushes its state the instant our connect completes has that message in
                // flight through that whole window, and a recv thread running inside it would
                // decode it into an empty sink and drop it with no counter moving. The vertex
                // calls `start_receiving()` once the link is fully wired.
                return make_checked<transport_ws_client>(s.addr, s.port, rx_backend, s.max_frame,
                                                         /*recv_stack=*/std::size_t{0},
                                                         /*defer_recv=*/true, liveness_window);
            },
            [&] {
                return make_checked<transport_ws_server>(s.port, rx_backend, s.max_frame, max_peers,
                                                         peer_named, /*recv_stack=*/std::size_t{0},
                                                         liveness_window);
            });
    });
}

}  // namespace tr::net
