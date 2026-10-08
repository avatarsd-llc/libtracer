/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */

/**
 * @file
 * @brief ONE CONCEPT — one link, created in-band, draws its receive blocks from the arena's net
 *        sub-pool, so its `max_frame` is a sizing decision.
 *
 * A link is created the production way: the app declares a `udp-server` module on the
 * `transport_vertex_t`, then writes a connection SPEC to its creator endpoint
 * `/net/udp-server/conn`. Left on the default sources, the UDP link draws its receive scratch
 * and each receive segment from the net sub-pool at its frame cap. That cap is the datagram
 * limit (64 KiB) unless the SPEC sets `max_frame`, and 64 KiB is twice the default arena: on
 * an MCU, `max_frame` is how a link is made to fit. The app sets it to 1 KiB and reads
 * the net sub-pool's census before and after the link comes up.
 *
 * Board-only step, not run here: a peer can reach the link only once the board has joined a
 * network (Wi-Fi or Ethernet bring-up, which is the application's and not libtracer's). From a
 * host on that network, dial `kind=udp`, `addr=<board IP>`, `port=47301`.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "libtracer/conn_spec.hpp"
#include "libtracer/fwd_router.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_udp.hpp"
#include "libtracer/transport_vertex.hpp"
#include "sdkconfig.h"
#if !CONFIG_IDF_TARGET_LINUX
#include "esp_netif.h"
#endif

namespace {

using tr::graph::path_t;

/** @brief The port the link listens on. */
constexpr std::uint16_t kPort = 47301;

/** @brief The inbound frame cap the SPEC sets: every receive block is this size. */
constexpr std::uint32_t kMaxFrame = 1024;

/** @brief Failed checks so far. */
int g_failures = 0;

/** @brief Print @p what with its verdict and count a failure. */
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief Print the verdict; on the `linux` target also exit with it, so CI can run this. */
void finish() {
    std::printf("RESULT %s\n", g_failures == 0 ? "ok" : "FAIL");
#if CONFIG_IDF_TARGET_LINUX
    std::exit(g_failures == 0 ? 0 : 1);
#endif
}

}  // namespace

extern "C" void app_main(void) {
#if !CONFIG_IDF_TARGET_LINUX
    ESP_ERROR_CHECK(esp_netif_init());  // starts lwIP; the link's socket needs it
#endif
    tr::graph::graph_t g;
    tr::net::fwd_router_t router(g);
    tr::net::transport_vertex_t net(g, router);  // receive and egress on the net sub-pool
    check(
        net.register_module(tr::net::kUdpServerSuggestedModule, "udp", tr::net::conn_role_t::LISTEN)
            .has_value(),
        "declare the udp-server module");

    tr::mem::block_source_t& pool = tr::mem::net_source();
    const std::size_t before = pool.stats().in_use;

    tr::net::conn_spec_t spec("host");
    spec.kind("udp").port(kPort).max_frame(kMaxFrame);
    check(g.write(path_t("/net/udp-server/conn"), spec.view()).has_value(),
          "write the SPEC: one udp link, max_frame 1 KiB");
    check(g.find(path_t("/net/udp-server/host").key()).has_value(),
          "the link is a vertex at /net/udp-server/host");

    const tr::mem::source_stats_t s = pool.stats();
    std::printf("net sub-pool: %zu bytes in use before the link, %zu after (max_frame %u)\n",
                before, s.in_use, static_cast<unsigned>(kMaxFrame));
    check(s.in_use > before && s.in_use - before < 4 * kMaxFrame,
          "the link drew its receive blocks from the net sub-pool, sized by max_frame");
    finish();
}
