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
 * This is NOT a public API: it is not part of the tracer.hpp umbrella and is not
 * scanned by core/Doxyfile.
 */
#pragma once

#include <memory>
#include <utility>

#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/transport.hpp"
#include "libtracer/transport_vertex.hpp"

namespace tr::net {

/**
 * @brief Construct concrete transport `T`, mapping a failed `ok()` to `NOT_FOUND`.
 *
 * The dial/bind/handshake-failure → `NOT_FOUND` mapping every built-in stream factory
 * shares, in one locus (`ok()` is a concrete, non-virtual method, so the check is
 * templated on `T`, not called through `transport_t`).
 */
template <class T, class... Args>
[[nodiscard]] graph::result_t<std::unique_ptr<transport_t>> make_checked(Args&&... args) {
    auto t = std::make_unique<T>(std::forward<Args>(args)...);
    if (!t->ok()) return std::unexpected(graph::status_t::NOT_FOUND);
    return t;  // unique_ptr<T> => unique_ptr<transport_t> (upcast move)
}

/**
 * @brief The role dispatch every built-in socket factory repeats.
 *
 * DIAL requires `addr` + `port` and constructs the dialer; LISTEN requires `port` and
 * constructs the listener (`TYPE_MISMATCH` on a missing field). The per-transport
 * construction stays in the @p dial / @p listen thunks.
 */
template <class Dial, class Listen>
[[nodiscard]] graph::result_t<std::unique_ptr<transport_t>> dial_or_listen(const conn_settings_t& s,
                                                                           Dial&& dial,
                                                                           Listen&& listen) {
    if (s.role == conn_role_t::DIAL) {
        if (s.addr.empty() || s.port == 0) return std::unexpected(graph::status_t::TYPE_MISMATCH);
        return dial();
    }
    if (s.port == 0) return std::unexpected(graph::status_t::TYPE_MISMATCH);
    return listen();
}

/** @brief Register the built-in `udp` transport factory on @p vertex (needs transport_udp). */
void register_udp_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend);

/** @brief Register the built-in `tcp` transport factory on @p vertex (needs transport_tcp). */
void register_tcp_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend);

/** @brief Register the built-in `ws` transport factory on @p vertex (needs transport_ws). */
void register_ws_transport(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend);

/**
 * @brief Register every built-in transport factory compiled into this build.
 *
 * Called once from the @ref transport_vertex_t constructor. The definition is
 * build-specific: src/builtin_transports.cpp provides the full-node form (udp + tcp +
 * ws), while a core build that drops a transport compiles a CMake-generated variant
 * (from src/builtin_transports.cpp.in) that calls only the enabled register_*_transport.
 * @param vertex     The transport vertex to register the catalog entries on.
 * @param rx_backend The ADR-0042 §2 receive-segment seam threaded to owning transports.
 */
void register_builtin_transports(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend);

}  // namespace tr::net
