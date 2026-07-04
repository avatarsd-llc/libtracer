# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

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

# Per-vector codec roundtrip bench — the `lang` axis (cpp-core) of the cross-core
# perf matrix (ADR-0032 / #96). Emits the same 12-field RESULT contract as the TS
# / Rust codec benches; defaults to the in-repo v1 conformance vectors.
add_executable(bench_codec bench_codec.cpp)
target_link_libraries(bench_codec PRIVATE libtracer)
target_compile_options(bench_codec PRIVATE -Wall -Wextra)
target_compile_definitions(bench_codec PRIVATE
    LIBTRACER_BENCH_DEFAULT_VECTORS="${CMAKE_CURRENT_SOURCE_DIR}/../tests/conformance/vectors/v1")

# Two-process network bench (real UDP). pub/sub roles; see run_net.sh.
# bench_libtracer_net (two-process ROUTER-flood bench) was retired with bridge_t
# (ADR-0040 — the net plane is explicit-source-routed FWD only). Two-node FWD delivery
# over a real transport is covered by core/tests/udp_test.

# Scatter-gather egress demo: one sendmsg(iovec) ships a K-value composite rope.
add_executable(bench_scatter bench_scatter.cpp)
target_link_libraries(bench_scatter PRIVATE libtracer)
target_compile_options(bench_scatter PRIVATE -Wall -Wextra)

# The 16KB-RAM zero-heap forward-path gate (ADR-0038): a global operator-new counter
# measures how many heap allocations one FWD forward hop costs. Report-only now (the
# Stage-1 baseline is non-zero); CI flips it to a hard gate (ZEROHEAP_MAX=0) at Stage-2.
add_executable(bench_forward_heap bench_forward_heap.cpp)
target_link_libraries(bench_forward_heap PRIVATE libtracer)
target_compile_options(bench_forward_heap PRIVATE -Wall -Wextra)

# Many-core contention microbenchmarks (Wave 0e; the ADR-0032 measurement-first
# answer to the 128-core scaling question). bench_fanout_clone_storm measures
# segment-refcount cacheline contention under wide fan-out; bench_await_wakeup_storm
# measures condvar/await fan-in scaling. DIAGNOSTIC ONLY — thread-contention numbers
# are runner-dependent, so these are NOT wired into perf.yml's regression gate; run
# them on the real many-core host (the nightly row).
add_executable(bench_fanout_clone_storm bench_fanout_clone_storm.cpp)
target_link_libraries(bench_fanout_clone_storm PRIVATE libtracer)
target_compile_options(bench_fanout_clone_storm PRIVATE -Wall -Wextra)

add_executable(bench_await_wakeup_storm bench_await_wakeup_storm.cpp)
target_link_libraries(bench_await_wakeup_storm PRIVATE libtracer)
target_compile_options(bench_await_wakeup_storm PRIVATE -Wall -Wextra)

# Zenoh side — built only when the prebuilt is vendored (run bench/fetch_zenoh.sh).
set(ZC ${CMAKE_CURRENT_SOURCE_DIR}/vendor/zenoh-c)
set(ZCPP ${CMAKE_CURRENT_SOURCE_DIR}/vendor/zenoh-cpp-1.9.0)
if(EXISTS ${ZC}/lib/libzenohc.so AND EXISTS ${ZCPP}/include/zenoh.hxx)
    add_executable(bench_zenoh bench_zenoh.cpp)
    add_executable(bench_zenoh_net bench_zenoh_net.cpp)
    foreach(t bench_zenoh bench_zenoh_net)
        target_include_directories(${t} PRIVATE ${ZC}/include ${ZCPP}/include)
        target_link_libraries(${t} PRIVATE ${ZC}/lib/libzenohc.so)
        target_compile_definitions(${t} PRIVATE ZENOHCXX_ZENOHC)
        set_target_properties(${t} PROPERTIES BUILD_RPATH ${ZC}/lib INSTALL_RPATH ${ZC}/lib)
    endforeach()
else()
    message(STATUS "bench: zenoh not vendored; skipping zenoh benches (run bench/fetch_zenoh.sh)")
endif()
