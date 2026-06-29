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
 * Topology: a single terminus node. Its graph seeds `/sensor/temp` with a known
 * VALUE; its only transport child is the ws server link named "client", so an
 * inbound FWD's first `dst` segment ("sensor") never names a transport child and
 * resolves LOCALLY (the terminus). The reply is sent back over the "client" link
 * (the link the request arrived on) — RFC-0004 §B terminus-reply asymmetry.
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

// The sample the node pushes to a subscriber once it subscribes — a distinct
// value so the TS test can tell a live producer delivery from the seeded read.
constexpr std::uint32_t kPushedSample = 0xCAFEBABEu;

// Build a VALUE TLV (opt=0, 16-bit length, LE u32 payload) via the production
// emit helpers — the canonical bytes the cross-validated TS codec also produces.
std::vector<std::byte> value_u32(std::uint32_t v) {
    std::vector<std::byte> p(4);
    tr::detail::store_le<std::uint32_t>(p, v);
    std::vector<std::byte> out;
    tr::detail::emit_tlv(out, tr::wire::type_t::VALUE, tr::wire::opt_t{}, p);
    return out;
}

tr::view::view_t make_value(std::span<const std::byte> bytes) {
    tr::view::segment_ptr_t seg = tr::view::heap_alloc(bytes.size());
    if (!bytes.empty()) std::memcpy(seg->bytes.data(), bytes.data(), bytes.size());
    return tr::view::view_t::over(std::move(seg));
}

// ---- the producer-fan-out simulation for the live-subscribe interop proof ----
// The auto-promote/flush producer seam (#136 / RFC-0004 §E) is not yet wired, so
// for the interop proof the harness watches inbound FWDs and, on a subscribe-write
// to `:subscribers[]`, pushes ONE VALUE delivery back to the subscriber's target
// over the inbound link — exactly the FWD{WRITE}+VALUE a real producer would emit.

using tr::wire::tlv_t;
using tr::wire::type_t;

std::string span_to_string(std::span<const std::byte> a) {
    return std::string(reinterpret_cast<const char*>(a.data()), a.size());
}

bool span_eq(std::span<const std::byte> a, std::string_view b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), b.size()) == 0;
}

// A subscribe is a WRITE (op byte 1) carrying a FIELD whose first NAME is "subscribers".
bool is_subscribe(const tlv_t& fwd) {
    const auto& ch = fwd.children;
    if (ch.empty() || ch[0].type != type_t::VALUE || ch[0].payload.empty()) return false;
    if (std::to_integer<int>(ch[0].payload[0]) != 1) return false;  // not WRITE
    for (const tlv_t& c : ch) {
        if (c.type == type_t::FIELD && !c.children.empty() && c.children[0].type == type_t::NAME) {
            return span_eq(c.children[0].payload, "subscribers");
        }
    }
    return false;
}

// The subscriber's target path = the SUBSCRIBER payload's PATH NAMEs (the return route).
std::vector<std::string> subscriber_target(const tlv_t& fwd) {
    for (const tlv_t& c : fwd.children) {
        if (c.type != type_t::SUBSCRIBER) continue;
        for (const tlv_t& pc : c.children) {
            if (pc.type != type_t::PATH) continue;
            std::vector<std::string> segs;
            for (const tlv_t& n : pc.children)
                if (n.type == type_t::NAME) segs.push_back(span_to_string(n.payload));
            return segs;
        }
    }
    return {};
}

// Emit a PATH TLV (PL=1) from segments.
std::vector<std::byte> path_tlv(const std::vector<std::string>& segs) {
    std::vector<std::byte> kids;
    for (const std::string& s : segs) tr::detail::emit_name(kids, s);
    std::vector<std::byte> out;
    tr::wire::opt_t pl;
    pl.pl = true;
    tr::detail::emit_tlv(out, type_t::PATH, pl, kids);
    return out;
}

// Build a delivery: FWD{ op=WRITE, dst=target, src=/sensor/temp, payload=VALUE(u32) }.
std::vector<std::byte> build_delivery(const std::vector<std::string>& target, std::uint32_t v) {
    std::vector<std::byte> kids;
    const std::byte write_op{1};
    tr::detail::emit_tlv(kids, type_t::VALUE, tr::wire::opt_t{},
                         std::span<const std::byte>(&write_op, 1));
    const std::vector<std::byte> dst = path_tlv(target);
    kids.insert(kids.end(), dst.begin(), dst.end());
    const std::vector<std::byte> src = path_tlv({"sensor", "temp"});
    kids.insert(kids.end(), src.begin(), src.end());
    std::vector<std::byte> vbytes(4);
    tr::detail::store_le<std::uint32_t>(vbytes, v);
    std::vector<std::byte> val;
    tr::detail::emit_tlv(val, type_t::VALUE, tr::wire::opt_t{}, vbytes);
    kids.insert(kids.end(), val.begin(), val.end());

    std::vector<std::byte> out;
    tr::wire::opt_t pl;
    pl.pl = true;
    tr::detail::emit_tlv(out, type_t::FWD, pl, kids);
    return out;
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
    auto vertex = graph.register_vertex(*temp_path, tr::graph::role_t::STORED_VALUE);
    if (!vertex) {
        std::fprintf(stderr, "fwd_node_server: failed to register /sensor/temp\n");
        return 1;
    }
    (void)graph.write(*vertex, make_value(value_u32(kSeededTemp)));

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

    // Live-subscribe proof: when a subscribe-write arrives, push one producer
    // VALUE delivery to the subscriber's target over the link it arrived on
    // (simulating the #136 producer fan-out). Fires on the recv thread before the
    // subscribe REPLY; the TS client registers its handler before the ack, so the
    // delivery is never dropped regardless of ordering.
    router.on_inbound([&server](std::string_view, const tlv_t& fwd) {
        if (!is_subscribe(fwd)) return;
        const std::vector<std::string> target = subscriber_target(fwd);
        if (target.empty()) return;
        server.send(build_delivery(target, kPushedSample));
    });

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
