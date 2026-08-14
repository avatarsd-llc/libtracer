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
#include <cstddef>
#include <utility>

#include "libtracer/builtin_transports.hpp"
#include "libtracer/config_reader.hpp"
#include "libtracer/transport_tcp.hpp"
#include "libtracer/transport_vertex.hpp"

namespace tr::net {

void register_tcp_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend,
                            mem::block_source_t* egress_src) {
    // Built-in `tcp`: DIAL = tcp_transport_t(addr, port) — a SYNCHRONOUS TCP connect at
    // creation time (the peer's listener must be up, or the SPEC write fails TRANSPORT_DOWN);
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
    // A third tcp-private key, honored on BOTH halves and mirroring ws's verbatim (#838):
    //  - `liveness_window` (VALUE u32, ms; default 0 = kDefaultLivenessWindowMs) — how long
    //    a peer may fail to take bytes before it is broken. It is the send bound's
    //    provenance: a host has no task watchdog to derive one from the way the MCU link
    //    does (#835), so the number comes from the deployer, exactly as `connect_timeout`
    //    and CAN's `peer_ttl` (ADR-0044) do.
    // tcp has both a dial and a listen shape, so it is TWO modules (RFC-0014 §1) — but the
    // library registers NEITHER (ADR-0073 §4): the application declares each module under a
    // name IT chooses via register_module (kTcpClientSuggestedModule /
    // kTcpServerSuggestedModule in transport_tcp.hpp are the suggested defaults).
    vertex.register_transport_type(
        "tcp", [rx_backend, egress_src](const conn_settings_t& s, const wire::tlv_t* raw_config) {
            const config_reader_t cfg(raw_config);
            const bool peer_named = cfg.flag("peer_named").value_or(false);
            const auto max_peers = static_cast<std::size_t>(cfg.u32("max_peers").value_or(0));
            const std::uint32_t liveness_window = cfg.u32("liveness_window").value_or(0);
            auto link = dial_or_listen(
                s,
                [&] {
                    // DEFERRED recv thread (#1045, the `ws` factory's shape): the connection is
                    // dialled here, but `transport_vertex_t::make_connection` only wires the
                    // receiver a few steps later (register the vertex, then
                    // `fwd_router_t::add_child`). A peer that pushes the instant our connect
                    // completes has that frame in flight through that whole window, and a recv
                    // thread running inside it would decode it into an empty sink and drop it
                    // with no counter moving. The vertex calls `start_receiving()` once the link
                    // is fully wired.
                    return make_checked<tcp_transport_t>(s.addr, s.port, rx_backend, s.max_frame,
                                                         /*recv_stack=*/std::size_t{0},
                                                         /*defer_recv=*/true, liveness_window);
                },
                [&] {
                    return make_checked<transport_tcp_server>(
                        s.port, rx_backend, s.max_frame, max_peers, peer_named,
                        /*recv_stack=*/std::size_t{0}, liveness_window);
                });
            // The ADR-0079 egress store, wired before the link is handed to the router (#873).
            return with_egress_source(std::move(link), egress_src);
        });
}

}  // namespace tr::net
