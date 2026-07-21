# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# ESP-IDF component wrapper for the libtracer C++ reference core.
#
# The sources live in the repo's core/; this component pulls them in so an
# ESP-IDF project can depend on libtracer through the component manager. It
# packages the FULL NODE (#183): the wire codec, the L0/L1 substrate, the L4
# graph runtime, AND the net plane — fwd_router / op_resolve / route_handle /
# transport_vertex (child_registry is header-only) plus the socket transports
# (udp / tcp / ws over lwIP BSD sockets), loopback, and transport_can with the
# ESP TWAI can_link_t (twai_link.cpp, this directory — the only IDF-specific
# source).
#
# Platform TU selection happens HERE, in the build system — never via in-source
# #ifdefs (the no-feature-macro ruling):
#   * chip targets compile socketcan_link_stub.cpp (SocketCAN is Linux-only;
#     socketcan_link_t::ok() stays false) and twai_link.cpp (the TWAI-backed
#     can_link_t, needs esp_driver_twai + FreeRTOS headers);
#   * the `linux` (POSIX host) target compiles the REAL socketcan_link.cpp
#     (glibc has <linux/can.h>) and no twai_link (no TWAI peripheral).
#
# Requires the GCC 13+ toolchain (C++23: std::expected, std::span) shipped with
# ESP-IDF >= 5.3; CI builds with release-v6.0 (GCC 15).

# Where core/ lives, resolved for BOTH consumption modes:
#   * In-repo: this component dir is integrations/esp-idf/libtracer/, so the repo
#     root (which holds core/) is three levels up. An ESP-IDF project consumes the
#     component in place by pointing `EXTRA_COMPONENT_DIRS` at it (see the bundled
#     examples' top-level CMakeLists.txt).
#   * Published registry component: the release job vendors core/ INTO this
#     directory so the archive is self-contained (a consumer only gets the
#     component, not the whole repo). When that vendored copy is present, prefer
#     it. This keeps one CMakeLists correct in both modes.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/core/src/frame.cpp")
    set(LIBTRACER_ROOT "${CMAKE_CURRENT_LIST_DIR}")
else()
    set(LIBTRACER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
endif()

set(LIBTRACER_SRCS
    # L2/L3 wire codec + L0/L1 substrate + L4 graph runtime (the P0 profile)
    "${LIBTRACER_ROOT}/core/src/frame.cpp"
    "${LIBTRACER_ROOT}/core/src/tlv_arena.cpp"
    "${LIBTRACER_ROOT}/core/src/backend_set.cpp"
    "${LIBTRACER_ROOT}/core/src/mem_heap.cpp"
    "${LIBTRACER_ROOT}/core/src/mem_pool.cpp"
    "${LIBTRACER_ROOT}/core/src/rope.cpp"
    "${LIBTRACER_ROOT}/core/src/rope_decode.cpp"
    "${LIBTRACER_ROOT}/core/src/tlv_view.cpp"
    "${LIBTRACER_ROOT}/core/src/path.cpp"
    "${LIBTRACER_ROOT}/core/src/graph.cpp"
    # net plane (ADR-0038/0040): FWD routing + transport/connection vertices
    "${LIBTRACER_ROOT}/core/src/op_resolve.cpp"
    "${LIBTRACER_ROOT}/core/src/op_resolve_view.cpp"
    "${LIBTRACER_ROOT}/core/src/route_handle.cpp"
    "${LIBTRACER_ROOT}/core/src/fwd_router.cpp"
    "${LIBTRACER_ROOT}/core/src/transport_vertex.cpp"
    "${LIBTRACER_ROOT}/core/src/loopback.cpp"
    # built-in transport-factory registrations (udp/tcp/ws) transport_vertex_t wires up.
    # The ESP-IDF component is always a FULL node, so it compiles all three glue TUs plus
    # the hand-written full-node dispatcher src/builtin_transports.cpp (register_builtin_transports).
    "${LIBTRACER_ROOT}/core/src/builtin_transport_udp.cpp"
    "${LIBTRACER_ROOT}/core/src/builtin_transport_tcp.cpp"
    "${LIBTRACER_ROOT}/core/src/builtin_transport_ws.cpp"
    "${LIBTRACER_ROOT}/core/src/builtin_transports.cpp"
    # socket transports — lwIP BSD sockets on chip targets, glibc on `linux`
    "${LIBTRACER_ROOT}/core/src/posix_endpoint.cpp"
    "${LIBTRACER_ROOT}/core/src/transport_udp.cpp"
    "${LIBTRACER_ROOT}/core/src/transport_tcp.cpp"
    "${LIBTRACER_ROOT}/core/src/transport_ws.cpp"
    # CAN transport (framing/advertise/reassembly — portable, host-tested)
    "${LIBTRACER_ROOT}/core/src/transport_can.cpp"
)

# lwip is a PRIVATE requirement of the socket transports' .cpp files only
# (<sys/socket.h> etc. resolve into lwIP on chip targets); the public headers
# expose no lwIP types. On the `linux` target the same includes resolve into
# glibc, so no lwip component is needed (and IDF does not provide one there).
set(LIBTRACER_PRIV_REQUIRES pthread)
set(LIBTRACER_PRIV_INCLUDE_DIRS "")

if(IDF_TARGET STREQUAL "linux")
    # POSIX host: the real SocketCAN link (glibc ships <linux/can.h>).
    list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/socketcan_link.cpp")
else()
    # Chip targets: SocketCAN stub + the TWAI-backed can_link_t, plus the
    # esp_http_server-backed WS transport link (httpd_ws_link_t) — the embedded-native
    # replacement for core's raw-socket transport_ws_server. Both are platform links,
    # picked by which TU compiles (never an in-source #ifdef). transport_ws.cpp stays
    # in LIBTRACER_SRCS above: it compiles but a node that binds this httpd link instead
    # references it nowhere, so --gc-sections drops it from the image (the linux VB,
    # which HAS no esp_http_server, keeps it).
    list(APPEND LIBTRACER_SRCS
        "${LIBTRACER_ROOT}/core/src/socketcan_link_stub.cpp"
        "twai_link.cpp"
    )
    # httpd_ws_link.cpp is built on esp_http_server's native WebSocket API
    # (httpd_ws_frame_t, HTTPD_WS_TYPE_*, httpd_uri_t::is_websocket, ...), which the
    # component ships ONLY when CONFIG_HTTPD_WS_SUPPORT is enabled. Select the TU in
    # CMake on that config (never an in-source #ifdef, per the platform-link rule
    # above): a node that binds this link enables WS support; an example that does
    # not use the WS link builds without it (transport_ws.cpp still --gc-sections out).
    if(CONFIG_HTTPD_WS_SUPPORT)
        list(APPEND LIBTRACER_SRCS "httpd_ws_link.cpp")
    endif()
    # esp_driver_gpio: twai_link.cpp names its pins via <driver/gpio.h> types.
    # esp_http_server: httpd_ws_link.cpp rides its native WS API. It MUST be an
    # UNCONDITIONAL requirement — NOT gated on CONFIG_HTTPD_WS_SUPPORT like the TU
    # above. IDF resolves component requirements in an early-expansion pass where
    # CONFIG_* symbols are not yet defined, so a Kconfig-gated REQUIRES silently
    # evaluates false there and never propagates the component's include dir; the
    # TU (correctly gated on the same config in the second pass) then compiles
    # WITHOUT <esp_http_server.h> and dies with "No such file" on any node that
    # enables WS support (the origin firmware's prod build hit exactly this). The header
    # is a base IDF component present on every chip target, so requiring it always is
    # safe and free when the TU is absent — nothing references its symbols, so
    # --gc-sections drops the link dep and libtracer.a is byte-identical (no footprint
    # change to the WS-off examples the sentinel measures).
    list(APPEND LIBTRACER_PRIV_REQUIRES lwip esp_driver_twai esp_driver_gpio esp_http_server)
    # compat/include: POSIX headers IDF's libc spells differently (<poll.h> ->
    # <sys/poll.h>) — private, so core sources stay pure POSIX and dependents
    # never see the shim.
    list(APPEND LIBTRACER_PRIV_INCLUDE_DIRS "compat/include")
endif()

idf_component_register(
    SRCS ${LIBTRACER_SRCS}
    INCLUDE_DIRS
        "${LIBTRACER_ROOT}/core/include"
        "include"
    PRIV_INCLUDE_DIRS ${LIBTRACER_PRIV_INCLUDE_DIRS}
    PRIV_REQUIRES ${LIBTRACER_PRIV_REQUIRES}
)

# pthread is a PRIVATE requirement, not a public (REQUIRES) one. libtracer's
# *public* headers expose only libstdc++ headers (<atomic>, <mutex>,
# <shared_mutex>, <thread>) — never ESP-IDF's <pthread.h> — so the component
# does not propagate a pthread dependency to its dependents. The pthread symbols
# (pthread_create, etc.) are pulled in by libstdc++'s gthread threading, which
# the *.cpp objects need at link time; that's exactly what PRIV_REQUIRES models.
# (The old `REQUIRES pthread` over-declared it as a public dependency.)

# libtracer's core is C++23. ESP-IDF compiles component sources with the project's
# C++ standard; pin it here so the component is correct regardless of project default.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)

# The sole process-global mutable buffer libtracer links is the vertex lock-stripe
# table (core/include/libtracer/vertex.hpp, #361 §2). It is sized by
# LIBTRACER_VERTEX_LOCK_STRIPES; expose that as the Kconfig knob
# CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES (see Kconfig) so a chip node can shrink it
# (fewer FreeRTOS mutexes/condvars) without patching the core header.
#
# PUBLIC, not PRIVATE: the table is an `inline constinit` object, so EVERY TU that
# includes <libtracer/vertex.hpp> — the component's own sources AND every dependent
# — must compile with the same value, or the differing std::array sizes are an ODR
# violation. A PUBLIC definition propagates through `REQUIRES libtracer` to all
# dependents.
#
# CONFIG_* is defined only in the SECOND (build) CMake pass, so guard the reference:
# in the early requirement-expansion pass the symbol is unset and the header's own
# `#ifndef LIBTRACER_VERTEX_LOCK_STRIPES` default (16) applies. The Kconfig default
# (also 16) means the value passed matches the header at stock settings — no
# footprint change unless a project lowers it in menuconfig.
if(CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC
        "LIBTRACER_VERTEX_LOCK_STRIPES=${CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES}")
endif()

# Ship the component archive as `libtracer.a`, not the double-prefixed
# `liblibtracer.a` that ESP-IDF's `lib${COMPONENT_NAME}.a` convention produces for
# a component literally named `libtracer`. This only renames the output file; the
# component NAME stays `libtracer` (so `REQUIRES libtracer`, the idf_component.yml
# registry entry, and the docs are all unchanged) and IDF links by CMake target,
# not by archive file name, so nothing in the link graph is affected. The
# esp-idf-size footprint gate matches the archive by the substring `libtracer`
# (tools/esp_size_gate.py --component-pattern), which still hits `libtracer.a`.
set_target_properties(${COMPONENT_LIB} PROPERTIES OUTPUT_NAME tracer)
