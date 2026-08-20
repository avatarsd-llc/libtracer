# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# backends/cuda — the GPU device backend as a TIER MODULE (#1381, docs/adr/0024
# Amendment 1). A standalone CMake project that CONSUMES core, the same shape
# bench/CMakeLists.txt uses: core's own CMake never names this directory, so the
# dependency arrow points up the tiers only and there is nothing in core to grep for.
#
# The module plugs into core through `tr::mem::register_device_backend` — the L0 mirror
# of `transport_vertex_t::register_transport_type` that libtracer_quic uses. There is no
# `#ifdef` in core, no compile definition reaching consumers, and adding a second vendor
# (ROCm, NPU, dmabuf) means adding a sibling directory here, not an enumerator to core.
#
#   cmake -S backends/cuda -B build/cuda -DBUILD_TESTING=ON
#   cmake --build build/cuda -j && ctest --test-dir build/cuda
#
# The .cpp uses the CUDA *runtime* API only (no kernels), so a host C++ compiler plus
# libcudart builds it — no nvcc. `tools/test-cuda.sh` does exactly that in a container
# with the local GPU attached; CI has no GPU and never builds this.
cmake_minimum_required(VERSION 3.16)
project(libtracer_cuda CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BUILD_TESTING (default ON) + enable_testing(), before the core subdirectory so core sees
# the same answer. Core's own tests self-gate on PROJECT_IS_TOP_LEVEL, which is false here,
# so this turns on THIS project's test only.
include(CTest)

# Build libtracer from ../../core (its tests/examples self-gate on PROJECT_IS_TOP_LEVEL).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core ${CMAKE_BINARY_DIR}/core)

find_package(CUDAToolkit REQUIRED)

add_library(libtracer_cuda STATIC src/mem_cuda.cpp)
target_include_directories(libtracer_cuda PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(libtracer_cuda PUBLIC libtracer CUDA::cudart)

# Same warning profile the libtracer / libtracer_quic targets carry: -Werror=switch so a
# new `mem_space_t` / `io_dir_t` enumerator is a red build here too, not a silent default.
if(MSVC)
    target_compile_options(libtracer_cuda PRIVATE /W4 /we4062)
else()
    target_compile_options(libtracer_cuda PRIVATE -Wall -Wextra -Werror=switch)
endif()

# The GPU test. It needs a real device, so it is never added by CI — `ctest` here is run
# by hand (or by tools/test-cuda.sh) on a host that has one.
if(BUILD_TESTING)
    add_executable(cuda_test tests/cuda_test.cpp)
    target_link_libraries(cuda_test PRIVATE libtracer_cuda)
    # test_support.hpp — the ONE shared runner the whole test tree reports through (#874).
    target_include_directories(cuda_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../core/tests)
    if(MSVC)
        target_compile_options(cuda_test PRIVATE /W4)
    else()
        target_compile_options(cuda_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME cuda COMMAND cuda_test)
endif()
