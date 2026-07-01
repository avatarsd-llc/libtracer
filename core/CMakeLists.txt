# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

cmake_minimum_required(VERSION 3.16)
project(libtracer CXX)

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
    src/mem_heap.cpp
    src/mem_pool.cpp
    src/rope.cpp
    src/path.cpp
    src/graph.cpp
    src/op_resolve.cpp
    src/route_handle.cpp
    src/fwd_router.cpp
    src/transport_vertex.cpp
    src/loopback.cpp
    src/transport_udp.cpp
    src/transport_ws.cpp
    src/transport_can.cpp
)
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
# CUDA toolkit and is built/tested locally (e.g. scripts/test-cuda.sh), never in
# CI (no GPU). The .cpp uses only the CUDA runtime API (no kernels), so a host
# compiler + libcudart suffices — no nvcc required.
option(LIBTRACER_WITH_CUDA "Build the mem_cuda GPU backend (needs CUDA toolkit)" OFF)
if(LIBTRACER_WITH_CUDA)
    find_package(CUDAToolkit REQUIRED)
    target_sources(libtracer PRIVATE src/mem_cuda.cpp)
    target_link_libraries(libtracer PUBLIC CUDA::cudart)
    target_compile_definitions(libtracer PUBLIC LIBTRACER_WITH_CUDA)
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
