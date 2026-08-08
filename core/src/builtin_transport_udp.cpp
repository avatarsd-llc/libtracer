/**
 * @file
 * @brief The built-in `udp` transport-factory registration.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Compiled only when the
 * LIBTRACER_TRANSPORT_UDP module option (and the net plane) is ON; the full-node /
 * generated register_builtin_transports() calls register_udp_transport only then, so a
 * UDP-less build carries no reference to udp_transport_t. See builtin_transports.hpp.
 */
#include "libtracer/builtin_transports.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_vertex.hpp"

namespace tr::net {

void register_udp_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend) {
    // Built-in `udp`: DIAL binds an ephemeral local port and targets `addr:port`;
    // LISTEN binds `port` peer-less — udp_transport_t then learns the peer from each
    // inbound datagram's source (the single-peer UDP-server shape), so replies to a
    // dialing client route back. `keepalive` is ignored (UDP is connectionless).
    // `rx_backend` is the ADR-0042 §2 receive-segment seam, so a config-constructed
    // socket participates in owning view delivery with the host's memory policy, and
    // `s.max_frame` — the universal `:settings` key parsed centrally into conn_settings_t —
    // is the largest datagram this connection accepts (#926). Threading it is what makes
    // the honored cap the CONFIGURED one; dropping it left the key inert on this kind
    // while tcp/ws/quic/webtransport all honored it.
    // udp has both a dial and a listen shape, so it is TWO modules (RFC-0014 §1) — but the
    // library registers NEITHER (ADR-0073 §4): the application declares each module under a
    // name IT chooses via register_module (kUdpClientSuggestedModule /
    // kUdpServerSuggestedModule in transport_udp.hpp are the suggested defaults).
    vertex.register_transport_type("udp", [rx_backend](const conn_settings_t& s,
                                                       const wire::tlv_t* /*raw_config*/) {
        return dial_or_listen(
            s,
            [&] {
                return make_checked<udp_transport_t>(0, s.addr, s.port, rx_backend, s.max_frame);
            },
            [&] {
                return make_checked<udp_transport_t>(s.port, s.addr, 0, rx_backend, s.max_frame);
            });
    });
}

}  // namespace tr::net
