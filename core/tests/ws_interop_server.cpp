/**
 * @file
 * @brief #54 (interop) — a standalone transport_ws_server harness driven by the TypeScript
 *        transport-ws interop test (bindings/typescript/.../test/interop.test.mjs).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * It proves the real socket path: the TS TransportWs client performs an actual
 * RFC 6455 101 handshake and sends a MASKED client BINARY frame; this C++
 * transport_ws_server unmasks it, hands the libtracer TLV to a receiver that
 * echoes the exact bytes back as an UNMASKED server BINARY frame.
 *
 * This is NOT an add_test() unit test — it is a helper binary the TS test spawns.
 * It binds a port (--port N, or 0 = ephemeral), prints a single parseable
 * `PORT=<n>` line to stdout and flushes (so the test can read the chosen port),
 * echoes every inbound frame, and exits 0 once a bounded --timeout-ms deadline
 * passes. The deadline keeps CI from ever stalling; the parent process also
 * terminates the harness promptly once the round-trip has been asserted.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <thread>

#include "libtracer/transport_ws.hpp"

namespace {

using namespace std::chrono_literals;

/** @brief Minimal argv parsing: --port N (default 0 = ephemeral), --timeout-ms N. */
std::uint16_t g_port = 0;
int g_timeout_ms = 10000;

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

    tr::net::transport_ws_server server(g_port);
    if (!server.ok()) {
        std::fprintf(stderr, "ws_interop_server: failed to bind/listen\n");
        return 1;
    }

    // Echo: hand each inbound libtracer TLV straight back as one BINARY frame.
    // This fires on the server recv thread; send() is thread-safe. The raw
    // {fn, ctx} form: the sink cannot outlive its context (the server itself).
    server.set_receiver(
        [](void* ctx, std::span<const std::byte> frame) {
            static_cast<tr::net::transport_ws_server*>(ctx)->send(frame);
        },
        &server);

    // Announce the bound port on a single parseable line, then flush so the
    // parent (the TS test) can read it immediately and never block on buffering.
    std::printf("PORT=%u\n", static_cast<unsigned>(server.local_port()));
    std::fflush(stdout);

    // Bounded run: exit cleanly at the deadline. transport_ws_server's destructor
    // stops the recv thread, joins it, and closes the sockets, so returning here
    // is a clean stop->join->exit. The parent normally kills us sooner once it has
    // asserted the round-trip; the deadline is the unconditional CI backstop.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(g_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    return 0;
}
