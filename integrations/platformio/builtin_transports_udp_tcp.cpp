/**
 * @file
 * @brief The PlatformIO `espressif32` register_builtin_transports dispatcher (udp + tcp — NO ws).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Compiled by
 * `integrations/platformio/pio_esp32_can.py` on an `espressif32` target ONLY, in place of
 * core's hand-written full-node dispatcher (`core/src/builtin_transports.cpp`, udp+tcp+ws),
 * which the hook excludes from the source filter there together with the portable
 * `transport_ws.cpp` / `builtin_transport_ws.cpp` pair. The full-node form's
 * `register_ws_transport` call is exactly the reference that would drag the portable
 * POSIX-socket WS objects back into a chip image, where they cannot move data at all
 * (lwIP's `lwip_sendmsg` rejects `MSG_NOSIGNAL` with `EOPNOTSUPP`, so every scatter-gather
 * data frame is silently dropped — the #947 ruling, failure mode #948, PlatformIO half
 * #984). Same TU-selection mechanism as the CMake-generated variant from
 * `builtin_transports.cpp.in` — no feature macros; a dropped transport leaves neither a
 * glue TU nor a dangling call.
 *
 * PlatformIO `espressif32` therefore ships NO WebSocket transport: the IDF-native links
 * (`httpd_ws_link_t` / `esp_ws_client_link_t`) are not packaged for PlatformIO either
 * (documented in `integrations/platformio/README.md`; packaging them is the sanctioned
 * follow-up on #984 once their component dependencies are verified under
 * `framework-espidf`).
 */
#include "libtracer/builtin_transports.hpp"

namespace tr::net {

void register_builtin_transports(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend) {
    register_udp_transport(vertex, rx_backend);
    register_tcp_transport(vertex, rx_backend);
}

}  // namespace tr::net
