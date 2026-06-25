# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

cmake_minimum_required(VERSION 3.16)
project(libtracer CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# M1: the L2/L3 wire codec (frame + CRC + TLV model).
# M2: the L0/L1 substrate (segment refcount + backends + view/rope). The L4
# in-process graph runtime follows (see README.md).
add_library(libtracer STATIC
    src/frame.cpp
    src/mem_heap.cpp
    src/mem_pool.cpp
    src/rope.cpp
)
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

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    add_subdirectory(tests)
endif()
