# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

cmake_minimum_required(VERSION 3.16)

# Version — one source of truth, no hardcoded literal to drift. Resolution order:
#   1. the release git tag (`vMAJOR.MINOR.PATCH`), when building at a tag, so a
#      tagged release reports its exact version;
#   2. otherwise the repo-root `VERSION` file — the single hand-edited version for
#      the core release axis, mirrored into the package manifests by
#      `tools/sync-version.py` (CI-gated by version-consistency.yml);
#   3. a last-resort literal, reached only by a vendored `core/` copied away from
#      the repo root (no `VERSION` file present).
# The protocol/wire version is a separate axis, pinned by docs/spec/v1.md.
set(_libtracer_version "")
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --match "v[0-9]*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _libtracer_git_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _libtracer_git_result)
    if(_libtracer_git_result EQUAL 0 AND _libtracer_git_tag)
        # Strip the leading `v`; project() wants a bare MAJOR.MINOR.PATCH.
        string(REGEX REPLACE "^v" "" _libtracer_version "${_libtracer_git_tag}")
    endif()
endif()
if(NOT _libtracer_version AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" _libtracer_version_file)
    string(STRIP "${_libtracer_version_file}" _libtracer_version)
endif()
if(NOT _libtracer_version)
    # Last resort: a `core/` tree vendored away from the repo-root VERSION file.
    set(_libtracer_version 0.3.0)
endif()

project(libtracer VERSION ${_libtracer_version} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Per-module build options — the project's own "a node is a chosen set of modules"
# design. Each option toggles whether its module's translation unit(s) are COMPILED
# into libtracer; it is NOT a compile-definition, so a dropped module leaves neither
# code nor a symbol behind. Selection is by which TUs compile (the no-feature-macro
# doctrine, cf. socketcan_link.cpp vs. its stub, picked by CMAKE_SYSTEM_NAME). All
# default ON, so the default build is the full node — modularity is opt-OUT.
#
#   * The required core (below) ALWAYS compiles: the L2/L3 wire codec (frame +
#     tlv_arena), the L0/L1 substrate (backends + view/rope), path, and the L4 graph
#     runtime. A pure in-process node needs nothing more.
#   * LIBTRACER_NET_PLANE adds the FWD routing plane (op_resolve, route_handle,
#     fwd_router, transport_vertex) — a node that forwards to other nodes.
#   * LIBTRACER_TRANSPORT_{TCP,UDP,WS,CAN} each add their transport's TU(s), plus
#     shared deps (posix_endpoint when any socket transport is on; socketcan_link when
#     CAN is on). udp/tcp/ws are the net-plane builtins auto-registered on the
#     transport vertex; can/quic/webtransport are opt-in external factories.
option(LIBTRACER_NET_PLANE
       "Build the FWD net/routing plane (op_resolve, route_handle, fwd_router, transport_vertex)"
       ON)
option(LIBTRACER_TRANSPORT_TCP "Build the TCP stream transport (tcp_transport_t)" ON)
option(LIBTRACER_TRANSPORT_UDP "Build the UDP datagram transport (udp_transport_t)" ON)
option(LIBTRACER_TRANSPORT_WS "Build the WebSocket transport (transport_ws_*)" ON)
option(LIBTRACER_TRANSPORT_CAN "Build the CAN transport (transport_can + socketcan_link)" ON)

# The required core (always compiled): L2/L3 wire codec + L0/L1 substrate + path + L4
# graph runtime, plus the in-process loopback transport (dev/test, no dependencies; a
# tracer.hpp umbrella member).
add_library(libtracer STATIC
    src/frame.cpp
    src/tlv_arena.cpp
    src/backend_set.cpp
    src/mem_heap.cpp
    src/mem_pool.cpp
    src/rope.cpp
    src/rope_decode.cpp
    src/tlv_view.cpp
    src/path.cpp
    src/graph.cpp
    src/loopback.cpp
)

# The net/routing plane (LIBTRACER_NET_PLANE). The graph runtime above works without it
# for a pure in-process node.
if(LIBTRACER_NET_PLANE)
    target_sources(libtracer PRIVATE
        src/op_resolve.cpp
        src/op_resolve_view.cpp
        src/route_handle.cpp
        src/fwd_router.cpp
        src/transport_vertex.cpp
    )
    # The built-in transport-factory registrations transport_vertex_t wires up. One glue
    # TU per enabled builtin (udp/tcp/ws), plus a dispatcher (register_builtin_transports)
    # that calls exactly the enabled ones: the hand-written src/builtin_transports.cpp for
    # a FULL node (also what PlatformIO and the ESP-IDF component compile), else a
    # CMake-generated variant naming only the enabled register_*_transport. No preprocessor
    # macros — a disabled transport contributes neither its glue TU nor a call, so nothing
    # references a dropped transport (the decoupling that lets a transport TU be omitted).
    set(_libtracer_builtin_calls "")
    if(LIBTRACER_TRANSPORT_UDP)
        target_sources(libtracer PRIVATE src/builtin_transport_udp.cpp)
        string(APPEND _libtracer_builtin_calls "    register_udp_transport(vertex, rx_backend);\n")
    endif()
    if(LIBTRACER_TRANSPORT_TCP)
        target_sources(libtracer PRIVATE src/builtin_transport_tcp.cpp)
        string(APPEND _libtracer_builtin_calls "    register_tcp_transport(vertex, rx_backend);\n")
    endif()
    if(LIBTRACER_TRANSPORT_WS)
        target_sources(libtracer PRIVATE src/builtin_transport_ws.cpp)
        string(APPEND _libtracer_builtin_calls "    register_ws_transport(vertex, rx_backend);\n")
    endif()
    if(LIBTRACER_TRANSPORT_UDP AND LIBTRACER_TRANSPORT_TCP AND LIBTRACER_TRANSPORT_WS)
        target_sources(libtracer PRIVATE src/builtin_transports.cpp)
    else()
        set(LIBTRACER_BUILTIN_TRANSPORT_CALLS "${_libtracer_builtin_calls}")
        configure_file(
            ${CMAKE_CURRENT_SOURCE_DIR}/src/builtin_transports.cpp.in
            ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_transports.cpp
            @ONLY)
        target_sources(libtracer PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_transports.cpp)
    endif()
endif()

# Socket transports — tcp/udp/ws share posix_endpoint_t (the recv-thread/endpoint
# scaffold), compiled when ANY of them is on.
if(LIBTRACER_TRANSPORT_TCP OR LIBTRACER_TRANSPORT_UDP OR LIBTRACER_TRANSPORT_WS)
    target_sources(libtracer PRIVATE src/posix_endpoint.cpp)
endif()
if(LIBTRACER_TRANSPORT_TCP)
    target_sources(libtracer PRIVATE src/transport_tcp.cpp)
endif()
if(LIBTRACER_TRANSPORT_UDP)
    target_sources(libtracer PRIVATE src/transport_udp.cpp)
endif()
if(LIBTRACER_TRANSPORT_WS)
    target_sources(libtracer PRIVATE src/transport_ws.cpp)
endif()

# CAN transport (portable framing/reassembly) + its can_link_t platform binding.
# socketcan_link_t — platform TU selection (no in-source #ifdefs): the Linux PF_CAN
# implementation on Linux, the always-off stub everywhere else. Platform ports (e.g.
# the ESP-IDF component's twai_link_t) implement the same can_link_t seam in their tree.
if(LIBTRACER_TRANSPORT_CAN)
    target_sources(libtracer PRIVATE src/transport_can.cpp)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(libtracer PRIVATE src/socketcan_link.cpp)
    else()
        target_sources(libtracer PRIVATE src/socketcan_link_stub.cpp)
    endif()
endif()
# Namespaced alias so a build-tree consumer (add_subdirectory / FetchContent) links
# the SAME `libtracer::libtracer` name that `find_package(libtracer)` provides from
# the installed package — one target spelling regardless of how libtracer was pulled in.
add_library(libtracer::libtracer ALIAS libtracer)
find_package(Threads REQUIRED)
target_link_libraries(libtracer PUBLIC Threads::Threads)
target_include_directories(libtracer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(libtracer PUBLIC cxx_std_23)
# The CMake target is `libtracer`, but the artifact ships as `libtracer.a` (not the
# double-prefixed `liblibtracer.a`) so a non-CMake downstream links it with the
# conventional `-ltracer`. CMake consumers use the imported `libtracer::libtracer`
# target and are unaffected by the file name. (The ESP-IDF component archive stays
# `liblibtracer.a` — that name is fixed by the IDF component build, a separate tree.)
set_target_properties(libtracer PROPERTIES OUTPUT_NAME tracer)
if(MSVC)
    target_compile_options(libtracer PRIVATE /W4)
else()
    target_compile_options(libtracer PRIVATE -Wall -Wextra)
endif()

# mem_cuda — the GPU device backend (docs/adr/0024). OFF by default: it needs the
# CUDA toolkit and is built/tested locally (e.g. tools/test-cuda.sh), never in
# CI (no GPU). The .cpp uses only the CUDA runtime API (no kernels), so a host
# compiler + libcudart suffices — no nvcc required.
# security_acl — the ACL policy seam (docs/adr/0050). The policy is selected at
# build time per target (ADR-0047 §1): OFF (default) = the ALLOW-only MCU
# profile; ON = the full host policy (ordered first-match-per-bit with DENY).
# Both adapters are always compiled and unit-tested (security_acl_test); this
# switch only picks which one graph_t evaluates and how strict :acl parse is.
option(LIBTRACER_ACL_FULL "Evaluate ACLs with the full security_acl host policy (DENY, ordered)"
       OFF)
if(LIBTRACER_ACL_FULL)
    target_compile_definitions(libtracer PUBLIC LIBTRACER_ACL_FULL)
endif()

option(LIBTRACER_WITH_CUDA "Build the mem_cuda GPU backend (needs CUDA toolkit)" OFF)
if(LIBTRACER_WITH_CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_sources(libtracer PRIVATE src/mem_cuda.cpp)
    target_link_libraries(libtracer PUBLIC CUDA::cudart)
    target_compile_definitions(libtracer PUBLIC LIBTRACER_WITH_CUDA)
endif()

# libtracer_quic — the QUIC transport MODULE (docs/adr/0043 Phase A), a SEPARATE
# library target: a host that talks QUIC links libtracer_quic; a host that
# doesn't simply never compiles these sources — no feature #ifdefs, no compile
# definitions, nothing in libtracer itself references msquic. The module extends
# the transport catalog through the existing register_transport_type seam
# (quic_transport_factory), so transport_vertex stays closed for modification.
# ADR-0043 Phase B adds webtransport_transport_t to the SAME target (one msquic
# investment serves both — QUIC is the substrate WebTransport requires): the
# WebTransport-over-HTTP/3 endpoint with its module-private minimal H3/QPACK
# layer (src/wt_h3.hpp — never installed, never referenced by core).
# LIBTRACER_WITH_QUIC only controls whether the target is CONFIGURED (msquic
# must be installed to build it; the dedicated quic CI job builds msquic from
# source). Lookup is find-first: the msquic CMake package config if installed,
# else a plain header+library search (point CMAKE_PREFIX_PATH at an msquic
# install prefix).
option(LIBTRACER_WITH_QUIC "Configure the libtracer_quic transport module (needs msquic)" OFF)
if(LIBTRACER_WITH_QUIC)
    find_package(msquic CONFIG QUIET)
    if(NOT TARGET msquic)
        find_library(MSQUIC_LIBRARY NAMES msquic)
        if(NOT MSQUIC_LIBRARY)
            message(FATAL_ERROR "LIBTRACER_WITH_QUIC=ON but msquic was not found. "
                "Install msquic (https://github.com/microsoft/msquic) and/or set "
                "CMAKE_PREFIX_PATH to its install prefix.")
        endif()
        add_library(msquic SHARED IMPORTED)
        set_target_properties(msquic PROPERTIES IMPORTED_LOCATION "${MSQUIC_LIBRARY}")
    endif()
    # msquic's exported target (as of v2.4) carries no include-dir usage
    # requirement — locate msquic.h ourselves and attach it (PUBLIC: consumers
    # like quic_test include msquic.h directly for their raw test client).
    find_path(MSQUIC_INCLUDE_DIR msquic.h)
    if(NOT MSQUIC_INCLUDE_DIR)
        message(FATAL_ERROR "msquic.h not found — set CMAKE_PREFIX_PATH to the msquic install prefix.")
    endif()
    add_library(libtracer_quic STATIC
        src/transport_quic.cpp
        src/transport_webtransport.cpp
    )
    target_include_directories(libtracer_quic PUBLIC "${MSQUIC_INCLUDE_DIR}")
    target_link_libraries(libtracer_quic PUBLIC libtracer msquic)
    if(MSVC)
        target_compile_options(libtracer_quic PRIVATE /W4)
    else()
        target_compile_options(libtracer_quic PRIVATE -Wall -Wextra)
    endif()
endif()

# Install / export: make libtracer consumable with `find_package(libtracer)` and
# `target_link_libraries(app PRIVATE libtracer::libtracer)` — the release-hardening
# packaging seam (before this, a downstream could only source-vendor the tree).
# Scope: the default-build core static library only. The optional libtracer_quic
# module is intentionally NOT part of the installed package yet — it links an
# imported msquic target whose provenance (CMake package vs. bare library search)
# can't be faithfully reconstructed in an exported config; packaging it cleanly is
# its own follow-up. Gated on LIBTRACER_INSTALL (default: only when top-level), so
# an embedding parent (bench/, esp-idf) that add_subdirectory()s core never
# inherits install rules it didn't ask for.
option(LIBTRACER_INSTALL "Generate install/export rules for libtracer" ${PROJECT_IS_TOP_LEVEL})
if(LIBTRACER_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS libtracer
        EXPORT libtracerTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY include/libtracer
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    set(LIBTRACER_CMAKE_CONFIG_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/libtracer)

    install(EXPORT libtracerTargets
        FILE libtracerTargets.cmake
        NAMESPACE libtracer::
        DESTINATION ${LIBTRACER_CMAKE_CONFIG_DIR})

    # Dependencies the exported config must re-find. Threads is unconditional;
    # CUDAToolkit only when the GPU backend was compiled into this install (its
    # CUDA::cudart link is a usage requirement that must be reconstructable).
    set(LIBTRACER_PACKAGE_DEPENDENCIES "find_dependency(Threads)")
    if(LIBTRACER_WITH_CUDA)
        string(APPEND LIBTRACER_PACKAGE_DEPENDENCIES "\nfind_dependency(CUDAToolkit)")
    endif()

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libtracerConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/libtracerConfig.cmake
        INSTALL_DESTINATION ${LIBTRACER_CMAKE_CONFIG_DIR})

    # Pre-1.0: a 0.x minor bump may break the C++ API (per CHANGELOG), so match on
    # minor, not major — a consumer that asks for 0.3 won't silently accept 0.4.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/libtracerConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/libtracerConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/libtracerConfigVersion.cmake
        DESTINATION ${LIBTRACER_CMAKE_CONFIG_DIR})
endif()

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()

# M3: the in-process pub/sub example (the P0 node end to end). Built by default
# when libtracer is the top-level project; runs as a smoke test under CTest.
option(LIBTRACER_BUILD_EXAMPLES "Build the core examples" ${PROJECT_IS_TOP_LEVEL})
if(LIBTRACER_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
