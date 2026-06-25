# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

cmake_minimum_required(VERSION 3.16)
project(libtracer_bench CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)  # -O3 for both sides; a fair comparison
endif()

# Build libtracer from ../core (its tests/examples self-gate on PROJECT_IS_TOP_LEVEL).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_BINARY_DIR}/core)

add_executable(bench_libtracer bench_libtracer.cpp)
target_link_libraries(bench_libtracer PRIVATE libtracer)
target_compile_options(bench_libtracer PRIVATE -Wall -Wextra)

# Zenoh side — built only when the prebuilt is vendored (run bench/fetch_zenoh.sh).
set(ZC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/zenoh-c)
set(ZCPP ${CMAKE_CURRENT_SOURCE_DIR}/vendor/zenoh-cpp-1.9.0)
if(EXISTS ${ZC}/lib/libzenohc.so AND EXISTS ${ZCPP}/include/zenoh.hxx)
    add_executable(bench_zenoh bench_zenoh.cpp)
    target_include_directories(bench_zenoh PRIVATE ${ZC}/include ${ZCPP}/include)
    target_link_libraries(bench_zenoh PRIVATE ${ZC}/lib/libzenohc.so)
    target_compile_definitions(bench_zenoh PRIVATE ZENOHCXX_ZENOHC)
    set_target_properties(bench_zenoh PROPERTIES BUILD_RPATH ${ZC}/lib INSTALL_RPATH ${ZC}/lib)
else()
    message(STATUS "bench: zenoh not vendored; skipping bench_zenoh (run bench/fetch_zenoh.sh)")
endif()
