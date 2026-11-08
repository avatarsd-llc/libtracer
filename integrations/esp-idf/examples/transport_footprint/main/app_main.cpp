/**
 * @file
 * @brief transport_footprint — links the CAN and WebSocket transports for the size gate.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * A footprint-only image (#2678): full_node creates no CAN and no WebSocket connection, so
 * --gc-sections drops both and tools/esp_size_gate.py has nothing to pin. This app
 * constructs each link the way a node owns one and attaches it to a router, so the linked
 * image keeps what a node using that transport keeps. CI builds it and reads its map; it is
 * never flashed, and it does not have to bring any link up.
 */

#include "libtracer/fwd_router.hpp"
#include "libtracer/mem_heap.hpp"
#include "libtracer/mem_poly_ptr.hpp"
#include "libtracer/tracer.hpp"
#include "libtracer/transport_can.hpp"
#include "libtracer_esp/esp_ws_client_link.hpp"
#include "libtracer_esp/httpd_ws_link.hpp"
#include "libtracer_esp/twai_link.hpp"

extern "C" void app_main(void) {
    tr::graph::graph_t graph;

    // CAN: the portable transport over the TWAI-backed raw link.
    tr::net::transport_can_config_t can_config;
    can_config.node = 1;
    tr::net::can_transport_t can(tr::mem::make_poly<tr::net::twai_link_t>(
                                     tr::mem::net_source(), tr::net::twai_link_config_t{}),
                                 can_config);
    // WebSocket: the esp_http_server link, and the esp_websocket client link.
    auto ws_server = tr::mem::make_poly<tr::net::httpd_ws_link_t>(tr::mem::net_source(), 80);
    auto ws_client =
        tr::mem::make_poly<tr::net::esp_ws_client_link_t>(tr::mem::net_source(), "192.0.2.1", 80);

    // Declared after the links, so it is destroyed first and never outlives one.
    tr::net::fwd_router_t router(graph);
    const bool attached = router.attach_link("can", can) && ws_server &&
                          router.attach_link("ws", *ws_server) && ws_client &&
                          router.attach_link("wsc", *ws_client);
    static_cast<void>(attached);
}
