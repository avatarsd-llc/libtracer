/**
 * @file
 * @brief The FULL-NODE built-in transport registration (udp + tcp + ws).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * This hand-written
 * dispatcher is what a full node compiles: the default core CMake build, the PlatformIO
 * portable set (library.json globs core/src), and the ESP-IDF component. A core build
 * that DROPS a transport compiles a CMake-GENERATED variant from builtin_transports.cpp.in
 * instead (which calls only the enabled register_*_transport), so this file and the
 * generated one are never linked together. See core/CMakeLists.txt and
 * libtracer/builtin_transports.hpp.
 */
#include "libtracer/builtin_transports.hpp"

namespace tr::net {

void register_builtin_transports(transport_vertex_t& vertex, mem::mem_backend_t* rx_backend) {
    register_udp_transport(vertex, rx_backend);
    register_tcp_transport(vertex, rx_backend);
    register_ws_transport(vertex, rx_backend);
}

}  // namespace tr::net
