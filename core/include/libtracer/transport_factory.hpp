/**
 * @file
 * @brief What a transport kind needs to plug into `/net` without the graph: the connection
 *        settings it is built from, its role, its link-liveness value, its kind traits and
 *        the factory signature.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A leaf on purpose (#1720): a transport header (CAN, QUIC, WebTransport, the self-heal
 * engine) only has to NAME a factory type and the settings record it receives, so it
 * includes this header instead of `transport_vertex.hpp`, and does not parse `graph.hpp`
 * or `vertex.hpp`. `transport_vertex.hpp` includes it back, so its users see no change,
 * and `transport_vertex_t::transport_factory_t` remains an alias of
 * @ref tr::net::transport_factory_t.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>

#include "libtracer/inline_fn.hpp"
#include "libtracer/link_kind.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/status.hpp"
#include "libtracer/transport.hpp"

namespace tr::wire {
class tlv_node_t;  // fwd-decl: the factory takes a `const tlv_node_t*` raw config (no L2
                   // pull-in).
}

namespace tr::net {

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
 * `addr`/`port`/`role`/`kind` are a *device-private* `:settings` facet of
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
 * §5 leanness ruling): a kind's PRIVATE config (e.g. quic's `tls` profile name) never
 * lands here — the kind's own factory parses it from the raw config SETTINGS TLV it
 * receives alongside these settings. A universal key with no consumer is not kept here either:
 * the `keepalive` key is still ACCEPTED on the wire (existing configs parse) but is ignored and
 * lands nowhere (#1666) — a keepalive a kind needs belongs in that kind's own config.
 *
 * The two text keys are VIEWS, not owned strings (ADR-0083 Q15, #1780): they borrow the config
 * bytes they were parsed from. `transport_vertex_t` parses them out of its own copy of the
 * SPEC's config, which lives as long as the connection, so a factory, the liveness engine and
 * @ref transport_vertex_t::settings_of all read views that stay valid for the connection's
 * life. A caller building a record by hand keeps the viewed text alive for as long as the
 * record is used.
 */
struct conn_settings_t {
    std::string_view addr;                /**< @brief Peer IPv4 dotted-quad (DIAL). */
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
    std::uint32_t max_frame = 0;          /**< @brief Per-connection receive frame cap for every
                                                      framed transport — the length-prefix streams
                                                      (`tcp`, `quic`, `webtransport`) read it off
                                                      their u32 prefix, `ws` off the RFC 6455 frame
                                                      header; 0 = the protocol default (`kMaxFrame`,
                                                      16 MiB). Only tightens, never raises. */
    std::string_view kind;                /**< @brief Transport-factory selector ("udp",
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
 * @brief The owner of a factory-built link: the concrete transport, in a block from the
 *        source the factory was handed, destroyed through `transport_t`'s virtual destructor
 *        and returned at the concrete type's size (ADR-0083, #1780).
 */
using transport_ptr_t = mem::poly_ptr_t<transport_t>;

/**
 * @brief Build concrete transport `T` from @p args in a block from @p src, as a
 *        @ref transport_ptr_t — the factory's spelling of `std::make_unique<T>`.
 * @return An empty owner when @p src refused; nothing was constructed.
 */
template <class T, class... Args>
[[nodiscard]] transport_ptr_t make_transport(mem::block_source_t& src, Args&&... args) noexcept {
    return mem::make_poly<T>(src, std::forward<Args>(args)...);
}

/** @brief Bytes a @ref transport_factory_t keeps inline for its captures: four pointers (a
 *         backend, a source and room for a kind's own two). A bigger capture fails to
 *         compile; capture a pointer to a context instead. */
inline constexpr std::size_t kTransportFactoryCapacity = 4 * sizeof(void*);

/**
 * @brief Constructs an owning transport from a connection's parsed settings plus
 *        the raw config TLV, in a block from the source it is handed.
 *
 * The shared @ref conn_settings_t carries ONLY the universal keys (the ADR-0043 §5
 * leanness ruling); the raw config is the SPEC's config SETTINGS TLV as written (may
 * be null when the SPEC carried none), from which a kind's factory parses its own
 * kind-private keys (e.g. quic's `tls`/`insecure`) — the factory's business, module-side.
 * The third argument is the store the link object itself is drawn from: the receiving
 * `transport_vertex_t` passes its own (receiver pays, ADR-0083 Q21), and the factory builds
 * with `%make_transport` over it. Its refusal is `BACKPRESSURE`.
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
 *
 * A stored, non-hook callable, so it is an `inline_fn_t` (ADR-0083 Q10/Q18): the
 * captures live inline, and one that does not fit, or owns a resource, fails to compile.
 */
using transport_factory_t =
    inline_fn_t<graph::result_t<transport_ptr_t>(const conn_settings_t&, const wire::tlv_node_t*,
                                                 mem::block_source_t&),
                kTransportFactoryCapacity>;

}  // namespace tr::net
