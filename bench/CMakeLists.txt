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

# NETWORK transport comparison (UDP/TCP/WS over the loopback kernel path) — the
# libtracer side of the transport matrix vs Zenoh (bench_zenoh_net). Two PROCESSES
# (pub|sub role arg), same topology as the Zenoh bench; emits mode `net-<proto>`.
# Orchestrated per protocol by run_net.sh.
add_executable(bench_transports bench_transports.cpp)
target_link_libraries(bench_transports PRIVATE libtracer)
target_compile_options(bench_transports PRIVATE -Wall -Wextra)

# The 16KB-RAM zero-heap forward-path gate (ADR-0038): a global operator-new counter
# measures how many heap allocations one FWD forward hop costs. Report-only now (the
# Stage-1 baseline is non-zero); CI flips it to a hard gate (ZEROHEAP_MAX=0) at Stage-2.
add_executable(bench_forward_heap bench_forward_heap.cpp)
target_link_libraries(bench_forward_heap PRIVATE libtracer)
target_compile_options(bench_forward_heap PRIVATE -Wall -Wextra)

# The forward-demux baseline (ADR-0061's one open acceptance condition): times ONE FWD
# forward hop and separates the size-independent fixed cost (target first) from the
# registry scan's marginal cost (target last), swept over links-per-node. Landed against
# the flat by_name so it needs no strip-K code — it is the yardstick that PR must meet.
add_executable(bench_forward_demux bench_forward_demux.cpp)
target_link_libraries(bench_forward_demux PRIVATE libtracer)
target_compile_options(bench_forward_demux PRIVATE -Wall -Wextra)

# The ROPE forward arm (#668). bench_forward_demux drives the SPAN arm; nothing reached
# on_frame_rope, so changes there could only be argued structurally.
add_executable(bench_forward_rope bench_forward_rope.cpp)
target_link_libraries(bench_forward_rope PRIVATE libtracer)
target_compile_options(bench_forward_rope PRIVATE -Wall -Wextra)

# Terminus tier comparison (ADR-0053): the eager arena reader vs the lazy view reader on
# the SAME frame. Replaces argument with measurement on whether the single-link
# short-circuit in on_frame_rope_impl is a fast path or a missed one.
# Steady-state compacted delivery (ADR-0062): what an ESTABLISHED flow costs per frame —
# the case delivery compaction exists for, and the one no other bench times.
add_executable(bench_compact_delivery bench_compact_delivery.cpp)
target_link_libraries(bench_compact_delivery PRIVATE libtracer)
target_compile_options(bench_compact_delivery PRIVATE -Wall -Wextra)

add_executable(bench_originate bench_originate.cpp)
target_link_libraries(bench_originate PRIVATE libtracer)
target_compile_options(bench_originate PRIVATE -Wall -Wextra)

add_executable(bench_hop_chain bench_hop_chain.cpp)
target_link_libraries(bench_hop_chain PRIVATE libtracer)
target_compile_options(bench_hop_chain PRIVATE -Wall -Wextra)

add_executable(bench_terminus_tier bench_terminus_tier.cpp)
target_link_libraries(bench_terminus_tier PRIVATE libtracer)
target_compile_options(bench_terminus_tier PRIVATE -Wall -Wextra)

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

# route_handle per-link-lock contention: many producers doing steady-state
# ensure_egress (reuse reads) on one hot link. Diagnostic, not a perf gate.
add_executable(bench_route_handle_contention bench_route_handle_contention.cpp)
target_link_libraries(bench_route_handle_contention PRIVATE libtracer)
target_compile_options(bench_route_handle_contention PRIVATE -Wall -Wextra)

# The FWD router's RX failable-source topology (ADR-0067 3, #625): shared heap vs one
# shared pool vs one pool per child, swept over T receive threads. Diagnostic, not a perf
# gate — thread-contention numbers are runner-dependent.
add_executable(bench_rx_source_topology bench_rx_source_topology.cpp)
target_link_libraries(bench_rx_source_topology PRIVATE libtracer)
target_compile_options(bench_rx_source_topology PRIVATE -Wall -Wextra)

# The LKV slot under concurrency (ADR-0064 2, #604): today's atomic<shared_ptr> against a
# no-reclamation ceiling, epoch-based reclamation and hazard pointers, in three shapes, plus
# the same question on the real graph_t::write path. The ADR's own closing consequence names
# this bench as the prerequisite to attempting 2. Diagnostic, not a perf gate.
add_executable(bench_lkv_slot bench_lkv_slot.cpp)
target_link_libraries(bench_lkv_slot PRIVATE libtracer)
target_compile_options(bench_lkv_slot PRIVATE -Wall -Wextra)

# What one shared cache line costs, with libtracer entirely out of the picture: nine
# primitives (private counter, read-only share, 1/2/4 contended RMWs, shared_mutex read
# lock, mutex, atomic<shared_ptr> load, shared_ptr copy) swept across thread counts. The
# instrument behind docs/design/concurrency/ — committed so its constants are reproducible
# on the reader's own box, which is the point. Diagnostic, never a perf gate.
add_executable(bench_contention bench_contention.cpp)
target_link_libraries(bench_contention PRIVATE libtracer)
target_compile_options(bench_contention PRIVATE -Wall -Wextra)

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

# The scatter-gather width at which a REAL transport starts allocating — the term
# bench_forward_heap's stub link cannot see (its capture_transport_t never assembles an iovec).
add_executable(bench_transport_iov bench_transport_iov.cpp)
target_link_libraries(bench_transport_iov PRIVATE libtracer)
target_compile_options(bench_transport_iov PRIVATE -Wall -Wextra)
