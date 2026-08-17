/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Internal glue — the built-in socket transport-factory registrations, split out of
 * transport_vertex.cpp so a build compiles ONLY the transports it selected (the
 * per-module CMake options in core/CMakeLists.txt). Each register_*_transport lives in
 * its own translation unit (built only when that transport's option is ON) and
 * references exactly one concrete transport type; register_builtin_transports() calls
 * only the enabled ones — hand-written for a full node (src/builtin_transports.cpp),
 * CMake-generated for a partial build (src/builtin_transports.cpp.in). Dropping a
 * transport therefore leaves neither a compiled factory nor a dangling call to it.
 *
 * NO preprocessor macro selects a module: selection is which TUs get compiled (the
 * project's no-feature-macro doctrine, cf. socketcan_link.cpp vs. its stub).
 *
 * Integration glue, not part of the modelling surface: it is deliberately absent from
 * the `%tracer.hpp` umbrella, so a translation unit that wants the built-in catalog
 * includes it by name. It IS scanned by core/Doxyfile and rendered on the transport
 * page — `register_builtin_transports` is the call an integrator makes.
 */
#pragma once

#include <memory>
#include <utility>

#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_vertex.hpp"

namespace tr::net {

/**
 * @brief Construct concrete transport `T`, mapping a failed `ok()` to `TRANSPORT_DOWN`.
 *
 * The dial/bind/handshake-failure → `TRANSPORT_DOWN` mapping the built-in udp/tcp/ws
 * factories share, in one locus (`ok()` is a concrete, non-virtual method, so the check is
 * templated on `T`, not called through `transport_t`). This is exactly the came-up
 * predicate `ok()` is defined as (#1059): asked once, right after construction; the
 * runtime liveness of a link this gate admitted is `transport_t::link_up()`.
 *
 * It was `NOT_FOUND` until #929, which sent a refused connect out as
 * `tr::path::not_found` — a PERMANENT "that address does not exist", when the address the
 * SPEC named resolved fine and it was the LINK that did not come up. A peer reading the
 * registry disposition off the code stopped retrying a link that would have come back.
 */
template <class T, class... Args>
[[nodiscard]] graph::result_t<std::unique_ptr<transport_t>> make_checked(Args&&... args) {
    auto t = std::make_unique<T>(std::forward<Args>(args)...);
    if (!t->ok()) return std::unexpected(graph::status_t::TRANSPORT_DOWN);
    return t;  // unique_ptr<T> => unique_ptr<transport_t> (upcast move)
}

/**
 * @brief The role dispatch every built-in socket factory repeats.
 *
 * DIAL requires `addr` + a nonzero `port` and constructs the dialer; LISTEN requires the
 * `port` KEY and constructs the listener (`TYPE_MISMATCH` on a missing field). The
 * per-transport construction stays in the @p dial / @p listen thunks.
 *
 * A LISTEN may now spell `port = 0` (#1362): the key is present, so nothing is missing, and
 * `0` is what every listener constructor in this tree already documents as EPHEMERAL — the
 * OS picks a free port and `local_port()` reports it. Before this the LISTEN arm rejected
 * `0` as if it were an omitted key, which conflated "you forgot the port" with "you do not
 * care which port", and left an in-band-created listener no way to ask for one. A DIAL still
 * refuses `0`: there is no such thing as dialling the ephemeral port.
 */
template <class Dial, class Listen>
[[nodiscard]] graph::result_t<std::unique_ptr<transport_t>> dial_or_listen(const conn_settings_t& s,
                                                                           Dial&& dial,
                                                                           Listen&& listen) {
    if (s.role == conn_role_t::DIAL) {
        if (s.addr.empty() || s.port == 0) return std::unexpected(graph::status_t::TYPE_MISMATCH);
        return dial();
    }
    if (!s.port_set) return std::unexpected(graph::status_t::TYPE_MISMATCH);
    return listen();
}

/**
 * @brief Wire @p egress_src into a just-constructed link — the EGRESS half of a factory's
 *        memory injection (#873 family 1, ADR-0079's net-plane failable store).
 *
 * The `rx_backend` argument every factory already carries is the INGRESS seam (ADR-0042 §2);
 * this is its egress twin, and the two are deliberately separate objects: an inbound frame
 * becomes a refcounted `segment_t` the receiver may keep, while an egress gather is a raw,
 * per-send scratch block whose exhaustion answer is "drop this frame". Applied right after
 * construction and before the link is wired into the router, which is the
 * @ref transport_t::set_egress_source contract ("before frames flow").
 *
 * A failed construction passes through untouched, so a factory can wrap its
 * `dial_or_listen` result in one expression.
 * @param link       The factory's result — forwarded on unchanged.
 * @param egress_src The store to wire; `nullptr` leaves the link on the process default.
 */
[[nodiscard]] inline graph::result_t<std::unique_ptr<transport_t>> with_egress_source(
    graph::result_t<std::unique_ptr<transport_t>> link, mem::block_source_t* egress_src) noexcept {
    if (link.has_value() && egress_src != nullptr) (*link)->set_egress_source(*egress_src);
    return link;
}

/** @brief Register the built-in `udp` transport factory on @p vertex (needs transport_udp).
 *         @p egress_src is the ADR-0079 egress store — see `with_egress_source`. */
void register_udp_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend,
                            mem::block_source_t* egress_src = &mem::heap_source());

/** @brief Register the built-in `tcp` transport factory on @p vertex (needs transport_tcp).
 *         @p egress_src is the ADR-0079 egress store — see `with_egress_source`. */
void register_tcp_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend,
                            mem::block_source_t* egress_src = &mem::heap_source());

/** @brief Register the built-in `ws` transport factory on @p vertex (needs transport_ws).
 *         @p egress_src is the ADR-0079 egress store — see `with_egress_source`. */
void register_ws_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend,
                           mem::block_source_t* egress_src = &mem::heap_source());

/**
 * @brief Register every built-in transport factory compiled into this build.
 *
 * Called once from the @ref transport_vertex_t constructor. The definition is
 * build-specific: src/builtin_transports.cpp provides the full-node form (udp + tcp +
 * ws), while a core build that drops a transport compiles a CMake-generated variant
 * (from src/builtin_transports.cpp.in) that calls only the enabled register_*_transport.
 * @param vertex     The transport vertex to register the catalog entries on.
 * @param rx_backend The ADR-0042 §2 receive-segment seam threaded to owning transports.
 * @param egress_src The ADR-0079 net-plane EGRESS store threaded to every socket these
 *                   factories construct — see `with_egress_source`. Default: the
 *                   process heap (today's behaviour, unchanged).
 */
void register_builtin_transports(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend,
                                 mem::block_source_t* egress_src = &mem::heap_source());

}  // namespace tr::net
