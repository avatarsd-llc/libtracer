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
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_vertex.hpp"

namespace tr::net {

void register_tcp_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend) {
    // Built-in `tcp`: DIAL = tcp_transport_t(addr, port) — a SYNCHRONOUS TCP connect at
    // creation time (the peer's listener must be up, or the SPEC write fails NOT_FOUND);
    // LISTEN = tcp_transport_t(port), accepting ONE inbound peer at a time. Length-prefix
    // framing is internal to the transport. `keepalive` is ignored (TCP's own keepalive
    // is the #66 lifecycle). `rx_backend` is the ADR-0042 §2 receive-segment seam.
    vertex.register_transport_type("tcp", [rx_backend](const conn_settings_t& s,
                                                       const wire::tlv_t* /*raw_config*/) {
        return dial_or_listen(
            s,
            [&] { return make_checked<tcp_transport_t>(s.addr, s.port, rx_backend, s.max_frame); },
            [&] { return make_checked<tcp_transport_t>(s.port, rx_backend, s.max_frame); });
    });
}

}  // namespace tr::net
