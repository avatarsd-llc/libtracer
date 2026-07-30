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
    "${LIBTRACER_ROOT}/core/src/mem_source.cpp"
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
)

# --- Built-in transport modules (#393) — per-module gating, mirroring the core CMake
# options LIBTRACER_TRANSPORT_{UDP,TCP,WS}. Each transport's glue factory + socket TU
# compile ONLY when its Kconfig bool is on; all default y, so a stock node is
# byte-identical to before. Turning one off sheds its TUs from libtracer.a and makes a
# SPEC of that `kind` resolve to SCHEMA_NOT_FOUND — the RAM-audit "compose the C6 for
# its role" lever, with NO in-source feature macros (selection is which TUs compile).
#
# NB the two-pass model: CONFIG_* is UNDEFINED in IDF's early requirement-expansion
# pass, so these guards read as OFF there. That is harmless — SRCS are consumed only in
# the second (build) pass, where sdkconfig (default y) is loaded. The transport
# PRIV_REQUIRES below therefore stay UNCONDITIONAL, for the same reason a Kconfig-gated
# REQUIRES never propagates (it evaluates false in the early pass).
set(LIBTRACER_BUILTIN_TRANSPORT_CALLS "")
if(CONFIG_LIBTRACER_TRANSPORT_UDP)
    list(APPEND LIBTRACER_SRCS
        "${LIBTRACER_ROOT}/core/src/builtin_transport_udp.cpp"
        "${LIBTRACER_ROOT}/core/src/transport_udp.cpp")
    string(APPEND LIBTRACER_BUILTIN_TRANSPORT_CALLS
        "    register_udp_transport(vertex, rx_backend);\n")
endif()
if(CONFIG_LIBTRACER_TRANSPORT_TCP)
    list(APPEND LIBTRACER_SRCS
        "${LIBTRACER_ROOT}/core/src/builtin_transport_tcp.cpp"
        "${LIBTRACER_ROOT}/core/src/transport_tcp.cpp")
    string(APPEND LIBTRACER_BUILTIN_TRANSPORT_CALLS
        "    register_tcp_transport(vertex, rx_backend);\n")
endif()
if(CONFIG_LIBTRACER_TRANSPORT_WS)
    list(APPEND LIBTRACER_SRCS
        "${LIBTRACER_ROOT}/core/src/builtin_transport_ws.cpp"
        "${LIBTRACER_ROOT}/core/src/transport_ws.cpp")
    string(APPEND LIBTRACER_BUILTIN_TRANSPORT_CALLS
        "    register_ws_transport(vertex, rx_backend);\n")
endif()

# posix_endpoint_t — the recv-thread/endpoint scaffold shared by every socket transport.
if(CONFIG_LIBTRACER_TRANSPORT_UDP OR CONFIG_LIBTRACER_TRANSPORT_TCP OR CONFIG_LIBTRACER_TRANSPORT_WS)
    list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/posix_endpoint.cpp")
endif()

# register_builtin_transports dispatcher: the hand-written full-node form (udp+tcp+ws)
# when all three are on — identical to a stock node — else a CMake-generated variant
# from src/builtin_transports.cpp.in that calls ONLY the enabled register_*_transport, so
# a dropped transport leaves neither a glue TU nor a dangling call (same mechanism as
# core/CMakeLists.txt).
if(CONFIG_LIBTRACER_TRANSPORT_UDP AND CONFIG_LIBTRACER_TRANSPORT_TCP AND CONFIG_LIBTRACER_TRANSPORT_WS)
    list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/builtin_transports.cpp")
else()
    configure_file(
        "${LIBTRACER_ROOT}/core/src/builtin_transports.cpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_transports.cpp"
        @ONLY)
    list(APPEND LIBTRACER_SRCS "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_transports.cpp")
endif()

# lwip is a PRIVATE requirement of the socket transports' .cpp files only
# (<sys/socket.h> etc. resolve into lwIP on chip targets); the public headers
# expose no lwIP types. On the `linux` target the same includes resolve into
# glibc, so no lwip component is needed (and IDF does not provide one there).
set(LIBTRACER_PRIV_REQUIRES pthread)
set(LIBTRACER_PRIV_INCLUDE_DIRS "")

# CAN transport (#393-gated): portable framing/reassembly (transport_can.cpp) + its
# can_link_t platform binding — the real SocketCAN link on the linux host (glibc ships
# <linux/can.h>), a SocketCAN stub + the TWAI-backed can_link_t (twai_link.cpp) on chip
# targets. Which link compiles is a TU choice, never an in-source #ifdef. Off => the
# whole CAN plane (and the TWAI driver glue) is dropped from the archive.
if(CONFIG_LIBTRACER_TRANSPORT_CAN)
    list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/transport_can.cpp")
    if(IDF_TARGET STREQUAL "linux")
        list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/socketcan_link.cpp")
    else()
        list(APPEND LIBTRACER_SRCS
            "${LIBTRACER_ROOT}/core/src/socketcan_link_stub.cpp"
            "twai_link.cpp")
    endif()
endif()

if(NOT IDF_TARGET STREQUAL "linux")
    # Chip targets: the esp_http_server-backed WS transport link (httpd_ws_link_t) — the
    # embedded-native replacement for core's raw-socket transport_ws_server, a platform
    # link picked by which TU compiles (never an in-source #ifdef). transport_ws.cpp is in
    # the WS block above: it compiles but a node that binds this httpd link instead
    # references it nowhere, so --gc-sections drops it (the linux VB, which HAS no
    # esp_http_server, keeps it). Gated on BOTH the WS module (#393) and
    # CONFIG_HTTPD_WS_SUPPORT (the TU rides esp_http_server's native WS API, which the
    # component ships only when that config is on).
    if(CONFIG_LIBTRACER_TRANSPORT_WS AND CONFIG_HTTPD_WS_SUPPORT)
        list(APPEND LIBTRACER_SRCS "httpd_ws_link.cpp")
    endif()
    # esp_driver_gpio (twai_link.cpp names its pins via <driver/gpio.h>), esp_driver_twai
    # (the TWAI peripheral) and esp_http_server (httpd_ws_link.cpp's native WS API) MUST be
    # UNCONDITIONAL requirements — NOT gated on the transport bools or CONFIG_HTTPD_WS_SUPPORT.
    # IDF resolves component requirements in an early-expansion pass where CONFIG_* symbols
    # are not yet defined, so a Kconfig-gated REQUIRES silently evaluates false there and
    # never propagates the component's include dir; the TU (correctly gated on the same
    # config in the second pass) then compiles WITHOUT its header and dies with "No such
    # file" on any node that enables it (the origin firmware's prod build hit exactly this).
    # These headers are base IDF components present on every chip target, so requiring them
    # always is safe and free when their TU is absent — nothing references the symbols, so
    # --gc-sections drops the link dep and libtracer.a is byte-identical (no footprint change
    # to a transport-trimmed build the sentinel measures). lwip stays unconditional for the
    # same reason (the socket transports' <sys/socket.h> resolves into it).
    list(APPEND LIBTRACER_PRIV_REQUIRES lwip esp_driver_twai esp_driver_gpio esp_http_server)
    # compat/include: POSIX headers IDF's libc spells differently (<poll.h> ->
    # <sys/poll.h>) — private, so core sources stay pure POSIX and dependents
    # never see the shim.
    list(APPEND LIBTRACER_PRIV_INCLUDE_DIRS "compat/include")
endif()

# libtracer/config.hpp — per-target compile-time knobs delivered as plain C++
# (ADR-0068). Generated here from the Kconfig values and listed BEFORE core/include,
# so the generated file shadows the checked-in default for the component's own
# sources AND every dependent (`REQUIRES libtracer`) — one file per build means every
# TU agrees, which is what the old PUBLIC compile definition existed to guarantee.
# CONFIG_* is defined only in the SECOND (build) CMake pass; in the early
# requirement-expansion pass the fallback renders the stock default (16), which the
# second pass then overwrites with the real value before anything compiles.
if(CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES)
    set(LIBTRACER_VERTEX_LOCK_STRIPES ${CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES})
else()
    set(LIBTRACER_VERTEX_LOCK_STRIPES 16)
endif()
set(LIBTRACER_ACL_POLICY allow_only_policy_t)  # the MCU profile — no full-ACL Kconfig yet
set(LIBTRACER_LKV_SLOT sp_atomic_slot_t)  # write-dominated single core: keep the refcount slot
set(LIBTRACER_HAZARD_READER_SLOTS 64)  # unused here: the refcount slot never builds the domain
# False-sharing padding: DERIVED, not a Kconfig option. A unicore chip (C2/C3/C6/H2, or a
# dual-core built with CONFIG_FREERTOS_UNICORE) has no second core to false-share with, so
# the padding is pure static RAM loss — measured 896 B of .bss for 16 stripes on rv32 -Os.
# Asking the integrator would only invite the wrong answer: the chip already knows.
if(CONFIG_FREERTOS_UNICORE)
    set(LIBTRACER_CACHE_LINE_BYTES 0)
else()
    set(LIBTRACER_CACHE_LINE_BYTES 64)
endif()
configure_file(
    "${LIBTRACER_ROOT}/core/include/libtracer/config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/libtracer/config.hpp"
    @ONLY)

idf_component_register(
    SRCS ${LIBTRACER_SRCS}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_BINARY_DIR}/generated/include"
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

# The vertex lock-stripe count (CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES, see Kconfig) now
# rides the generated config.hpp above — it reaches every TU as the ordinary constexpr
# `tr::graph::kVertexLockStripes` through ONE shared header, so the old PUBLIC compile
# definition (and the ODR essay that justified it) is gone (ADR-0068).

# Ship the component archive as `libtracer.a`, not the double-prefixed
# `liblibtracer.a` that ESP-IDF's `lib${COMPONENT_NAME}.a` convention produces for
# a component literally named `libtracer`. This only renames the output file; the
# component NAME stays `libtracer` (so `REQUIRES libtracer`, the idf_component.yml
# registry entry, and the docs are all unchanged) and IDF links by CMake target,
# not by archive file name, so nothing in the link graph is affected. The
# esp-idf-size footprint gate matches the archive by the substring `libtracer`
# (tools/esp_size_gate.py --component-pattern), which still hits `libtracer.a`.
set_target_properties(${COMPONENT_LIB} PROPERTIES OUTPUT_NAME tracer)
