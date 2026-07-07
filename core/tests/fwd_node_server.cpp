/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * #56 / RFC-0004 (interop) — a standalone FWD-serving node harness driven by the
 * TypeScript client SDK interop test (bindings/typescript/.../client/test/interop.test.mjs).
 * It proves the web-UI<->device consumer path end to end: the TS
 * `LibtracerClient` issues a path-addressed `read`/`write`/`await` as a `FWD`
 * frame over a real `TransportWs`; this C++ node resolves it against a live
 * `graph_t` behind a `fwd_router_t` + `op_resolver_t` and source-routes the
 * `FWD{REPLY}` back over the same ws link.
 *
 * Topology: a single terminus node. Its graph seeds `/sensor/temp` (a TRANSIENT-LOCAL
 * producer) with a known VALUE; its only transport child is the ws server link named
 * "client", so an inbound FWD's first `dst` segment ("sensor") never names a transport
 * child and resolves LOCALLY (the terminus). The reply is sent back over the "client"
 * link (the link the request arrived on) — RFC-0004 §B terminus-reply asymmetry.
 *
 * The producer fan-out is the REAL one (#136): an inbound `:subscribers[]` WRITE binds a
 * remote subscriber on `/sensor/temp`; because the vertex is transient-local the subscribe
 * LATCHES the current value (one immediate delivery), and any later WRITE to `/sensor/temp`
 * fans out a live `FWD{WRITE}` delivery back over the "client" link — all through
 * `fwd_router_t`'s registered sink, with no hand-built frames here.
 *
 * This is NOT an add_test() unit test — it is a helper binary the TS test spawns.
 * It binds a port (--port N, or 0 = ephemeral), prints a single parseable
 * `PORT=<n>` line to stdout and flushes, then runs until a bounded --timeout-ms
 * deadline (the unconditional CI backstop; the parent kills it sooner). Teardown
 * is RAII stop->join->close.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "libtracer/byteorder.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tlv.hpp"
#include "libtracer/tlv_emit.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_ws.hpp"

namespace {

using namespace std::chrono_literals;

// The seeded value at /sensor/temp — a VALUE u32 the TS interop test asserts
// byte-exact. Keep in sync with the constant in the TS test.
constexpr std::uint32_t kSeededTemp = 0x1234ABCDu;

// Build a VALUE TLV (opt=0, 16-bit length, LE u32 payload) via the production
// emit helpers — the canonical bytes the cross-validated TS codec also produces.
std::vector<std::byte> value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::wire::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, p);
    return out;
}

tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

// Minimal argv parsing: --port N (default 0 = ephemeral), --timeout-ms N.
std::uint16_t g_port = 0;
int g_timeout_ms = 15000;

void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            g_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (a == "--timeout-ms" && i + 1 < argc) {
            g_timeout_ms = std::atoi(argv[++i]);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    parse_args(argc, argv);

    // ----- the terminus graph: /sensor/temp holds a known VALUE. ----------------
    tr::graph::graph_t graph;
    const auto temp_path = tr::graph::path_t::parse("/sensor/temp");
    if (!temp_path) {
        std::fprintf(stderr, "fwd_node_server: failed to parse /sensor/temp\n");
        return 1;
    }
    // Transient-local (durability=1) so a fresh subscribe LATCHES the current value
    // (RFC-0004 §D / #136) — the interop test's first delivery, with no producer thread.
    tr::graph::settings_t producer_settings;
    producer_settings.durability = 1;
    const auto vertex =
        graph.register_vertex(*temp_path, tr::graph::role_t::STORED_VALUE, {}, producer_settings);
    (void)graph.write(vertex, make_value(value_u32(kSeededTemp)));

    // ----- the FWD router + ws server. The server link is the node's only child,
    // named "client"; an inbound FWD resolves /sensor/temp locally and the reply
    // goes back over this link. ---------------------------------------------------
    tr::net::fwd_router_t router(graph);
    tr::net::transport_ws_server server(g_port);
    if (!server.ok()) {
        std::fprintf(stderr, "fwd_node_server: ws server failed to bind/listen\n");
        return 1;
    }
    router.add_child("client", server);
    // The producer fan-out (subscribe latch + write-driven deliveries) is wired by the
    // fwd_router_t sink itself (#136) — no observer hook here. A subscribe binds a remote
    // subscriber; the transient-local latch and any later WRITE drive real deliveries.

    // Announce the bound port on a single parseable line, then flush.
    std::printf("PORT=%u\n", static_cast<unsigned>(server.local_port()));
    std::fflush(stdout);

    // Bounded run: exit cleanly at the deadline (RAII stops the recv thread, joins
    // it, closes the sockets). The parent normally kills us sooner.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(g_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    return 0;
}
