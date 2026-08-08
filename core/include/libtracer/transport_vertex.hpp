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
#include <mutex>
#include <set>
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
 * @brief The connection vertex's link-liveness value (RFC-0014 §4).
 *
 * The 1-byte VALUE a connection vertex stores — `await`-able and subscribable, so a
 * `subscribe /net/<module>/<name>` streams every transition (assign-then-deliver under
 * RFC-0008 §D). Supersedes the binary up/down `set_link_state(name, bool)`. The six
 * states are RFC-0014 §4's table, in table order; the byte encoding becomes normative
 * on the S7 conformance-vector merge (the RFC defers it, so these values are the
 * reference encoding until then). `DORMANT` keeps the old "down" `0x00` so a resting
 * link stays the falsy default.
 *
 * DIAL links move through `DORMANT`/`DIALING`/`RECONNECTING`/`UP`; LISTEN links report
 * listen-socket reachability as `LISTENING`/`BIND_FAILED` (never per-accepted-peer). The
 * liveness *engine* that drives these automatically is RFC-0014 S5 — for now the value is
 * set manually (config-constructed sockets report `UP`/`LISTENING` at creation; provided
 * links report via @ref transport_vertex_t::set_link_state).
 */
enum class link_state_t : std::uint8_t {
    DORMANT = 0,      /**< @brief DIAL: vertex exists; no socket (refcount 0). */
    DIALING = 1,      /**< @brief DIAL: a connect attempt is in flight. */
    RECONNECTING = 2, /**< @brief DIAL: retrying toward `UP` between backoff waits. */
    UP = 3,           /**< @brief DIAL: socket connected, bidirectional. */
    LISTENING = 4,    /**< @brief LISTEN: listen socket bound and accepting. */
    BIND_FAILED = 5,  /**< @brief LISTEN: the listen socket could not bind. */
};

/**
 * @brief One connection's transport-private settings — a `tr::net` record, not part of
 *        any vertex's protocol `:settings` surface.
 *
 * `addr`/`port`/`role`/`keepalive_ms`/`kind` are a *device-private* `:settings` facet of
 * a connection vertex (ADR-0021: standard vs device-private fields), so they live here on
 * the `tr::net` leaf record. They are reached through this transport's own config door,
 * never through the vertex `:settings` core namespace — which RFC-0022 §3.B emptied
 * outright, so there is no shared per-vertex policy record for these to be confused with
 * or to leak into.
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
    std::uint32_t max_frame = 0;          /**< @brief Per-connection receive frame cap for every
                                                      framed transport — the length-prefix streams
                                                      (`tcp`, `quic`, `webtransport`) read it off
                                                      their u32 prefix, `ws` off the RFC 6455 frame
                                                      header; 0 = the protocol default (`kMaxFrame`,
                                                      16 MiB). Only tightens, never raises. */
    std::string kind;                     /**< @brief Transport-factory selector ("udp",
                                                      "ws", ...); empty = provide_link only. */
    std::uint32_t backoff_ms = 0;         /**< @brief DIAL self-heal retry interval (RFC-0014 §4);
                                                      consumed by the S5 liveness engine, 0 = the
                                                      engine's default. Parsed but dormant until S5. */
    std::uint32_t connect_timeout_ms = 0; /**< @brief DIAL connect-attempt deadline (RFC-0014 §4):
                                                      how long one dial waits for `UP` before it
                                                      counts as failed. 0 = the engine's default.
                                                      Parsed but dormant until S5. */
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
 * Creation resolves the MODULE first, then the transport — because the mount, the routing
 * key and the staging key are all `<module>/<name>`, and a NAME alone names none of them
 * (#883). A config `kind` decides the module, through the (kind, role) declaration
 * @ref register_module minted; a kind-less SPEC — the @ref provide_link spelling — takes it
 * from the staged set, and only when exactly one staging carries that leaf NAME (two do and
 * the SPEC carries no `kind` to tell them apart ⇒ creation is refused `TYPE_MISMATCH`,
 * rather than one of them being picked by map order).
 *
 * The transport then comes from one of two sources, in precedence order **within that
 * module**:
 *  1. a link staged via @ref provide_link under exactly this `<module>/<name>` (borrowed;
 *     the caller owns it) — the test/manual seam for loopback channels and transports the
 *     catalog doesn't cover;
 *  2. otherwise, the transport-factory catalog: the config's `kind` selects a factory
 *     (built-in `udp`/`tcp`/`ws`, or any registered via @ref register_transport_type), which
 *     CONSTRUCTS the real socket from the parsed @ref conn_settings_t; the connection
 *     vertex OWNS it, and its link state is written up on successful construction.
 *
 * A staging under some OTHER module that happens to share the leaf NAME is a different
 * connection: it is neither used nor consumed here.
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
     * the fields the kind requires (e.g. a DIAL without `addr`/`port`),
     * `TRANSPORT_DOWN` for a socket that failed to come up (bind/dial/handshake
     * failure).
     *
     * The did-not-come-up status is `TRANSPORT_DOWN`, not `NOT_FOUND` (#929), and a
     * factory written outside the library owes the same answer: the address the SPEC
     * named RESOLVED — the failure is the LINK — and `NOT_FOUND` goes out as
     * `tr::path::not_found`, which the RFC-0002 registry marks PERMANENT, telling a
     * peer to stop retrying a link that may well come back. `TRANSPORT_DOWN` carries
     * the TRANSIENT disposition the condition actually has.
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
     * @brief Declare the MODULE that connections of @p kind and @p role mount under.
     *
     * A creatable *(transport, role)* pair is a self-contained module under the net root
     * (RFC-0014 §1), so a connection vertex lives at `<net_root>/<module>/<name>` and its
     * routing key is that same path (ADR-0061). Modules are **declared-only, by the
     * application** (ADR-0073 §4): the library never derives or auto-registers a module
     * name, so every module segment in the graph traces to an application decision. The
     * mapping is not uniform either: a transport with both a dial and a listen shape is TWO
     * modules (`ws-client`, `ws-server`), while a bus like `can` is ONE for both roles — "a
     * bus has no dial/listen asymmetry" (`%transport_can.hpp`).
     *
     * Built-in transports ship *suggested* module names (e.g. `kWsClientSuggestedModule` in
     * `%transport_ws.hpp`) the application may pass here — but the call is always
     * application code. Registration is a minting boundary, so the shared segment-validity
     * predicate (ADR-0073 §1) gates @p module: a name carrying a reserved character, empty,
     * or over the segment byte cap answers `INVALID_PATH` and registers nothing.
     * @param module The module segment (e.g. `"ws-client"`, `"can"`); must satisfy
     *               `tr::graph::valid_segment`.
     * @param kind   The config `kind` this module constructs (e.g. `"ws"`).
     * @param role   The role this module fixes positionally.
     * @return `INVALID_PATH` if @p module is not a valid path segment.
     */
    [[nodiscard]] graph::result_t<void> register_module(std::string module, std::string kind,
                                                        conn_role_t role);

    /**
     * @brief The module a connection of @p kind and @p role mounts under (RFC-0014 §1).
     *
     * Declared-only (ADR-0073 §4): a *(kind, role)* pair the application never declared via
     * @ref register_module answers `SCHEMA_NOT_FOUND` — the unsupported-catalog-entry
     * convention an unknown SPEC `type` uses — instead of a library-derived name.
     *
     * @note Thread-safe (#881): this takes the control mutex, so it may run concurrently
     *       with @ref register_module and with wire-driven connection creation. It used to
     *       read the declaration vector with no lock, which a concurrent
     *       @ref register_module could reallocate out from under the walk — the
     *       declare-only-at-setup contract that papered over the gap is WITHDRAWN. The
     *       lock is control-plane; nothing on the forward or delivery path takes it.
     */
    [[nodiscard]] graph::result_t<std::string> module_for(std::string_view kind,
                                                          conn_role_t role) const;

    /**
     * @brief Supply a pre-built transport a subsequent SPEC of connection @p name binds.
     *
     * The test/manual seam: the link is not constructed from the config — it is handed
     * in here (a loopback endpoint, a test channel, a transport the catalog doesn't
     * cover) and wired into the router when the matching `:children[]` SPEC is created.
     * The caller keeps ownership. Call at setup, before the SPEC write.
     *
     * The staging key is `<module>/<name>` in BOTH halves (#883). A creating SPEC reaches
     * this staging when it resolves to the same module — i.e. it carries no `kind` (and no
     * second staging shares @p name), or it carries a `kind` whose @ref register_module
     * declaration for the creation's role names @p module. Then the staged link takes
     * precedence over config construction, and the `kind`'s factory does not run. A `kind`
     * declared under a DIFFERENT module builds its own socket there and leaves this staging
     * untouched — it no longer captures the creation by leaf NAME alone.
     * @param module The module the connection mounts under (`/net/<module>/<name>`). Required
     *               because a staged link may bypass the transport factory, so there is no
     *               `kind` to derive one from — the caller staging the link says where it
     *               mounts (RFC-0014 §1).
     * @param name The connection's NAME (the `/net/<module>/<name>` leaf segment).
     * @param link The transport carrying this connection's bytes.
     */
    void provide_link(std::string module, std::string name, transport_t& link);

    /**
     * @brief Report a connection's link-liveness state — a write to the vertex value.
     *
     * Writing the 1-byte link_state_t makes `await(/net/<name>)` fire (ADR-0021:
     * `await` is the vertex's `poll`) and delivers to every subscriber (RFC-0008 §D
     * assign-then-deliver). Config-constructed transports are set `UP`/`LISTENING` at
     * creation; this remains the seam for later link events (and the only source for
     * provided links). Until the RFC-0014 S5 liveness engine lands, callers drive this
     * manually — the engine will become the sole writer of the DIAL transitions.
     *
     * @note Thread-safe (#881): this takes the control mutex, so the transport thread
     *       reporting a provided link's liveness may run concurrently with the wire-driven
     *       create/remove that inserts into and erases from the connection table. It used
     *       to look the connection up with no lock, racing that map's rebalance and the
     *       erase of the very node it returned. The vertex write happens inside the locked
     *       section, in the order the class documents (this → router → graph → stripe).
     * @param name  The connection's **qualified** key `net/<module>/<name>` (#605).
     * @param state The link-liveness state to publish (see link_state_t).
     * @return NotFound if @p name names no created connection vertex.
     */
    [[nodiscard]] graph::result_t<void> set_link_state(std::string_view name, link_state_t state);

    /**
     * @brief Remove connection @p name — un-route it, retire its vertex, close its socket.
     *
     * The teardown counterpart of creation (#494), in the order the invariants require:
     * `fwd_router_t::remove_child` first (the name stops resolving, so no forward can
     * reach the link), then `graph.retire()` on the identity vertex (RFC-0009 §B.6 —
     * the path re-virginizes), then the owned transport is destroyed (joining its recv
     * thread). A connection whose link was staged via @ref provide_link leaves that
     * borrowed link alone; only the routing entry and the vertex go.
     *
     * This is the owner-internal operation the RFC-0014 `NAME`-write removal dispatch
     * (S2b) will call; it is not itself reachable from the wire.
     * @param name The connection's **qualified** key `net/<module>/<name>` — NOT the bare
     *             connection NAME. RFC-0014 S2a re-keyed `conns_` to the qualified form so
     *             the routing address equals the vertex path; these lookups moved with it
     *             and the doc did not, so a caller following the old wording got a silent
     *             `NOT_FOUND` / `nullptr` (#605).
     * @return NotFound if @p name names no created connection.
     */
    [[nodiscard]] graph::result_t<void> remove_connection(std::string_view name);

    /**
     * @brief The parsed transport-private settings of connection @p name (nullptr if none).
     * @param name The connection's **qualified** key `net/<module>/<name>` (#605).
     */
    [[nodiscard]] const conn_settings_t* settings_of(std::string_view name) const;

    /**
     * @brief The OWNED transport of connection @p name — the config-constructed socket.
     *
     * The seam for reaching a SPEC-constructed listener/server after creation (e.g.
     * to enumerate its peers via `link_of(name)->bus()` or close one via
     * `link_of(name)->bus()->close_peer(peer)`). Returns nullptr for a connection
     * whose link was staged with @ref provide_link (the caller already owns that
     * link) and for an unknown NAME.
     * @param name The connection's **qualified** key `net/<module>/<name>` — NOT the bare
     *             connection NAME. RFC-0014 S2a re-keyed `conns_` to the qualified form so
     *             the routing address equals the vertex path; these lookups moved with it
     *             and the doc did not, so a caller following the old wording got a silent
     *             `NOT_FOUND` / `nullptr` (#605).
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

    /**
     * @brief `module_for`'s body, for a caller that ALREADY holds `ctl_m_`.
     *
     * `make_connection` resolves a module from inside its own locked section, and `ctl_m_`
     * is a plain, NON-RECURSIVE `std::mutex` — so it cannot reach the public entry, which
     * would self-deadlock. That constraint is why #881 is a split rather than a lock added
     * in place: one body, two surfaces, exactly one acquisition per call.
     */
    [[nodiscard]] graph::result_t<std::string> module_for_locked(std::string_view kind,
                                                                 conn_role_t role) const;

    /**
     * @brief `set_link_state`'s body, for a caller that ALREADY holds `ctl_m_`.
     *
     * Creation publishes `UP`/`LISTENING` for a config-constructed socket from inside the
     * locked section and calls this; every external liveness report goes through the
     * locking public entry. Same non-recursive-mutex constraint as `module_for_locked`.
     */
    [[nodiscard]] graph::result_t<void> set_link_state_locked(std::string_view name,
                                                              link_state_t state);

    /**
     * @brief Serializes every CONTROL-PLANE mutation here (ADR-0063 §3).
     *
     * This class had no synchronization at all, yet `make_connection` writes `conns_` and
     * `pending_links_` while `settings_of` / `link_of` / `remove_connection` traverse `conns_`
     * — and the graph invokes the connection factory OUTSIDE `map_mutex_`, on whichever
     * transport's receive thread delivered the CREATE. Two transports means two such threads,
     * so concurrent `std::map` inserts (and the readers racing their rebalance) were reachable
     * on any ordinary multi-transport node.
     *
     * A plain mutex is deliberate. The guarded section constructs sockets and can block for
     * milliseconds, which rules out an interrupt-disable critical section outright and makes a
     * spinlock a priority-inversion hazard on single-core FreeRTOS (ADR-0063 erratum 1).
     *
     * Lock order: this → `fwd_router_t::ctl_m_` → `graph_t::map_mutex_` → the vertex stripe.
     * Nothing on the forward or delivery path takes any of them.
     */
    mutable std::mutex ctl_m_;

    graph::graph_t& graph_;
    fwd_router_t& router_;
    std::string net_root_;
    mem::mem_backend_t* rx_backend_;  // RX segment source for owned view-delivering sockets
    // Pre-supplied links awaiting their SPEC, and created connections, both by NAME;
    // the transport-factory catalog by config `kind`.
    std::map<std::string, transport_t*, std::less<>> pending_links_;
    std::map<std::string, conn_t, std::less<>> conns_;
    std::map<std::string, transport_factory_t, std::less<>> transport_types_;
    // Declared modules — see register_module / module_for (declared-only, ADR-0073 §4).
    // A flat vector, not a map: this is written at setup and read once per
    // connection creation, so an rbtree buys nothing a linear scan over a handful of entries
    // does not — and it costs a whole extra container instantiation in flash. Nothing here is
    // on the forward path.
    struct module_decl_t {
        std::string module;
        std::string kind;
        conn_role_t role;
    };
    std::vector<module_decl_t> modules_;
};

}  // namespace tr::net
