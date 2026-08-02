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

# Per-frame heap allocations on the REAL wire, EGRESS and INGRESS. bench_transport_iov counts
# only the sending thread's gather; nothing has ever counted the receive loop. Thread-local
# counters attribute each side separately.
add_executable(bench_wire_heap bench_wire_heap.cpp)
target_link_libraries(bench_wire_heap PRIVATE libtracer)
target_compile_options(bench_wire_heap PRIVATE -Wall -Wextra)

# Aggregate throughput vs concurrent peer count — prices transport_tcp_server's SINGLE poll
# thread (the "MCU-shaped choice, #362") on a many-core host, which nothing else measures.
add_executable(bench_tcp_fanin bench_tcp_fanin.cpp)
target_link_libraries(bench_tcp_fanin PRIVATE libtracer)
target_compile_options(bench_tcp_fanin PRIVATE -Wall -Wextra)

# Resolves what bench_tcp_fanin cannot: its idle-fanout arm holds the work constant and sweeps
# CONNECTED-BUT-SILENT peers, so the O(peers) poll and the locked pollfd rebuild are priced apart
# from the thread's own saturation (ADR-0071).
# The census #617 turns on: which vertices carry an extension block, and how many of those
# hold QoS byte-identical to the default. Counted, never timed.
add_executable(bench_qos_census bench_qos_census.cpp)
target_link_libraries(bench_qos_census PRIVATE libtracer)
target_compile_options(bench_qos_census PRIVATE -Wall -Wextra)

add_executable(bench_tcp_peer_scaling bench_tcp_peer_scaling.cpp)
target_link_libraries(bench_tcp_peer_scaling PRIVATE libtracer)
target_compile_options(bench_tcp_peer_scaling PRIVATE -Wall -Wextra)

# Validates the tail estimator itself against analytically known distributions. Links nothing:
# bench::Latency is header-only, and the point is that a p999 failure here is a defect in the
# instrument and not in libtracer. Exit status is the FAIL count, so it can gate.
add_executable(bench_tail_validate bench_tail_validate.cpp)
target_compile_options(bench_tail_validate PRIVATE -Wall -Wextra)

# The mount descent's k=W..1 width loop vs one full-table scan — the headroom a single-pass
# descent could win, measured at W>3 where the shipped cap makes it invisible.
# The A/B the width lift is gated on (#523). Deliberately written to compile UNCHANGED
# against the pre-lift tree, so the two binaries can be interleaved on one machine.
# The PRODUCER fan-out's reply leg (#504): a write to a vertex with N remote subscribers runs
# deliver_remote N times, and each opens with a by_name registry scan. Prices that scan per
# delivery — swept over fan-out, registry width and scan position — with an exact resolution
# count taken through the bus-peer tier.
add_executable(bench_reply_leg bench_reply_leg.cpp)
target_link_libraries(bench_reply_leg PRIVATE libtracer)
target_compile_options(bench_reply_leg PRIVATE -Wall -Wextra)
# EXPERIMENT (#504 — DELETE with the verdict): arms the prototype reply-leg memo so the bench
# can interleave it against the shipped path inside one binary.
target_compile_definitions(bench_reply_leg PRIVATE LIBTRACER_BENCH_REPLY_LEG_MEMO=1)

add_executable(bench_mount_resolve bench_mount_resolve.cpp)
target_link_libraries(bench_mount_resolve PRIVATE libtracer)
target_compile_options(bench_mount_resolve PRIVATE -Wall -Wextra)

# The TCP server's END-TO-END baseline (throughput AND the p50/p99/p999/max latency
# distribution) swept over concurrent peers — the join of bench_tcp_fanin's fan-in topology
# with bench_transports' timestamped payload. Diagnostic, not a perf gate.
add_executable(bench_tcp_baseline bench_tcp_baseline.cpp)
target_link_libraries(bench_tcp_baseline PRIVATE libtracer)
target_compile_options(bench_tcp_baseline PRIVATE -Wall -Wextra)

# Per-connection RAM census (the transport plane's half of the ADR-0038 memory gate):
# a counting operator-new measures the LIVE heap a real established connection holds,
# across accept -> established -> big frame -> teardown -> reconnect, for tcp / ws /
# udp / can. Diagnostic, not a perf gate — it stands up real loopback sockets.
add_executable(bench_conn_ram bench_conn_ram.cpp)
target_link_libraries(bench_conn_ram PRIVATE libtracer)
target_compile_options(bench_conn_ram PRIVATE -Wall -Wextra)

# What the transport iovec spill COSTS and how often it is reached. bench_transport_iov found
# WHERE the boundary sits (17 caller spans); this one measures the egress span count a real
# rope forward hop produces, the latency of one spill (isolated and behind a real sendmsg),
# and prices it against transport_t::send(iov)'s always-allocating default. Diagnostic.
add_executable(bench_iov_spill_cost bench_iov_spill_cost.cpp)
target_link_libraries(bench_iov_spill_cost PRIVATE libtracer)
target_compile_options(bench_iov_spill_cost PRIVATE -Wall -Wextra)

# Costs the ADR-0065 `block_source_t` migration (#603 defect 1): a deterministic BLOCK census
# of the peer-driven route-handle control path (which blocks go through the injected seam and
# which escape to the global heap), plus an interleaved A/B of the two guard shapes — the
# `std::vector` + `detail::try_push_back` probe pattern the 29 call sites use today against
# `block_array_t` over an injected `block_source_t`, with an unguarded `push_back` control arm.
# Diagnostic, not a perf gate.
add_executable(bench_failable_census bench_failable_census.cpp)
target_link_libraries(bench_failable_census PRIVATE libtracer)
target_compile_options(bench_failable_census PRIVATE -Wall -Wextra)

# RFC-0022 §6's implementation gate: does turning §3.D's pin predicate on by default buy
# latency, and at what K? Two halves, because §6 demands both halves of the dual target.
#
#   bench_pin_ratio — the WRITE store leg over a (payload x segment) grid, every arm rotating
#     inside ONE process (the recorded 2.8x sequential swing is what that defends against).
#   bench_pin_net   — two PROCESSES over real UDP with a delivery-COUNTING receiver and a
#     bounded RX pool, so the throughput claim is a receiver's count and the RAM claim is a
#     measured free-slot floor rather than arithmetic.
#
# LIBTRACER_PIN_INSTRUMENT arms the decision site's pin/copy counters. It is defined on the
# libtracer target too (PUBLIC would leak it into every consumer), because the counter is
# incremented inside the library's TU and read from the bench's. Both benches ALSO carry an
# instrument that needs no such define — segment-pointer identity between the stored value and
# the frame — which is what lets this exact source build as the untouched-main control arm.
option(LIBTRACER_PIN_INSTRUMENT "Arm the RFC-0022 §6 pin/copy branch counters" ON)
if(LIBTRACER_PIN_INSTRUMENT)
    target_compile_definitions(libtracer PUBLIC LIBTRACER_PIN_INSTRUMENT)
endif()

add_executable(bench_pin_ratio bench_pin_ratio.cpp)
target_link_libraries(bench_pin_ratio PRIVATE libtracer)
target_compile_options(bench_pin_ratio PRIVATE -Wall -Wextra)

add_executable(bench_pin_net bench_pin_net.cpp)
target_link_libraries(bench_pin_net PRIVATE libtracer)
target_compile_options(bench_pin_net PRIVATE -Wall -Wextra)
