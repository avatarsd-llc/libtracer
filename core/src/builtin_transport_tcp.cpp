/**
 * @file
 * @brief The built-in `tcp` transport-factory registration.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Compiled only when the
 * LIBTRACER_TRANSPORT_TCP module option (and the net plane) is ON; the full-node /
 * generated register_builtin_transports() calls register_tcp_transport only then, so a
 * TCP-less build carries no reference to tcp_transport_t. See builtin_transports.hpp.
 */
#include "libtracer/builtin_transports.hpp"
#include "libtracer/config_reader.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_vertex.hpp"

namespace tr::net {

void register_tcp_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend) {
    // Built-in `tcp`: DIAL = tcp_transport_t(addr, port) — a SYNCHRONOUS TCP connect at
    // creation time (the peer's listener must be up, or the SPEC write fails NOT_FOUND);
    // LISTEN = transport_tcp_server(port), the multi-peer listener (the ws factory's
    // shape: a single-client deployment behaves exactly as the one-peer listener always
    // did). Length-prefix framing is internal to the transport. `keepalive` is ignored
    // (TCP's own keepalive is the #66 lifecycle). `rx_backend` is the ADR-0042 §2
    // receive-segment seam.
    //
    // The two LISTEN-side tcp-private keys mirror ws's verbatim (ADR-0043 §5
    // kind-private config keys):
    //  - `peer_named` (VALUE u8, nonzero = true; default false) exposes the bus_link_t
    //    facet — each inbound peer gets its own return-route identity (board↔board).
    //  - `max_peers` (VALUE u32; default 0 = unbounded, host-bounded per RFC-0006) is
    //    the concurrent-peer admission cap.
    vertex.register_transport_type("tcp", [rx_backend](const conn_settings_t& s,
                                                       const wire::tlv_t* raw_config) {
        const config_reader_t cfg(raw_config);
        const bool peer_named = cfg.flag("peer_named").value_or(false);
        const auto max_peers = static_cast<std::size_t>(cfg.u32("max_peers").value_or(0));
        return dial_or_listen(
            s,
            [&] { return make_checked<tcp_transport_t>(s.addr, s.port, rx_backend, s.max_frame); },
            [&] {
                return make_checked<transport_tcp_server>(s.port, rx_backend, s.max_frame,
                                                          max_peers, peer_named);
            });
    });
}

}  // namespace tr::net
