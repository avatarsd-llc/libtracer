/**
 * @file
 * @brief #408 — one node of the containerized decentralized MESH testbed.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A real libtracer host node: a `graph_t` behind a `fwd_router_t` + `transport_vertex_t`
 * with the built-in socket factories, run one-per-container by `tests/testbed/compose.yml`
 * and driven by the TypeScript client SDK
 * (`bindings/typescript/packages/client/test/mesh-testbed.test.mjs`).
 *
 * WHAT THIS BINARY DELIBERATELY DOES **NOT** DO: form the mesh. It creates only its own
 * LISTENERS — the sockets it owns — and then waits. Every inter-node link is DIALLED by a
 * `SPEC` the driver writes into this node's `/net:children[]` **remotely, over the wire**.
 * That is the whole point of the testbed: it exercises the in-band formation plane a web UI
 * uses (ADR-0017 / ADR-0027, reference/13 §2) — a third party holding delegated admin
 * creates links on devices and departs, leaving the devices talking with nothing in the
 * data path — rather than a node self-wiring from a config file. No `provide_link` seam is
 * used anywhere: every link is a real socket built by the built-in `ws` factory from a
 * SPEC's config.
 *
 * STARTUP ORDER IS THE TESTBED'S SYNCHRONISATION, and it is structural rather than a
 * healthcheck. A built-in DIAL is SYNCHRONOUS and has no retry (`transport_tcp.hpp`:
 * "Reconnect is out of scope"), and a refused dial returns before `register_vertex_key`,
 * so it leaves NO vertex and the whole SPEC must be re-issued. This binary therefore
 * creates every link listener FIRST and its ctrl listener LAST, then announces READY.
 * Because the ctrl link is the only way the driver can reach this node at all,
 * **ctrl-reachable implies every link listener is already bound** — so once the driver has
 * connected to all nodes, no dial it issues can lose a startup race.
 *
 * ADDRESSING (the form this testbed pins — see #419): a connection's ROUTING key is its
 * BARE name, not its `/net/<name>` vertex key. `transport_vertex.cpp` registers the router
 * child under the bare `name` while the graph composes the vertex at `/net/<name>`, and
 * `fwd_router_t` resolves a FWD's first `dst` segment against the child-link registry
 * BEFORE the local graph (which is exactly why #373 must reject a link name that shadows a
 * first-level vertex). So node A reaches C through B at `dst=/b/c/...`, NOT
 * `/net/b/net/c/...` as reference/03 and /07 currently claim.
 *
 * IDENTITY: `--identity <64 hex>` installs the node's RFC-0011 `:identity` record, which
 * is what lets the #409 topology walk collapse the ring instead of unrolling it. A real
 * node loads a real keypair; either way the facet serves a CLAIM and signs nothing.
 *
 * NAMING RULE: every node names every link after the node at the FAR end. That is what
 * makes the return route work — each forwarder prepends its own name for the ARRIVAL link
 * to `src`, so a reply retraces the request hop by hop (RFC-0004 §B; the terminus itself
 * answers over the arrival link).
 *
 * This is NOT an add_test() unit test — it is a helper binary, like `ws_interop_server` and
 * `fwd_node_server`. It prints a single parseable `READY name=<n>` line and flushes, then
 * runs until a bounded `--timeout-ms` deadline (the unconditional CI backstop; the parent
 * normally tears it down sooner). Teardown is RAII stop->join->close.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_vertex.hpp"

namespace {

using namespace std::chrono_literals;
using tr::graph::graph_t;
using tr::graph::path_t;
using tr::graph::role_t;
using tr::net::conn_role_t;
using tr::view::view_t;
using tr::wire::opt_t;
using tr::wire::type_t;

/** @brief The seeded `/sensor/temp` value — the TS driver asserts it byte-exactly. */
constexpr std::uint32_t kSeededTemp = 0x1234ABCDu;

/** @brief Wrap @p bytes in an owning single-link view. */
view_t owned(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return view_t::over(std::move(seg));
}

/** @brief A VALUE TLV over an opaque byte payload. */
view_t value_bytes(std::span<const std::byte> payload) {
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::VALUE, opt_t{}, payload);
    return owned(out);
}

/** @brief A VALUE TLV carrying @p text's bytes (the protocol never inspects a VALUE). */
view_t value_text(std::string_view text) {
    std::vector<std::byte> p(text.size());
    std::memcpy(p.data(), text.data(), text.size());
    return value_bytes(p);
}

/** @brief A VALUE TLV carrying a little-endian u32. */
view_t value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    return value_bytes(p);
}

/**
 * @brief A connection-creation SPEC (ADR-0027 / reference/05), with the ws-private
 *        `peer_named` / `max_peers` keys (ADR-0043 §5).
 *
 * Copied from `core/tests/transport_vertex_test.cpp` rather than shared: `conn_spec` is a
 * test/example-local helper in six places today and is deliberately NOT a shipped header
 * (`tree_of_ropes.cpp` says the same). Promoting it is its own change, not #408's.
 *
 * SPEC{ NAME "type" <type>, NAME "name" <name>, NAME "config" SETTINGS{ NAME "role" VALUE u8,
 *       NAME "port" VALUE u16 [, NAME "kind" NAME <kind>][, NAME "addr" NAME <addr>]
 *       [, NAME "peer_named" VALUE u8, NAME "max_peers" VALUE u32] } }
 *
 * The TS `encodeConnSpec` is byte-pinned against this exact layout.
 */
view_t conn_spec(std::string_view type, std::string_view name, conn_role_t role, std::uint16_t port,
                 std::string_view kind = {}, std::string_view addr = {}, bool peer_named = false,
                 std::uint32_t max_peers = 0) {
    std::vector<std::byte> cfg;
    tr::wire::emit_name(cfg, "role");
    const std::byte r{static_cast<std::uint8_t>(role)};
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&r, 1));
    tr::wire::emit_name(cfg, "port");
    std::vector<std::byte> pb(2);
    tr::detail::store_le(pb, port, 2);
    tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, pb);
    if (!kind.empty()) {
        tr::wire::emit_name(cfg, "kind");
        tr::wire::emit_name(cfg, kind);
    }
    if (!addr.empty()) {
        tr::wire::emit_name(cfg, "addr");
        tr::wire::emit_name(cfg, addr);
    }
    if (peer_named) {
        tr::wire::emit_name(cfg, "peer_named");
        const std::byte pn{1};
        tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, std::span<const std::byte>(&pn, 1));
        tr::wire::emit_name(cfg, "max_peers");
        std::vector<std::byte> mb(4);
        tr::detail::store_le(mb, max_peers, 4);
        tr::wire::emit_tlv(cfg, type_t::VALUE, opt_t{}, mb);
    }

    std::vector<std::byte> body;
    tr::wire::emit_name(body, "type");
    tr::wire::emit_name(body, type);
    tr::wire::emit_name(body, "name");
    tr::wire::emit_name(body, name);
    tr::wire::emit_name(body, "config");
    tr::wire::emit_tlv(body, type_t::SETTINGS, opt_t{.pl = true}, cfg);

    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, type_t::SPEC, opt_t{.pl = true}, body);
    return owned(out);
}

/** @brief One `--listen name:port` request parsed from argv. */
struct listen_spec_t {
    std::string name;
    std::uint16_t port = 0;
    bool peer_named = false;
    std::uint32_t max_peers = 0;
};

std::string g_name;
std::uint16_t g_ctrl_port = 47300;
int g_timeout_ms = 180000;
std::vector<listen_spec_t> g_listens;
std::vector<std::byte> g_identity;  // 32 raw bytes when --identity was given

/**
 * @brief Parse 64 hex chars into the 32 raw bytes of an ed25519-shaped public key.
 *
 * A REAL node loads a real keypair; the testbed passes a fixed per-node key from argv
 * so the topology walk has a stable cross-path identity to dedup by. The facet serves a
 * CLAIM either way (RFC-0011 / ADR-0045 decision 3) — nothing here signs anything.
 */
bool parse_identity(std::string_view hex, std::vector<std::byte>& out) {
    if (hex.size() != 64) return false;
    out.assign(32, std::byte{});
    for (std::size_t i = 0; i < 32; ++i) {
        int hi = -1, lo = -1;
        for (int p = 0; p < 2; ++p) {
            const char ch = hex[i * 2 + static_cast<std::size_t>(p)];
            const int v = (ch >= '0' && ch <= '9')   ? ch - '0'
                          : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10
                          : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10
                                                     : -1;
            if (v < 0) return false;
            (p == 0 ? hi : lo) = v;
        }
        out[i] = static_cast<std::byte>((hi << 4) | lo);
    }
    return true;
}

/** @brief Parse `name:port` (e.g. `a:47311`); returns false on a malformed spec. */
bool parse_listen(std::string_view arg, listen_spec_t& out) {
    const auto colon = arg.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= arg.size()) return false;
    out.name = std::string(arg.substr(0, colon));
    out.port = static_cast<std::uint16_t>(std::atoi(std::string(arg.substr(colon + 1)).c_str()));
    return out.port != 0;
}

bool parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--name" && i + 1 < argc) {
            g_name = argv[++i];
        } else if (a == "--ctrl-port" && i + 1 < argc) {
            g_ctrl_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (a == "--timeout-ms" && i + 1 < argc) {
            g_timeout_ms = std::atoi(argv[++i]);
        } else if (a == "--identity" && i + 1 < argc) {
            if (!parse_identity(argv[++i], g_identity)) {
                std::fprintf(stderr, "mesh_node: --identity wants 64 hex chars (32 bytes)\n");
                return false;
            }
        } else if ((a == "--listen" || a == "--peer-named-listen") && i + 1 < argc) {
            listen_spec_t ls;
            if (!parse_listen(argv[++i], ls)) {
                std::fprintf(stderr, "mesh_node: malformed %s (want name:port)\n", a.c_str());
                return false;
            }
            if (a == "--peer-named-listen") {
                ls.peer_named = true;
                ls.max_peers = 8;
            }
            g_listens.push_back(std::move(ls));
        } else {
            std::fprintf(stderr, "mesh_node: unknown argument %s\n", a.c_str());
            return false;
        }
    }
    if (g_name.empty()) {
        std::fprintf(stderr, "mesh_node: --name is required\n");
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (!parse_args(argc, argv)) return 2;

    // The graph and router outlive the transport vertex that binds them (owned sockets must
    // stop delivering before the router they feed is gone) — the documented stack order.
    graph_t graph;
    tr::net::fwd_router_t router(graph);
    tr::net::transport_vertex_t net(graph, router);

    // ----- the node's IDENTITY (#406 / RFC-0011), before anything is served ---------
    // With this installed, `read <any-vertex>:identity` answers the same 60-byte record
    // everywhere, so a topology walk can prove that /b and /c/a/b are ONE device and
    // terminate on the ring (ADR-0044 pt 3: the core never dedups; the client does,
    // keyed by an identity it chooses — this is that key). Without --identity the node
    // stays keyless and the walk degrades to a bounded, non-authoritative shape.
    if (!g_identity.empty()) {
        const auto idres = graph.set_identity(0x01, g_identity);
        if (!idres) {
            std::fprintf(stderr, "mesh_node[%s]: set_identity FAILED (status %d)\n", g_name.c_str(),
                         static_cast<int>(idres.error()));
            return 1;
        }
    }

    // ----- the application surface every node exposes -----------------------------
    // /node/name is a SEEDED APPLICATION VALUE, *not* an identity: it proves which node a
    // route terminated at, and nothing more. A node has no identity facet today (#406 /
    // RFC-0011), so two paths reaching the same device cannot be recognised as one — see
    // tests/testbed/README.md.
    const auto name_v = graph.register_vertex(*path_t::parse("/node/name"), role_t::STORED_VALUE);
    (void)graph.write(name_v, value_text(g_name));

    // Transient-local (durability=1) so a fresh subscribe LATCHES the current value
    // (RFC-0004 §D / #136): the driver's multi-hop subscribe gets one delivery with no
    // producer thread, and a later remote WRITE fans out a live delivery.
    tr::graph::settings_t producer;
    producer.durability = 1;
    const auto temp_v =
        graph.register_vertex(*path_t::parse("/sensor/temp"), role_t::STORED_VALUE, {}, producer);
    (void)graph.write(temp_v, value_u32(kSeededTemp));

    // ----- every LISTENER, created IN BAND ------------------------------------------
    // A local graph.write of a SPEC is the identical code path an inbound FWD{WRITE} takes
    // — same op_resolver, same make_connection, same factory. No provide_link anywhere.
    for (const auto& ls : g_listens) {
        const auto w = graph.write(path_t("/net:children[]"),
                                   conn_spec("listener", ls.name, conn_role_t::LISTEN, ls.port,
                                             "ws", {}, ls.peer_named, ls.max_peers));
        if (!w) {
            std::fprintf(stderr, "mesh_node[%s]: listener %s:%u FAILED (status %d)\n",
                         g_name.c_str(), ls.name.c_str(), static_cast<unsigned>(ls.port),
                         static_cast<int>(w.error()));
            return 1;
        }
    }

    // ----- the ctrl listener, LAST (the readiness barrier; see the file header) -------
    const auto ctrl =
        graph.write(path_t("/net:children[]"),
                    conn_spec("listener", "ctrl", conn_role_t::LISTEN, g_ctrl_port, "ws"));
    if (!ctrl) {
        std::fprintf(stderr, "mesh_node[%s]: ctrl listener :%u FAILED (status %d)\n",
                     g_name.c_str(), static_cast<unsigned>(g_ctrl_port),
                     static_cast<int>(ctrl.error()));
        return 1;
    }

    std::printf("READY name=%s ctrl=%u listens=%zu identity=%s\n", g_name.c_str(),
                static_cast<unsigned>(g_ctrl_port), g_listens.size(),
                g_identity.empty() ? "none" : "ed25519");
    std::fflush(stdout);

    // Bounded run: exit cleanly at the deadline (RAII stops the recv threads, joins them,
    // closes the sockets). The parent normally tears us down sooner.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(g_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(50ms);
    return 0;
}
