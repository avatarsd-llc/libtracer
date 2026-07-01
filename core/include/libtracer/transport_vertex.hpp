/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #83 Stage-1 — a transport, and each connection inside it, as a first-class `/`
 * vertex (ADR-0027). This is the SHELL over the live path (ADR-0037 Stage-1):
 * transports/connections appear in the graph as `/net/<conn>` vertices —
 * `:children[]`-created, `:settings`-readable, `await`-able for link up/down —
 * WHILE `fwd_router_t` still carries the bytes. It proves the vertex/compositor
 * model with zero regression to the TSan-clean forward path; the dissolution of
 * `fwd_router_t::children_` into `graph.find` is the Stage-2 flip (ADR-0037/0038).
 *
 * SOLID / layering: the graph owns the *addressing* (`register_child_type` composes
 * the `/net/<name>` key, #82); this `tr::net` seam owns the *catalog entry* — it
 * parses the connection's transport-private `{addr, port, role}` config and wires the
 * supplied `transport_t&` into the router. L4 (`graph`) never learns what a `client`
 * or `listener` is. (A) supplied/loopback transport: no real socket is constructed
 * from `addr`/`port` here — that per-transport construction is a follow-on that plugs
 * into this same catalog seam.
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

class fwd_router_t;

/**
 * @brief The connection's transport-private role (ADR-0027 §default link direction).
 *
 * `DIAL` = this node opens the link (the consumer-dials default); `LISTEN` = this node
 * accepts. Stage-1 records it as connection state; a real socket transport acts on it.
 */
enum class conn_role_t : std::uint8_t { DIAL = 0, LISTEN = 1 };

/**
 * @brief One connection's transport-private settings — NOT the graph's `settings_t`.
 *
 * `addr`/`port`/`role`/`keepalive_ms` are a *device-private* `:settings` facet of a
 * connection vertex (ADR-0021: standard vs device-private fields), so they live here on
 * the `tr::net` leaf record, never polluting the L4 `settings_t` every sensor carries.
 */
struct conn_settings_t {
    std::string addr;
    std::uint16_t port = 0;
    conn_role_t role = conn_role_t::DIAL;
    std::uint32_t keepalive_ms = 0;
};

/**
 * @brief Groups connection vertices under `/net` and makes each a `/` vertex (ADR-0027).
 *
 * Construct over a live @ref graph::graph_t and @ref fwd_router_t. Registers a
 * `client` and `listener` child type on the graph (via the #82 `register_child_type`
 * seam) so an in-band `write /net:children[] += SPEC{type, name, config}` instantiates
 * a connection vertex at `/net/<name>` — and, Stage-1, wires the pre-built transport
 * for that connection into the router so bytes still flow the tested path.
 *
 * Because Stage-1 does not construct sockets, the pre-built `transport_t&` for a
 * connection is supplied via @ref provide_link before the SPEC that names it is written
 * (setup-time, mirroring how the router's children are wired today). This is the (A)
 * shell: the *model* is proven in-band; real per-transport socket construction is the
 * follow-on that replaces @ref provide_link with construction from @ref conn_settings_t.
 */
class transport_vertex_t {
   public:
    /**
     * @brief Bind to @p graph and @p router and register the `client`/`listener` catalog
     *        types under the `/net` parent (which is registered if absent).
     * @param net_root The parent path for connection vertices (default "/net").
     */
    transport_vertex_t(graph::graph_t& graph, fwd_router_t& router, std::string net_root = "/net");

    transport_vertex_t(const transport_vertex_t&) = delete;
    transport_vertex_t& operator=(const transport_vertex_t&) = delete;

    /**
     * @brief Supply the pre-built transport a subsequent SPEC of connection @p name binds.
     *
     * Stage-1 (A): the link is not constructed from `addr`/`port` — it is handed in here
     * (a loopback endpoint, a test channel) and wired into the router when the matching
     * `:children[]` SPEC is created. Call at setup, before the SPEC write.
     * @param name The connection's NAME (the `/net/<name>` segment / the router child name).
     * @param link The transport carrying this connection's bytes.
     */
    void provide_link(std::string name, transport_t& link);

    /**
     * @brief Report a connection's link up/down — a write to the vertex value.
     *
     * Writing the 1-byte link state makes `await(/net/<name>)` fire (ADR-0021: `await`
     * is the vertex's `poll`). Stage-1 stand-in for a real transport link event.
     * @param name The connection NAME.
     * @param up   True = link up (`0x01`), false = down (`0x00`).
     * @return NotFound if @p name names no created connection vertex.
     */
    [[nodiscard]] graph::result_t<void> set_link_state(std::string_view name, bool up);

    /** @brief The parsed transport-private settings of connection @p name (nullptr if none). */
    [[nodiscard]] const conn_settings_t* settings_of(std::string_view name) const;

   private:
    // One connection leaf: the graph identity vertex + its transport-private config. The
    // NAME→link routing table is NOT duplicated here — it has one owner, the router's
    // child_registry_t (Brick 3a); make_connection registers the link there.
    struct conn_t {
        graph::vertex_t* vertex = nullptr;  // the /net/<name> identity vertex
        conn_settings_t settings;
    };

    // The catalog factory shared by the `client`/`listener` types: parse the SPEC config,
    // record the leaf, wire the supplied link into the router. `role` distinguishes them.
    graph::result_t<graph::vertex_t*> make_connection(std::vector<std::byte> child_key,
                                                      const wire::tlv_t* config, conn_role_t role);

    graph::graph_t& graph_;
    fwd_router_t& router_;
    std::string net_root_;
    // Pre-supplied links awaiting their SPEC, and created connections, both by NAME.
    std::map<std::string, transport_t*, std::less<>> pending_links_;
    std::map<std::string, conn_t, std::less<>> conns_;
};

}  // namespace tr::net
