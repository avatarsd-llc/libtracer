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
#include <utility>

#include "libtracer/builtin_transports.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_vertex.hpp"

namespace tr::net {

void register_udp_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend,
                            mem::block_source_t* egress_src) {
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
    // RFC-0014 §4 S5 (#1548): `udp` is a POINT-TO-POINT kind, so its DIAL connections are
    // engine-managed — see `kBuiltinPointToPointTraits` for the whole reasoning, including
    // why `self_heal_dial` is build-conditioned. RE-RUNNABILITY (the engine runs this
    // factory once per dial attempt): the thunk captures only three raw pointers whose
    // lifetime outlives the vertex, and the DIAL arm constructs a FRESH `udp_transport_t`
    // binding a fresh ephemeral local port each run — nothing here is consumed, moved from,
    // or memoized between runs, so the second dial builds exactly what the first did (a new
    // ephemeral source port, which no peer depends on: a udp DIAL socket is peer-addressed,
    // never peer-known). The LISTEN arm never reaches the engine at all.
    // #2574: `node` is the vertex's node-wide link memory default, or null; every link this
    // thunk builds draws its io, state and stack from it (`builtin_link_memory`). #2147:
    // `wait` is the node's wait module, or null; every link this thunk builds puts its
    // sockets on it and owns no thread of its own.
    vertex.register_transport_type(
        "udp",
        [rx_backend, egress = builtin_egress(egress_src, vertex.link_memory()),
         node = vertex.link_memory(), wait = vertex.wait_module()](
            const conn_settings_t& s, const wire::tlv_node_t* /*raw_config*/,
            mem::block_source_t& src) {
            // One construction serves both roles: a dial binds an ephemeral port and names the
            // peer's, a listen binds its own and learns the peer from the first datagram.
            const bool dial = s.role == conn_role_t::DIAL;
            const udp_config_t config{.memory = builtin_link_memory(rx_backend, egress, node),
                                      .max_frame = s.max_frame,
                                      .wait = wait};
            const auto build = [&] {
                return make_checked<udp_transport_t>(src, dial ? std::uint16_t{0} : s.port, s.addr,
                                                     dial ? s.port : std::uint16_t{0}, config);
            };
            auto link = dial_or_listen(s, build, build);
            // The ADR-0079 egress store, wired before the link is handed to the router
            // (#873).
            return with_egress_source(std::move(link), egress);
        },
        kBuiltinPointToPointTraits);
}

}  // namespace tr::net
