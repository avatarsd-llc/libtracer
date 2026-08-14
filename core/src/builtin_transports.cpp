/**
 * @file
 * @brief The FULL-NODE built-in transport registration (udp + tcp + ws).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This hand-written
 * dispatcher is what a full node compiles: the default core CMake build, the PlatformIO
 * portable set (the extra-script source filter over core/src) on every platform except
 * espressif32, and the ESP-IDF component's `linux` target. A core build that DROPS a
 * transport compiles a CMake-GENERATED variant from builtin_transports.cpp.in instead
 * (which calls only the enabled register_*_transport), so this file and the generated one
 * are never linked together; a PlatformIO espressif32 build swaps in the udp+tcp-only
 * integrations/platformio/builtin_transports_udp_tcp.cpp the same way, because its ws leg
 * is the portable POSIX pair that must not reach silicon (#947/#984). See
 * core/CMakeLists.txt and libtracer/builtin_transports.hpp.
 */
#include "libtracer/builtin_transports.hpp"

namespace tr::net {

void register_builtin_transports(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend,
                                 mem::block_source_t* egress_src) {
    register_udp_transport(vertex, rx_backend, egress_src);
    register_tcp_transport(vertex, rx_backend, egress_src);
    register_ws_transport(vertex, rx_backend, egress_src);
}

}  // namespace tr::net
