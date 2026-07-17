/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #83 — a transport, and each connection inside it, as a first-class `/` vertex
 * (ADR-0027): connections appear in the graph as `/net/<conn>` vertices —
 * `:children[]`-created, `:settings`-readable, `await`-able for link up/down.
 *
 * The production path is CONFIG-CONSTRUCTED sockets: the SPEC's `config` names a
 * transport `kind` (`udp`, `ws`, or any kind registered via
 * @ref transport_vertex_t::register_transport_type), and the connection vertex
 * constructs and OWNS the real socket transport from its parsed
 * @ref conn_settings_t, wiring it into the router exactly as a hand-supplied link.
 * @ref transport_vertex_t::provide_link remains the test/manual seam (loopback
 * channels, exotic transports) and takes precedence when staged.
 *
 * SOLID / layering: the graph owns the *addressing* (`register_child_type` composes
 * the `/net/<name>` key, #82); this `tr::net` seam owns the *catalog entry* — it
 * parses the connection's transport-private `{addr, port, role, kind}` config and
 * wires the transport into the router. L4 (`graph`) never learns what a `client`
 * or `listener` is.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

class fwd_router_t;

/**
 * @brief Tag selecting the SLIM @ref transport_vertex_t ctor — the one that does
 *        NOT auto-register the built-in udp/tcp/ws transport factories.
 *
 * The default ctor is batteries-included: it registers the built-in socket
 * factories so a full node can create udp/tcp/ws connections from a SPEC `kind`.
 * A slim node binds its links DIRECTLY instead — @ref transport_vertex_t::provide_link stages a
 * hand-constructed transport and a `:children[]` SPEC wires it in (the way the
 * device/VB nodes stage their ws and CAN links) — so it never routes creation
 * through the built-in factories. Yet while the ctor hard-references
 * `register_builtin_transports`, the linker must keep udp+tcp+ws (and their
 * factory glue) even on a node that binds none of them: constructing the `/net`
 * vertex pulls all three in. Passing @ref slim_net_t selects a ctor whose
 * translation-unit graph never names `register_builtin_transports`, so on a
 * `--gc-sections` target the unbound factories — and the transport TUs nothing
 * else references — garbage-collect. A slim node re-adds exactly the factories it
 * wants via @ref transport_vertex_t::register_transport_type (or a hand-picked
 * register_*_transport).
 *
 * NON-BREAKING: the default (full-node) ctor is unchanged, so existing consumers
 * keep the auto-registered builtins; only a node that opts in with this tag sheds
 * them. Compile-time (not a runtime flag) precisely so the reference is absent
 * from the slim TU and the GC can fire.
 */
struct slim_net_t {
    explicit slim_net_t() = default;
};
inline constexpr slim_net_t slim_net{};

/**
 * @brief The connection's transport-private role (ADR-0027 §default link direction).
 *
 * `DIAL` = this node opens the link (the consumer-dials default); `LISTEN` = this node
 * accepts. A config-constructed socket transport acts on it (bind vs. connect).
 */
enum class conn_role_t : std::uint8_t { DIAL = 0, LISTEN = 1 };

/**
 * @brief One connection's transport-private settings — NOT the graph's `settings_t`.
 *
 * `addr`/`port`/`role`/`keepalive_ms`/`kind` are a *device-private* `:settings` facet of
 * a connection vertex (ADR-0021: standard vs device-private fields), so they live here on
 * the `tr::net` leaf record, never polluting the L4 `settings_t` every sensor carries.
 * `kind` selects the transport factory (e.g. `"udp"`, `"ws"`) used to construct the
 * socket when no @ref transport_vertex_t::provide_link was staged; empty = pre-supplied
 * link only.
 *
 * This record carries ONLY the universal keys every transport kind shares (the ADR-0043
 * §5 leanness ruling): a kind's PRIVATE config (e.g. quic's `cert`/`key` PEM paths) never
 * lands here — the kind's own factory parses it from the raw config SETTINGS TLV it
 * receives alongside these settings.
 */
struct conn_settings_t {
    std::string addr;                     /**< @brief Peer IPv4 dotted-quad (DIAL). */
    std::uint16_t port = 0;               /**< @brief Peer port (DIAL) / bind port (LISTEN). */
    conn_role_t role = conn_role_t::DIAL; /**< @brief Link direction (type default; config
                                                      `role` overrides). */
    std::uint32_t keepalive_ms = 0;       /**< @brief Keepalive interval (transport-specific;
                                                      ignored by the built-ins). */
    std::uint32_t max_frame = 0;          /**< @brief Per-connection receive frame cap for the
                                                      length-prefix stream transports (`tcp`,
                                                      `quic`, `webtransport`); 0 = the protocol
                                                      default (`kMaxFrame`, 16 MiB). Only tightens,
                                                      never raises. */
    std::string kind;                     /**< @brief Transport-factory selector ("udp",
                                                      "ws", ...); empty = provide_link only. */
};

/**
 * @brief Groups connection vertices under `/net` and makes each a `/` vertex (ADR-0027).
 *
 * Construct over a live @ref graph::graph_t and `fwd_router_t`. Registers a
 * `client` and `listener` child type on the graph (via the #82 `register_child_type`
 * seam) so an in-band `write /net:children[] += SPEC{type, name, config}` instantiates
 * a connection vertex at `/net/<name>` and wires its transport into the router's
 * @ref child_registry_t — the single NAME→link demux table.
 *
 * The transport for a connection comes from one of two sources, in precedence order:
 *  1. a link staged via @ref provide_link (borrowed; the caller owns it) — the
 *     test/manual seam for loopback channels and transports the catalog doesn't cover;
 *  2. otherwise, the transport-factory catalog: the config's `kind` selects a factory
 *     (built-in `udp`/`tcp`/`ws`, or any registered via @ref register_transport_type), which
 *     CONSTRUCTS the real socket from the parsed @ref conn_settings_t; the connection
 *     vertex OWNS it, and its link state is written up on successful construction.
 *
 * Destruction semantics (honest): there is no child-removal / connection-teardown
 * model yet (#66), so an owned transport lives as long as this `transport_vertex_t` —
 * its recv thread is joined when this object destructs. Declare the
 * `transport_vertex_t` AFTER the graph and router it binds (the usual stack order), so
 * owned transports stop delivering frames before the router they feed is gone.
 */
class transport_vertex_t {
   public:
    /**
     * @brief Constructs an owning transport from a connection's parsed settings plus
     *        the raw config TLV.
     *
     * The shared @ref conn_settings_t carries ONLY the universal keys (the ADR-0043 §5
     * leanness ruling); @p raw_config is the SPEC's config SETTINGS TLV as written (may
     * be null when the SPEC carried none), from which a kind's factory parses its own
     * kind-private keys (e.g. quic's `cert`/`key`) — the factory's business, module-side.
     *
     * Returns the live transport, or a status: `TYPE_MISMATCH` for a config missing
     * the fields the kind requires (e.g. a DIAL without `addr`/`port`), `NOT_FOUND`
     * for a socket that failed to come up (bind/dial/handshake failure).
     */
    using transport_factory_t = std::function<graph::result_t<std::unique_ptr<transport_t>>(
        const conn_settings_t&, const wire::tlv_t* raw_config)>;

    /**
     * @brief Bind to @p graph and @p router and register the `client`/`listener` catalog
     *        types under the `/net` parent (which is registered if absent).
     *
     * Also registers the built-in transport factories: `udp` (DIAL: bind an ephemeral
     * port, peer = `addr:port`; LISTEN: bind `port`, peer learned from inbound
     * datagrams) and `ws` (DIAL: `transport_ws_client(addr, port)` — a synchronous
     * connect + RFC 6455 handshake at creation time; LISTEN: `transport_ws_server(port)`
     * — accepts MANY concurrent inbound peers (#362), with the ws-private `peer_named` /
     * `max_peers` config keys selecting the ADR-0044 bus facet and the admission cap).
     * @param net_root   The parent path for connection vertices (default "/net").
     * @param rx_backend The RX memory seam config-constructed view-delivering
     *                   transports draw their inbound frame segments from
     *                   (ADR-0042 §2): the built-in `udp` factory passes it to
     *                   every socket it constructs, so a `:children[]`-created
     *                   connection participates in owning delivery. Default: the
     *                   process heap; a bounded host injects its pool over its
     *                   static slab. Must outlive this object (and thus every
     *                   owned transport).
     */
    transport_vertex_t(graph::graph_t& graph, fwd_router_t& router, std::string net_root = "/net",
                       mem::mem_backend_t* rx_backend = &mem::heap_backend());

    /**
     * @brief SLIM ctor (@ref slim_net_t): bind to @p graph / @p router and register
     *        the `client`/`listener` catalog types under `/net`, but DO NOT
     *        auto-register the built-in udp/tcp/ws factories.
     *
     * Identical to the default ctor except it omits the `register_builtin_transports`
     * call, so a node that binds its links directly (@ref provide_link) sheds the
     * unused socket transports on a `--gc-sections` target (see @ref slim_net_t). The
     * composition root registers whatever factories it does want afterward via
     * @ref register_transport_type. All arguments are required (the tag is last, so
     * there is nothing to default against).
     */
    transport_vertex_t(graph::graph_t& graph, fwd_router_t& router, std::string net_root,
                       mem::mem_backend_t* rx_backend, slim_net_t);

    transport_vertex_t(const transport_vertex_t&) = delete;
    transport_vertex_t& operator=(const transport_vertex_t&) = delete;

    /**
     * @brief Register (or replace) the transport factory for config `kind` @p kind.
     *
     * Mirrors the graph's child-type catalog (#82): a subsequent `:children[]` SPEC
     * whose config carries `kind = <kind>` constructs its transport via @p factory. An
     * unregistered kind fails creation with `SCHEMA_NOT_FOUND` (the same "unsupported
     * catalog entry" convention as an unknown SPEC `type`). Call at setup, before
     * frames flow (mirrors `register_child_type`'s thread contract).
     * @param kind    The config `kind` selector (e.g. "udp", "quic").
     * @param factory Builds an owning transport from the parsed universal settings
     *                plus the raw config TLV (for its kind-private keys).
     */
    void register_transport_type(std::string kind, transport_factory_t factory);

    /**
     * @brief Supply a pre-built transport a subsequent SPEC of connection @p name binds.
     *
     * The test/manual seam: the link is not constructed from the config — it is handed
     * in here (a loopback endpoint, a test channel, a transport the catalog doesn't
     * cover) and wired into the router when the matching `:children[]` SPEC is created.
     * Takes precedence over config construction (`kind` is ignored when a link is
     * staged). The caller keeps ownership. Call at setup, before the SPEC write.
     * @param name The connection's NAME (the `/net/<name>` segment / the router child name).
     * @param link The transport carrying this connection's bytes.
     */
    void provide_link(std::string name, transport_t& link);

    /**
     * @brief Report a connection's link up/down — a write to the vertex value.
     *
     * Writing the 1-byte link state makes `await(/net/<name>)` fire (ADR-0021: `await`
     * is the vertex's `poll`). Config-constructed transports are written up at creation;
     * this remains the seam for later link events (and the only source for provided links).
     * @param name The connection NAME.
     * @param up   True = link up (`0x01`), false = down (`0x00`).
     * @return NotFound if @p name names no created connection vertex.
     */
    [[nodiscard]] graph::result_t<void> set_link_state(std::string_view name, bool up);

    /** @brief The parsed transport-private settings of connection @p name (nullptr if none). */
    [[nodiscard]] const conn_settings_t* settings_of(std::string_view name) const;

    /**
     * @brief The OWNED transport of connection @p name — the config-constructed socket.
     *
     * The seam for reaching a SPEC-constructed listener/server after creation (e.g.
     * to enumerate its peers via `link_of(name)->bus()` or close one via
     * `link_of(name)->bus()->close_peer(peer)`). Returns nullptr for a connection
     * whose link was staged with @ref provide_link (the caller already owns that
     * link) and for an unknown NAME.
     * @param name The connection's NAME (the `/net/<name>` segment).
     */
    [[nodiscard]] transport_t* link_of(std::string_view name) const;

   private:
    // One connection leaf: the graph identity vertex, its transport-private config, and —
    // when config-constructed — the OWNED transport (`owned` empty for a provided link).
    // The NAME→link routing table is NOT duplicated here — it has one owner, the router's
    // child_registry_t (Brick 3a); make_connection registers the link there.
    struct conn_t {
        graph::vertex_handle_t vertex;  // the /net/<name> identity vertex (set on creation)
        conn_settings_t settings;
        std::unique_ptr<transport_t> owned;  // config-constructed socket (see class docs)
    };

    // The catalog factory shared by the `client`/`listener` types: parse the SPEC config,
    // resolve the link (provided > config-constructed), record the leaf, wire the link
    // into the router. `role` distinguishes the two types (a config `role` overrides).
    graph::result_t<graph::vertex_handle_t> make_connection(std::vector<std::byte> child_key,
                                                            const wire::tlv_t* config,
                                                            conn_role_t role);

    graph::graph_t& graph_;
    fwd_router_t& router_;
    std::string net_root_;
    mem::mem_backend_t* rx_backend_;  // RX segment source for owned view-delivering sockets
    // Pre-supplied links awaiting their SPEC, and created connections, both by NAME;
    // the transport-factory catalog by config `kind`.
    std::map<std::string, transport_t*, std::less<>> pending_links_;
    std::map<std::string, conn_t, std::less<>> conns_;
    std::map<std::string, transport_factory_t, std::less<>> transport_types_;
};

}  // namespace tr::net
