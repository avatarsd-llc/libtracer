/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #83 — a transport, and each connection inside it, as a first-class `/` vertex
 * (ADR-0027): connections appear in the graph as `/net/<module>/<conn>` vertices —
 * created by a `SPEC` write to the module's creator endpoint `/net/<module>/conn`
 * (RFC-0014 §2, the ONE wire creation door since S7 retired the `:children[]`
 * spelling), `:settings`-readable, `await`-able for link up/down.
 *
 * The production path is CONFIG-CONSTRUCTED sockets: the SPEC's `config` names a
 * transport `kind` (`udp`, `ws`, or any kind registered via
 * @ref transport_vertex_t::register_transport_type), and the connection vertex
 * constructs and OWNS the real socket transport from its parsed
 * @ref conn_settings_t, wiring it into the router exactly as a hand-supplied link.
 * @ref transport_vertex_t::provide_link remains the test/manual seam (loopback
 * channels, exotic transports) and takes precedence when staged.
 *
 * SOLID / layering: the graph owns the *addressing* and the generic write gate; this
 * `tr::net` seam owns the whole creation semantics behind the creator endpoint — it
 * parses the connection's transport-private `{addr, port, kind}` config, composes the
 * `/net/<module>/<name>` key and wires the transport into the router. L4 (`graph`) never
 * learns what a transport module is; the endpoint is an ordinary `role_t::HANDLER` vertex
 * to it, and the one thing it learns is that a handler may say which ACL right a written
 * payload TYPE costs (RFC-0014 Amendment 2).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "libtracer/graph.hpp"
#include "libtracer/key_view.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_source.hpp"
#include "libtracer/thread_id.hpp"
#include "libtracer/transport.hpp"

namespace tr::net {

class fwd_router_t;
class self_heal_link_t;

/**
 * @brief Tag selecting the SLIM @ref transport_vertex_t ctor — the one that does
 *        NOT auto-register the built-in udp/tcp/ws transport factories.
 *
 * The default ctor is batteries-included: it registers the built-in socket
 * factories so a full node can create udp/tcp/ws connections from a SPEC `kind`.
 * A slim node binds its links DIRECTLY instead — @ref transport_vertex_t::provide_link stages a
 * hand-constructed transport and a creator-endpoint `SPEC` wires it in (the way the
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
 * states are RFC-0014 §4's table, in table order, and the byte encoding is **normative**:
 * the `conn/liveness-enum` conformance vector plus its bound host test pinned it, and
 * RFC-0014 Amendment 4 (S7) promoted the clause out of `proposed pending`. `DORMANT` keeps
 * the old "down" `0x00` so a resting link stays the falsy default.
 *
 * DIAL links move through `DORMANT`/`DIALING`/`RECONNECTING`/`UP`; LISTEN links report
 * listen-socket reachability as `LISTENING`/`BIND_FAILED` (never per-accepted-peer). The
 * DIAL transitions are driven by the RFC-0014 S5 liveness engine
 * (@ref tr::net::self_heal_link_t, #492) for kinds registered with
 * `transport_kind_traits_t::self_heal_dial` — which since #1548 is every built-in
 * point-to-point kind (`udp`, `tcp`, `ws`), so a stock DIAL connection is engine-managed.
 * Everywhere else the value is still set manually (eagerly-constructed sockets — every
 * LISTEN link, every bus kind — report `UP`/`LISTENING` at creation; provided links report
 * via @ref transport_vertex_t::set_link_state).
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
    std::uint16_t port = 0;               /**< @brief Peer port (DIAL) / bind port (LISTEN);
                                                      `0` on a LISTEN is the EPHEMERAL request —
                                                      see @ref port_set. */
    bool port_set = false;                /**< @brief Was a `port` key PRESENT in the config?
                                                      Distinguishes an omitted required key (a
                                                      `TYPE_MISMATCH` config error) from an
                                                      explicit `port = 0`, which on a LISTEN asks
                                                      the OS to pick the bind port (#1362). Read
                                                      the granted port back off the constructed
                                                      link (`local_port()`). */
    conn_role_t role = conn_role_t::DIAL; /**< @brief Link direction. POSITIONAL: set from the
                                                      module's own `register_module` declaration,
                                                      never from the config. The `role` config key
                                                      that once overrode it died with the
                                                      `:children[]` door at RFC-0014 S7. */
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
                                                      consumed by the S5 liveness engine
                                                      (`self_heal_link.hpp`), 0 = the engine's
                                                      default (`kDefaultBackoffMs`). */
    std::uint32_t connect_timeout_ms = 0; /**< @brief DIAL connect-attempt deadline (RFC-0014 §4):
                                                      how long one dial waits for `UP` before it
                                                      counts as failed; consumed by the S5 engine.
                                                      0 = `kDefaultConnectTimeoutMs`. */
};

/**
 * @brief The RESERVED, protocol-owned leaf NAME of a module's CREATOR ENDPOINT —
 *        `<net_root>/<module>/conn` (RFC-0014 §1, S2b).
 *
 * Each module a application declares through @ref transport_vertex_t::register_module gets
 * one endpoint vertex under this name, and a write to it is the connection lifecycle: a
 * `SPEC{ name, config }` CREATES `<net_root>/<module>/<name>`, a `NAME{ <name> }` REMOVES it,
 * and any other payload is refused `TYPE_MISMATCH` (RFC-0014 §2). Both transport and role are
 * POSITIONAL — they are the module — so the SPEC carries neither a `type` nor a `role`.
 *
 * The name is reserved per module in BOTH directions (RFC-0014 §3): a SPEC may not name a
 * connection `conn` (the endpoint vertex already occupies the key), and `NAME{conn}` is
 * refused, so the endpoint cannot be made to destroy itself.
 *
 * It is also **hidden** from `<net_root>/<module>:children[]` (RFC-0014 §3, S4 — the graph's
 * @ref tr::graph::graph_t::hide_from_enumeration seam): that listing returns the module's
 * member CONNECTIONS, and the endpoint is the control that creates them, not one of them.
 * Hidden is not unreachable — `read <module>/conn:schema` is §6's creatability probe, so the
 * endpoint stays addressable precisely because it is unlisted.
 */
inline constexpr std::string_view kConnEndpointName = "conn";

/**
 * @brief Per-kind CAPABILITY declarations a transport factory registers with (RFC-0014 §4,
 *        S5) — properties of the KIND, not of one connection, so they live in the factory
 *        catalog and never on the shared @ref conn_settings_t (the ADR-0043 §5 leanness
 *        ruling protects that record; this struct is the catalog's row, not the SPEC's).
 *
 * The defaults preserve every existing registration: a kind registered through the
 * traits-less overload keeps today's eager-construction behaviour exactly. The built-in
 * point-to-point kinds do NOT take the defaults since #1548 — see
 * `%kBuiltinPointToPointTraits` in `%builtin_transports.hpp` for the row they share and why
 * `self_heal_dial` there is conditioned on the `kSelfHealLinks` build knob.
 */
struct transport_kind_traits_t {
    /**
     * @brief Opt this kind's DIAL connections into the RFC-0014 §4 S5 liveness engine
     *        (@ref tr::net::self_heal_link_t).
     *
     * When set, a DIAL creation constructs NO socket: the vertex is minted `DORMANT` and
     * the engine dials on demand (any op auto-wakes it, bounded by `connect_timeout`),
     * self-heals with `backoff` while a standing binding holds it, and closes the socket
     * back to dormant on the last release. The kind's factory is then run once per dial
     * attempt — it must be re-runnable (every built-in socket factory is, and each states
     * why in its own registration comment). LISTEN connections of the same kind are
     * untouched (RFC-0014 §4: a LISTEN link ignores refcount; it binds eagerly at creation
     * as before).
     *
     * Only for POINT-TO-POINT, connection-oriented kinds: a bus kind (CAN) must keep the
     * default — the engine has no socket at creation, so the router's bus-facet wiring
     * (`bus_of` at add_child) would never see the facet.
     */
    bool self_heal_dial = false;
    /**
     * @brief The kind's delivery capability (`transport_t::delivers_ropes`), declared
     *        statically because the engine must answer it for `fwd_router_t::add_child`
     *        BEFORE any socket exists. Ignored unless @ref self_heal_dial is set.
     */
    bool delivers_ropes = false;
};

/**
 * @brief Groups connection vertices under `/net` and makes each a `/` vertex (ADR-0027).
 *
 * Construct over a live @ref graph::graph_t and `fwd_router_t`.
 *
 * There is exactly ONE wire creation door: the per-module CREATOR ENDPOINT (RFC-0014 §2,
 * S2b). @ref register_module mints `<net_root>/<module>/conn` (the `%kConnEndpointName`
 * constant), and a `SPEC{ name, config }` written there creates
 * `<net_root>/<module>/<name>` and wires its transport into the router's
 * @ref child_registry_t — the single NAME→link demux table — while a `NAME{ <name> }`
 * removes it. The module in the path fixes both the transport and the role, so the SPEC
 * carries neither a `type` nor a `role`.
 *
 * The superseded global `write /net:children[] += SPEC{type = "client"|"listener", …}`
 * spelling is **retired** (RFC-0014 S7, executing the supersession ADR-0059 ruled): the
 * `client`/`listener` catalog types are no longer registered, so that write now answers
 * `SCHEMA_NOT_FOUND` like any other unregistered type. `/net:children[]` remains a
 * read-only ENUMERATION of this plane's modules.
 *
 * The MODULE is therefore known POSITIONALLY, from the endpoint's own path, before any
 * transport is resolved — which is what the mount, the routing key and the staging key all
 * need, since each is `<module>/<name>` and a NAME alone names none of them (#883). A config
 * `kind`, when present, must be one the module declares (@ref register_module); a kind-less
 * SPEC — the @ref provide_link spelling — takes the module's own declared kind, and a module
 * declared for two kinds is refused `TYPE_MISMATCH` rather than resolved by declaration order.
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
     * connect + RFC 6455 handshake, run by the liveness engine on its first DIAL rather
     * than at creation; LISTEN: `transport_ws_server(port)`
     * — accepts MANY concurrent inbound peers (#362), with the ws-private `peer_named` /
     * `max_peers` config keys selecting the ADR-0044 bus facet and the admission cap).
     *
     * All three built-ins are registered with `%kBuiltinPointToPointTraits` (#1548), so
     * their DIAL connections are ENGINE-MANAGED: creation constructs no socket, mints the
     * vertex `DORMANT`, and dials on first use. Their LISTEN halves bind eagerly as before.
     * @param net_root   The parent path for connection vertices (default "/net").
     * @param rx_backend The RX memory seam config-constructed view-delivering
     *                   transports draw their inbound frame segments from
     *                   (ADR-0042 §2): the built-in `udp` factory passes it to
     *                   every socket it constructs, so a creator-endpoint-created
     *                   connection participates in owning delivery. Default: the
     *                   process heap; a bounded host injects its pool over its
     *                   static slab. Must outlive this object (and thus every
     *                   owned transport).
     * @param egress_src The EGRESS twin of @p rx_backend (#873 family 1, ADR-0079's
     *                   net-plane failable store): the `block_source_t` every socket
     *                   these built-in factories construct draws its per-send gather
     *                   block from — the base `transport_t::send(iov)` temporary and the
     *                   `iov_table_t` overflow, both sized by the SENDING peer. Sizing it
     *                   is what bounds this node's egress allocation; exhaustion drops the
     *                   frame and counts it, exactly as it already does. Default: the
     *                   process heap, i.e. today's behaviour unchanged. Must outlive this
     *                   object (and thus every owned transport). A kind's own factory
     *                   registered later via @ref register_transport_type reaches the same
     *                   store through @ref egress_source.
     */
    transport_vertex_t(graph::graph_t& graph, fwd_router_t& router, std::string net_root = "/net",
                       mem::mem_backend_t* rx_backend = &mem::heap_backend(),
                       mem::block_source_t* egress_src = &mem::heap_source());

    /**
     * @brief SLIM ctor (@ref slim_net_t): bind to @p graph / @p router and register
     *        the `client`/`listener` catalog types under `/net`, but DO NOT
     *        auto-register the built-in udp/tcp/ws factories.
     *
     * Identical to the default ctor except it omits the `register_builtin_transports`
     * call, so a node that binds its links directly (@ref provide_link) sheds the
     * unused socket transports on a `--gc-sections` target (see @ref slim_net_t). The
     * composition root registers whatever factories it does want afterward via
     * @ref register_transport_type. Every argument up to the tag is required — the tag
     * keeps its overload-disambiguating position, so the one defaultable argument
     * (@p egress_src) follows it.
     *
     * @param graph      The graph the `/net` subtree is registered on.
     * @param router     The forwarding router connections are wired into.
     * @param net_root   The parent path for connection vertices (e.g. "/net").
     * @param rx_backend The ADR-0042 §2 RX memory seam — as on the default ctor.
     * @param egress_src The ADR-0079 net-plane EGRESS store this vertex REPORTS through
     *                   @ref egress_source (#873). A slim node registers its own factories,
     *                   so nothing here wires it into a link automatically — but the
     *                   accessor is the documented way a factory reaches "this net plane's
     *                   store", and before this parameter existed it answered the process
     *                   heap on a slim node no matter what the composition root had chosen.
     *                   `nullptr` (and the default) means the process heap, i.e. today's
     *                   behaviour unchanged. Must outlive this object.
     */
    transport_vertex_t(graph::graph_t& graph, fwd_router_t& router, std::string net_root,
                       mem::mem_backend_t* rx_backend, slim_net_t,
                       mem::block_source_t* egress_src = &mem::heap_source());

    transport_vertex_t(const transport_vertex_t&) = delete;
    transport_vertex_t& operator=(const transport_vertex_t&) = delete;

    /**
     * @brief Register (or replace) the transport factory for config `kind` @p kind.
     *
     * The transport-factory catalog (the kind selector), not the graph's child-type
     * catalog: a subsequent creator-endpoint SPEC whose config carries `kind = <kind>`
     * constructs its transport via @p factory. An unregistered kind fails creation with
     * `SCHEMA_NOT_FOUND` (the same "unsupported catalog entry" convention the graph's own
     * child-type catalog gives an unknown SPEC `type`). Call at setup, before frames flow
     * (mirrors `%graph::graph_t::register_child_type`'s thread contract).
     * @param kind    The config `kind` selector (e.g. "udp", "quic").
     * @param factory Builds an owning transport from the parsed universal settings
     *                plus the raw config TLV (for its kind-private keys).
     */
    void register_transport_type(std::string kind, transport_factory_t factory);

    /**
     * @brief Register a transport factory WITH its kind capabilities (RFC-0014 §4 S5).
     *
     * The traits overload: identical to the two-argument form, plus the
     * @ref transport_kind_traits_t row that opts the kind's DIAL connections into the
     * S5 liveness engine. The traits-less overload registers `{}` — every existing
     * caller keeps eager construction unchanged.
     * @param kind    The config `kind` selector (e.g. "udp", "quic").
     * @param factory Builds an owning transport from the parsed universal settings plus
     *                the raw config TLV; with `traits.self_heal_dial` set it is re-run
     *                once per dial attempt, off the creation path.
     * @param traits  The kind's capability row — see @ref transport_kind_traits_t.
     */
    void register_transport_type(std::string kind, transport_factory_t factory,
                                 transport_kind_traits_t traits);

    /**
     * @brief A STANDING binding takes its hold on connection @p name's link (RFC-0014 §4).
     *
     * The S5 refcount seam: a standing subscription or `await` routed through the link
     * holds it acquired for its lifetime (the routing-plane callers are S6's wiring; an
     * embedder may drive it directly). While the count is above zero the engine keeps the
     * link's steady-state target `UP` — self-healing on loss with `backoff`, forever —
     * and the last @ref release_link closes the socket back to `DORMANT`. Non-blocking;
     * "bring it up and wait" is `await` on the connection vertex.
     *
     * A connection that is not engine-managed (a provided link, an eagerly-constructed
     * kind, any LISTEN) answers success as a no-op — RFC-0014 §4: a LISTEN link ignores
     * refcount, and a manual link's liveness stays the caller's.
     * @param name The connection's **qualified** key `net/<module>/<name>` (#605).
     * @return NotFound if @p name names no created connection.
     */
    [[nodiscard]] graph::result_t<void> acquire_link(std::string_view name);

    /**
     * @brief The standing binding releases its hold — see @ref acquire_link.
     * @param name The connection's **qualified** key `net/<module>/<name>` (#605).
     * @return NotFound if @p name names no created connection.
     */
    [[nodiscard]] graph::result_t<void> release_link(std::string_view name);

    /**
     * @brief This net plane's EGRESS store — the one the built-in factories wire into every
     *        socket they construct (see the ctor's `egress_src`, #873 / ADR-0079).
     *
     * Exposed so a factory registered through @ref register_transport_type (the out-of-tree
     * kinds — `quic`, `can`, an embedder's own) can hand the SAME store to the link it
     * builds, via `tr::net::with_egress_source`, rather than silently leaving that kind's
     * gather on the process heap while the built-ins are bounded. A deployer fanning the
     * ADR-0079 NARROW shape ignores this and captures its own per-thread source instead.
     */
    [[nodiscard]] mem::block_source_t& egress_source() const noexcept { return *egress_src_; }

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
     *
     * **Declaring a module MINTS its creator endpoint** (RFC-0014 §1: "adding a module adds
     * its creator endpoint and catalog"): this registers the `<net_root>/<module>` grouping
     * vertex and, below it, the write-driven `<net_root>/<module>/conn`
     * endpoint whose `SPEC`/`NAME` writes create and remove the module's connections. Both
     * are idempotent — a second declaration naming the same module (a module serving two
     * kinds) finds them and mints nothing. The endpoint is minted HIDDEN from the module's
     * `:children[]` (RFC-0014 §3, S4) — see `%kConnEndpointName`.
     * @param module The module segment (e.g. `"ws-client"`, `"can"`); must satisfy
     *               `tr::graph::valid_segment`.
     * @param kind   The config `kind` this module constructs (e.g. `"ws"`).
     * @param role   The role this module fixes positionally.
     * @return `INVALID_PATH` if @p module is not a valid path segment; the graph's own
     *         refusal (e.g. `BACKPRESSURE`) if the endpoint could not be registered — in
     *         which case nothing is declared either.
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
     * @brief Is @p key one of THIS net plane's **structural vertices** — the net root, or a
     *        `<net_root>/<module>` segment — rather than a connection or an application
     *        vertex (#1096)?
     *
     * `transport_vertex_t` mints two vertices nobody asked for: the net root (the
     * `:children[]` ENUMERATION root) and, lazily, each `<net_root>/<module>` segment a
     * connection mounts under. Both are registered `role_t::STORED_VALUE` and carry no
     * descriptor table, so an embedder walking @ref graph::graph_t::for_each_vertex sees
     * them as ordinary value vertices someone forgot to describe — byte-identical `:schema`
     * shape to a real leaf, differing only in the NAME. This predicate is how an embedder
     * tells them apart without re-typing the library's own naming rule.
     *
     * The answer is deliberately scoped HERE and nowhere wider. `graph_t` cannot answer it:
     * an application's own structural vertex (a `/zone` holding nothing but children) is
     * indistinguishable from a connection vertex on every graph-visible surface — same
     * visit, same schema shape, same composed-branch read (RFC-0016) — so a graph-level
     * predicate would be inventing an answer. What the LIBRARY minted, the library can
     * report; what the APPLICATION minted stays the application's business (ADR-0010:
     * libtracer is a transport for application data, not a definer of application
     * semantics). See `docs/reference/11` §structural vertices.
     *
     * @note **Name match, not provenance — a documented false positive.** Creation
     *       deduplicates against `graph_.find`, deliberately keeping no per-module minted
     *       set (commit `221ed983` deleted exactly that state). So a vertex an application
     *       registered at `<net_root>` or `<net_root>/<module>` *before* this object got
     *       there answers `true` here: the predicate says "this key names a structural
     *       position of this net plane", not "this object registered this vertex".
     * @note The RFC-0014 per-module creator endpoint `<net_root>/<module>/conn`
     *       (implemented by S2b) is **not** structural and answers `false`:
     *       it is an addressable control surface — a `role_t::HANDLER` vertex that EXECUTES
     *       the `SPEC`/`NAME` writes reaching it — not a grouping segment, and it sits one
     *       level below the deepest structural position anyway.
     * @param key The canonical PATH-payload key `for_each_vertex` hands its callback (no
     *            handle unwrapping needed — the signature matches).
     * @return true iff @p key is `<net_root>`, or `<net_root>/<module>` for a module of this
     *         plane — one declared through @ref register_module, one staged through
     *         @ref provide_link (the kind-less spelling never declares its module), or one
     *         carrying a live connection.
     */
    [[nodiscard]] bool is_structural(wire::key_view_t key) const;

    /**
     * @brief Supply a pre-built transport a subsequent SPEC of connection @p name binds.
     *
     * The test/manual seam: the link is not constructed from the config — it is handed
     * in here (a loopback endpoint, a test channel, a transport the catalog doesn't
     * cover) and wired into the router when the matching creator-endpoint SPEC is created.
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
     * assign-then-deliver). Eagerly-constructed transports are set `UP`/`LISTENING` at
     * creation; this remains the seam for later link events (and the only source for
     * provided links). On an ENGINE-MANAGED connection (a `self_heal_dial` kind's DIAL,
     * RFC-0014 S5) the engine is the sole writer of the DIAL transitions — a manual
     * write here still lands but is advisory-at-best and the next transition overwrites
     * it; drive such a link through @ref acquire_link / @ref release_link instead.
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
    // child_registry_t (Brick 3a); `make_connection_locked` registers the link there.
    struct conn_t {
        graph::vertex_handle_t vertex;  // the /net/<name> identity vertex (set on creation)
        conn_settings_t settings;
        std::unique_ptr<transport_t> owned;  // config-constructed socket (see class docs)
        // The S5 liveness engine, iff this connection is engine-managed (RFC-0014 §4):
        // a non-owning view of `owned` as its concrete type, so teardown can stop the
        // worker BEFORE the vertex retires and acquire/release can reach the refcount.
        self_heal_link_t* engine = nullptr;
    };

    // One declared module: the segment it mounts under, the config `kind` it constructs, and
    // the role it fixes positionally. Declared above the methods that name it because a
    // member function's return type is parsed against the class-so-far.
    struct module_decl_t {
        std::string module;
        std::string kind;
        conn_role_t role;
    };

    /**
     * @brief What a `%ctl_txn_t` is for — which of the two control-plane locks it needs.
     *
     * The distinction IS the S6 discipline (#492). See `ops_m_` and `ctl_m_`.
     */
    enum class ctl_scope_t : std::uint8_t {
        LOOKUP,    /**< @brief The maps only: a reader, or the refcount seam. `ctl_m_`. */
        OPERATION, /**< @brief A whole mutation, phase 1 + phase 2. `ops_m_` then `ctl_m_`. */
    };

    /**
     * @brief The RFC-0014 S6 two-phase control-plane seam (#492): the ONE way either
     *        control-plane lock is taken, and the thing that makes the discharge unskippable.
     *
     * **Phase 1** — construction takes the locks its `%ctl_scope_t` asks for; the decision
     * runs under them and touches only this class's own maps. **Phase 2** — `%discharge`
     * (or, as the backstop, the destructor) drops `ctl_m_` FIRST and only then performs the
     * collected work against the graph, the router and the sockets. Nothing between the two
     * phases can skip the release: there is no hand-written unlock path to forget, and an
     * early `return` out of a decision still discharges.
     *
     * The work collected is exactly the work that can RE-ENTER this class:
     * `%publish` is a `graph_t::write` that fans out to subscribers (a routing-plane
     * subscriber of a connection's own liveness is what S6 wires, and it drives
     * @ref transport_vertex_t::acquire_link — straight back into `ctl_m_`), and
     * `%stop_engine` / `%destroy_link` JOIN a thread that may be inside such a
     * fan-out. Held across `ctl_m_`, either one deadlocks a non-recursive mutex.
     *
     * An `OPERATION` keeps `ops_m_` for the WHOLE of both phases, which is what preserves
     * ADR-0063 §3's serialization promise now that the discharge is outside `ctl_m_`: two
     * mutations cannot interleave, so a deferred publish can no longer land on a vertex a
     * concurrent teardown is retiring. `ops_m_` is safe to hold there precisely because the
     * doors a fan-out reaches never take it.
     *
     * A `LOOKUP` that collects nothing is a plain scoped lock — which is the point: every
     * acquisition of either mutex in this class goes through this type, so the re-entrancy
     * checks in the constructor see them all.
     */
    class ctl_txn_t {
       public:
        /**
         * @brief Phase 1 begins: take the locks @p scope asks for and claim them for this
         *        thread.
         *
         * `const`, because the pure readers (`settings_of` / `link_of` / `module_for` /
         * `is_structural`) take `ctl_m_` too — it is `mutable`, as are `ops_m_` and both
         * ownership stamps. Phase 2 still reaches the graph and the router: those are
         * REFERENCE members, so const on the owner does not propagate to them.
         */
        explicit ctl_txn_t(const transport_vertex_t& owner,
                           ctl_scope_t scope = ctl_scope_t::LOOKUP);

        /** @brief The backstop discharge — see `%discharge`; the status is dropped. */
        ~ctl_txn_t();

        ctl_txn_t(const ctl_txn_t&) = delete;
        ctl_txn_t& operator=(const ctl_txn_t&) = delete;

        /** @brief Collect the liveness publish for @p vertex (RFC-0014 §4's 1-byte VALUE). */
        void publish(graph::vertex_handle_t vertex, link_state_t state);

        /** @brief Collect `fwd_router_t::remove_child(name)` — the un-route, first in phase 2. */
        void unroute(std::string name);

        /** @brief Collect `self_heal_link_t::stop()` on @p engine — JOINS its worker. */
        void stop_engine(self_heal_link_t* engine);

        /** @brief Collect the destruction of @p link — JOINS its receive thread. */
        void destroy_link(std::unique_ptr<transport_t> link);

        /** @brief Collect `graph_t::retire(vertex)` — the connection's identity goes. */
        void retire(graph::vertex_handle_t vertex);

        /**
         * @brief Phase 2: drop `ctl_m_`, then run the collected work in teardown order —
         *        un-route, stop the engine, retire the vertex, destroy the socket, publish.
         *
         * `ops_m_`, if this is an `OPERATION`, is NOT dropped here: it is the destructor's,
         * so the whole mutation stays one serialized step.
         *
         * Idempotent (a second call has nothing left to do), so the destructor may run it
         * again harmlessly after an explicit call.
         * @return The status of whichever of the two failable actions was collected — a
         *         retire (teardown) or a publish (creation, liveness). They are mutually
         *         exclusive by construction: a teardown publishes nothing and a publish
         *         retires nothing. A transaction that collected neither answers success.
         */
        graph::result_t<void> discharge();

       private:
        /** @brief End phase 1: clear the `ctl_m_` stamp, then unlock it. Idempotent. */
        void release_lock();

        const transport_vertex_t& owner_;
        std::unique_lock<std::mutex> ops_lock_; /**< @brief Engaged for an `OPERATION`. */
        std::unique_lock<std::mutex> lock_;     /**< @brief `ctl_m_`; dropped by phase 2. */
        std::string unroute_;                   /**< @brief Empty = no un-route. */
        self_heal_link_t* stop_ = nullptr;      /**< @brief Null = no engine to stop. */
        std::unique_ptr<transport_t> destroy_;  /**< @brief Null = nothing to destroy. */
        // Engaged = collected. `vertex_handle_t` has no default state to spell (ADR-0056 —
        // it is exactly a pointer, constructible only by the graph), so the optional IS the
        // armed flag rather than riding beside a synthetic null handle.
        std::optional<graph::vertex_handle_t> retire_;  /**< @brief The vertex to retire. */
        std::optional<graph::vertex_handle_t> publish_; /**< @brief The vertex to write. */
        link_state_t publish_state_{};                  /**< @brief The value to write. */
    };

    /** @brief True iff THIS thread is inside a `%ctl_txn_t`'s phase 1 — the S6 lock-order
     *         self-check's predicate (see `ctl_owner_`). */
    [[nodiscard]] bool ctl_held_by_this_thread() const noexcept;

    /** @brief True iff THIS thread is inside an `OPERATION` transaction (see `ops_owner_`). */
    [[nodiscard]] bool ops_held_by_this_thread() const noexcept;

    /**
     * @brief Creation's body, for a caller that already holds `ctl_m_` and has already
     *        resolved the MODULE: stage-or-construct the link, mint the vertex, wire the router.
     *
     * The `_locked` split dates from when there were TWO creation doors sharing one body.
     * RFC-0014 S7 retired the other one — the `:children[]` `client`/`listener` catalog types
     * — so the creator endpoint is now the sole wire caller and `%endpoint_create_locked` the
     * sole in-tree one. The split is kept because the module resolution above it is the
     * endpoint's own (positional, from its path) and this half is deliberately ignorant of
     * where the module came from.
     * @param module   The module segment the connection mounts under.
     * @param name     The connection's leaf NAME (already segment-validated).
     * @param config   The SPEC's raw `config` SETTINGS, for the kind factory's private keys.
     * @param settings The universal keys parsed out of @p config, with `role` and `kind`
     *                 already fixed by the module's declaration.
     * @param txn      The open transaction — creation's birth liveness (RFC-0014 §4's
     *                 `UP`/`LISTENING`/`DORMANT`) is COLLECTED on it, not published here,
     *                 because publishing fans out to subscribers under `ctl_m_`.
     */
    [[nodiscard]] graph::result_t<graph::vertex_handle_t> make_connection_locked(
        ctl_txn_t& txn, const std::string& module, const std::string& name,
        const wire::tlv_t* config, conn_settings_t settings);

    /**
     * @brief `remove_connection`'s body, for a caller that ALREADY holds `ctl_m_` — same
     *        non-recursive-mutex constraint as `%module_for_locked`.
     *
     * Phase 1 only: the `conns_` entry goes here, and the un-route / engine stop / vertex
     * retire / socket destruction are collected on @p txn for phase 2. Erasing the entry
     * before the vertex retires cannot let a same-name creation slip in between: the
     * identity vertex is still registered, so `register_vertex_key` refuses it
     * `PATH_IN_USE` until phase 2's retire has run — and by then the un-route has too.
     */
    [[nodiscard]] graph::result_t<void> remove_connection_locked(ctl_txn_t& txn,
                                                                 std::string_view name);

    /**
     * @brief Register `<net_root>/<module>` and its `conn` creator endpoint, idempotently.
     *
     * Called from @ref register_module under `ctl_m_`. The endpoint is a `role_t::HANDLER`
     * vertex: its `on_write` seam is the RFC-0014 §2 dispatch, so a write is EXECUTED rather
     * than assigned and the vertex stores no value.
     */
    [[nodiscard]] graph::result_t<void> mint_module_locked(const std::string& module);

    /**
     * @brief The creator endpoint's `on_write` body (RFC-0014 §2) — the payload TLV type
     *        selects create vs. remove.
     *
     * `SPEC` ⇒ create, `NAME` ⇒ remove, anything else (including an empty or undecodable
     * payload) ⇒ `TYPE_MISMATCH`. The endpoint never falls through to an ordinary assign.
     * Takes `ctl_m_` itself, so the whole dispatch — parse, module lookup, socket
     * construction, routing — is one control-plane critical section.
     * @param module The module this endpoint creates into (captured at mint time; the path
     *               IS the module, so it is never re-derived from the payload).
     * @param value  The written value, exactly as the graph handed it over (borrowed).
     */
    [[nodiscard]] graph::result_t<void> endpoint_write(const std::string& module,
                                                       const view::rope_t& value);

    /** @brief The `SPEC` ⇒ create leg of `%endpoint_write`; runs in @p txn's phase 1. */
    [[nodiscard]] graph::result_t<void> endpoint_create_locked(ctl_txn_t& txn,
                                                               const std::string& module,
                                                               const wire::tlv_t& spec);

    /** @brief The `NAME` ⇒ remove leg of `%endpoint_write`; runs in @p txn's phase 1. */
    [[nodiscard]] graph::result_t<void> endpoint_remove_locked(ctl_txn_t& txn,
                                                               const std::string& module,
                                                               std::string_view name);

    /**
     * @brief The declaration an endpoint write resolves its (kind, role) through; caller
     *        holds `ctl_m_`.
     *
     * A module's endpoint fixes the role positionally, so the role comes from here and never
     * from the payload. @p kind is the SPEC config's `kind` when it carried one — then the
     * declaration must be THIS module's for that kind, or the kind is not one this endpoint
     * creates (`SCHEMA_NOT_FOUND`). An empty @p kind takes the module's sole declaration;
     * a module declared for two kinds is genuinely ambiguous without one (`TYPE_MISMATCH`),
     * the same refusal an ambiguous staging used to give the retired `:children[]` spelling.
     */
    [[nodiscard]] graph::result_t<module_decl_t> declaration_for_locked(
        std::string_view module, std::string_view kind) const;

    /**
     * @brief `module_for`'s body, for a caller that ALREADY holds `ctl_m_`.
     *
     * A creation resolves a module from inside its own locked section, and `ctl_m_` is a
     * plain, NON-RECURSIVE `std::mutex` — so it cannot reach the public entry, which would
     * self-deadlock. That constraint is why #881 is a split rather than a lock added in
     * place: one body, two surfaces, exactly one acquisition per call.
     */
    [[nodiscard]] graph::result_t<std::string> module_for_locked(std::string_view kind,
                                                                 conn_role_t role) const;

    /**
     * @brief `set_link_state`'s body, for a caller that ALREADY holds `ctl_m_`.
     *
     * Resolution only: it maps @p name to its connection vertex and COLLECTS the write on
     * @p txn. The write itself is phase 2's, because it fans out to subscribers — see
     * `%ctl_txn_t`. Creation reaches it the same way for its birth liveness, so the two
     * doors publish through one body and one discharge.
     */
    [[nodiscard]] graph::result_t<void> set_link_state_locked(ctl_txn_t& txn, std::string_view name,
                                                              link_state_t state);

    /**
     * @brief Serializes every CONTROL-PLANE mutation here (ADR-0063 §3).
     *
     * This class had no synchronization at all, yet a creation writes `conns_` and
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
     * **Lock order (ADR-0063 §3, erratum 7).** `ops_m_ → this → fwd_router_t::ctl_m_ →
     * graph_t::map_mutex_ → the vertex stripe`, and nothing on the forward or delivery
     * path takes any of them. The order is only half the discipline, though, and the
     * weaker half: an order says which lock may nest inside which, and says nothing about
     * a call that comes back round the outside. The binding rule is the second one:
     *
     * > **`ctl_m_` is NEVER held across a call that can re-enter `transport_vertex_t`** —
     * > a subscriber fan-out (`graph_t::write`) or a thread join (`self_heal_link_t::stop`,
     * > a socket destructor) — and **`ops_m_`, which IS held across those, is never taken
     * > by a door such a call can reach.**
     *
     * Both re-entries were reachable before RFC-0014 S6 (#492), and S6's own wiring is what
     * made them live: a routing-plane subscriber of a connection's liveness drives
     * @ref acquire_link, so a publish made under a single control mutex re-entered a
     * non-recursive lock on its own thread, and a teardown that joined the engine's worker
     * mid-publish did the same across two threads. Both now run in phase 2 of a
     * `%ctl_txn_t`, with `ctl_m_` already released. What the class still holds `ctl_m_`
     * across — `graph_t::find` / `register_vertex_key` / `hide_from_enumeration`,
     * `fwd_router_t::add_child` — are structural mutations that dispatch nothing and join
     * nothing, so they cannot come back round.
     *
     * Enforced, not merely documented: `%ctl_txn_t` is the only acquisition of either
     * mutex in the class, the discharge is its destructor's, and the ownership stamps make
     * a re-entry an assertion rather than a hang (`net_lock_order_test`).
     */
    mutable std::mutex ctl_m_;

    /**
     * @brief Serializes whole control-plane OPERATIONS — the outer half of the pair.
     *
     * ADR-0063 §3 promised that a create / remove / liveness publish is serialized "in
     * full", and while one mutex covered the whole operation it was. S6 moves the discharge
     * out of `ctl_m_`, which alone would let one operation's deferred work interleave with
     * another's decision — concretely, `set_link_state`'s deferred `graph_t::write` landing
     * on the vertex a concurrent `remove_connection` is retiring, which is a genuine race
     * (`net_control_plane_race_test` reports it under TSan) and not merely an odd ordering.
     * This mutex restores the promise: an `OPERATION` transaction holds it across BOTH
     * phases.
     *
     * It is therefore held across the fan-out and the joins — and that is safe for exactly
     * one reason, which is the reason it exists as a second mutex rather than as `ctl_m_`
     * held longer: **no door a fan-out can reach takes it.** @ref acquire_link,
     * @ref release_link, @ref link_of, @ref settings_of, @ref module_for and
     * `is_structural` are `LOOKUP` scope. A callback that instead MUTATES the control plane
     * re-entrantly — a liveness subscriber calling @ref remove_connection — is refused by
     * assertion rather than deadlock; that is a restriction, and a deliberate one: a
     * teardown re-entered from inside its own teardown's fan-out has no defined meaning.
     */
    mutable std::mutex ops_m_;

    /**
     * @brief The thread inside a `%ctl_txn_t`'s phase 1 — the S6 lock-order self-check.
     *
     * `%tr::detail::unowned_thread_id()` means "nobody". Written under `ctl_m_` and read from
     * anywhere, hence atomic: the read that matters is a re-entrant one, made by a thread
     * that is about to block on the mutex this same thread holds. Without it that bug is a
     * silent hang wherever the callback happened to be installed; with it, it is a
     * diagnosable assertion at the acquisition site.
     *
     * The identity is `%tr::detail::thread_id_t` and **not** `std::thread::id` (#1532): the
     * latter lowers to `pthread_self()`, which ESP-IDF ASSERTS out of for any task it did not
     * register — the IDF main task included — so v0.15.0 abort-looped at boot on a stamp it
     * could not take. See `%thread_id.hpp` for the mechanism and for why the sentinel and
     * every live value must stay distinguishable.
     */
    mutable std::atomic<detail::thread_id_t> ctl_owner_{};

    /** @brief The same stamp for `ops_m_` — the thread inside an `OPERATION` transaction,
     *         across both its phases. This is the one a re-entrant mutation trips. */
    mutable std::atomic<detail::thread_id_t> ops_owner_{};

    graph::graph_t& graph_;
    fwd_router_t& router_;
    std::string net_root_;
    mem::mem_backend_t* rx_backend_;   // RX segment source for owned view-delivering sockets
    mem::block_source_t* egress_src_;  // TX gather store for owned sockets (#873 / ADR-0079)
    // One factory-catalog row: the constructor and the kind's capability declarations
    // (RFC-0014 S5). Traits ride WITH the factory so one lookup answers both.
    struct kind_entry_t {
        transport_factory_t factory;
        transport_kind_traits_t traits;
    };

    // Pre-supplied links awaiting their SPEC, and created connections, both by NAME;
    // the transport-factory catalog by config `kind`.
    std::map<std::string, transport_t*, std::less<>> pending_links_;
    std::map<std::string, conn_t, std::less<>> conns_;
    std::map<std::string, kind_entry_t, std::less<>> transport_types_;
    // Declared modules — see register_module / module_for (declared-only, ADR-0073 §4).
    // A flat vector, not a map: this is written at setup and read once per
    // connection creation, so an rbtree buys nothing a linear scan over a handful of entries
    // does not — and it costs a whole extra container instantiation in flash. Nothing here is
    // on the forward path.
    std::vector<module_decl_t> modules_;
};

}  // namespace tr::net
