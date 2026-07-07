# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

cmake_minimum_required(VERSION 3.16)

# Version: derived from the git tag (`vMAJOR.MINOR.PATCH`) — the single source of
# truth, so there is no hardcoded version to drift (the protocol/wire version is
# separate and pinned by docs/spec/v1.md). A build outside a git checkout, or
# before the first release tag, falls back to the in-development version below.
set(LIBTRACER_FALLBACK_VERSION 0.3.0)
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
if(NOT _libtracer_version)
    set(_libtracer_version ${LIBTRACER_FALLBACK_VERSION})
endif()

project(libtracer VERSION ${_libtracer_version} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# M1: the L2/L3 wire codec (frame + CRC + TLV model).
# M2: the L0/L1 substrate (segment refcount + backends + view/rope).
# M3: the L4 in-process graph runtime (path + vertex + read/write/await).
# M4: the first transport + bridge (loopback transport, ROUTER envelope, dedup).
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
    src/op_resolve.cpp
    src/op_resolve_view.cpp
    src/route_handle.cpp
    src/fwd_router.cpp
    src/transport_vertex.cpp
    src/loopback.cpp
    src/posix_endpoint.cpp
    src/transport_tcp.cpp
    src/transport_udp.cpp
    src/transport_ws.cpp
    src/transport_can.cpp
)
# socketcan_link_t — platform TU selection (no in-source #ifdefs): the Linux
# PF_CAN implementation on Linux, the always-off stub everywhere else. Platform
# ports (e.g. the ESP-IDF component's twai_link_t) implement the same can_link_t
# seam in their own tree.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(libtracer PRIVATE src/socketcan_link.cpp)
else()
    target_sources(libtracer PRIVATE src/socketcan_link_stub.cpp)
endif()
find_package(Threads REQUIRED)
target_link_libraries(libtracer PUBLIC Threads::Threads)
target_include_directories(libtracer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(libtracer PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(libtracer PRIVATE /W4)
else()
    target_compile_options(libtracer PRIVATE -Wall -Wextra)
endif()

# mem_cuda — the GPU device backend (docs/adr/0024). OFF by default: it needs the
# CUDA toolkit and is built/tested locally (e.g. tools/test-cuda.sh), never in
# CI (no GPU). The .cpp uses only the CUDA runtime API (no kernels), so a host
# compiler + libcudart suffices — no nvcc required.
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
