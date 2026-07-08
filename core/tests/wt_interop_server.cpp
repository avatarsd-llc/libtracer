/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ADR-0043 Phase B (interop) — a standalone webtransport_transport_t echo
 * harness for the TypeScript transport-webtransport browser interop test
 * (bindings/typescript/packages/transport-webtransport/test/interop-browser.test.mjs):
 * a real browser (chrome-headless via puppeteer, when available) dials
 * `https://127.0.0.1:<port>/` with `serverCertificateHashes`, opens the
 * bidirectional frame stream, and every length-prefixed frame it sends is
 * echoed back — end-to-end validation of the browser <-> device path.
 *
 * Like ws_interop_server this is NOT an add_test() unit test — it is a helper
 * binary the TS test spawns (only built with LIBTRACER_WITH_QUIC). It serves
 * the PEM cert/key given by --cert/--key (the browser trust path needs an
 * ECDSA cert valid <= 14 days — the TS harness generates one), prints a single
 * parseable `PORT=<n>` line, echoes every inbound frame, and exits 0 at the
 * --timeout-ms backstop so CI can never stall.
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <thread>

#include "libtracer/transport_webtransport.hpp"

namespace {

using namespace std::chrono_literals;

// Minimal argv parsing: --port N (default 0 = ephemeral), --cert/--key PEM
// paths (required), --timeout-ms N.
std::uint16_t g_port = 0;
std::string g_cert;
std::string g_key;
int g_timeout_ms = 15000;

void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            g_port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (a == "--cert" && i + 1 < argc) {
            g_cert = argv[++i];
        } else if (a == "--key" && i + 1 < argc) {
            g_key = argv[++i];
        } else if (a == "--timeout-ms" && i + 1 < argc) {
            g_timeout_ms = std::atoi(argv[++i]);
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    parse_args(argc, argv);
    if (g_cert.empty() || g_key.empty()) {
        std::fprintf(stderr, "wt_interop_server: --cert and --key are required\n");
        return 1;
    }

    tr::net::webtransport_transport_t server(g_port, g_cert, g_key);
    if (!server.ok()) {
        std::fprintf(stderr, "wt_interop_server: failed to bind/listen\n");
        return 1;
    }

    // Echo: hand each inbound frame straight back on the session's frame
    // stream. Fires on an msquic worker thread; send() is thread-safe. The raw
    // {fn, ctx} form: the sink cannot outlive its context (the server itself).
    server.set_receiver(
        [](void* ctx, std::span<const std::byte> frame) {
            static_cast<tr::net::webtransport_transport_t*>(ctx)->send(frame);
        },
        &server);

    // Announce the bound port on a single parseable line, then flush so the
    // parent (the TS test) can read it immediately.
    std::printf("PORT=%u\n", static_cast<unsigned>(server.local_port()));
    std::fflush(stdout);

    // Bounded run: exit cleanly at the deadline (the destructor drains msquic
    // callbacks). The parent normally kills us sooner once it has asserted the
    // round-trip; the deadline is the unconditional CI backstop.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(g_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }
    return 0;
}
