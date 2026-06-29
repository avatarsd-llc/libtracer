# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

set(LIBTRACER_VECTORS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/conformance/vectors/v1")

function(libtracer_test_warnings tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Low-level primitive — little-endian (de)serialization (byteorder.hpp).
add_executable(byteorder_test byteorder_test.cpp)
target_link_libraries(byteorder_test PRIVATE libtracer)
libtracer_test_warnings(byteorder_test)
add_test(NAME byteorder COMMAND byteorder_test)

# Path parsing / validation — reserved chars, limits, field tail (reference/03).
add_executable(path_test path_test.cpp)
target_link_libraries(path_test PRIVATE libtracer)
libtracer_test_warnings(path_test)
add_test(NAME path COMMAND path_test)

# Frame codec — nesting depth cap / iterative-parse contract (reference/01).
add_executable(frame_test frame_test.cpp)
target_link_libraries(frame_test PRIVATE libtracer)
libtracer_test_warnings(frame_test)
add_test(NAME frame COMMAND frame_test)

# #54 — transport_ws RFC 6455 protocol layer (handshake key + frame codec; header-only).
add_executable(ws_test ws_test.cpp)
target_link_libraries(ws_test PRIVATE libtracer)
libtracer_test_warnings(ws_test)
add_test(NAME ws COMMAND ws_test)

# #55 — transport_can pure framing layer (29-bit ID codec, view_can_frames,
# mem_can_reassembly, advertise codec; no SocketCAN). Links libtracer for heap_alloc.
add_executable(can_frames_test can_frames_test.cpp)
target_link_libraries(can_frames_test PRIVATE libtracer)
libtracer_test_warnings(can_frames_test)
add_test(NAME can_frames COMMAND can_frames_test)

# #55 (increment 2) — transport_can SocketCAN binding, exercised over an in-memory
# fake link (no kernel CAN needed; the real vcan path runs in the can-vcan-e2e CI job).
add_executable(transport_can_test transport_can_test.cpp)
target_link_libraries(transport_can_test PRIVATE libtracer)
libtracer_test_warnings(transport_can_test)
add_test(NAME transport_can COMMAND transport_can_test)

# #55 (increment 2) — real-bus smoke test over a kernel vcan0 device. Self-skips
# (clean exit) when vcan0 cannot bind, so it is harmless in the normal suite; the
# dedicated can-vcan-e2e CI job sets vcan0 up so the SocketCAN path runs for real.
add_executable(transport_can_vcan_test transport_can_vcan_test.cpp)
target_link_libraries(transport_can_vcan_test PRIVATE libtracer)
libtracer_test_warnings(transport_can_vcan_test)
add_test(NAME transport_can_vcan COMMAND transport_can_vcan_test)

# #81-A — per-endpoint :acl structural storage (store/serve raw ACL TLV; no enforcement).
add_executable(acl_test acl_test.cpp)
target_link_libraries(acl_test PRIVATE libtracer)
libtracer_test_warnings(acl_test)
add_test(NAME acl COMMAND acl_test)

# M1 — conformance harness (links the built library).
add_executable(conformance_runner conformance_runner.cpp)
target_link_libraries(conformance_runner PRIVATE libtracer)
libtracer_test_warnings(conformance_runner)
target_compile_definitions(conformance_runner PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME conformance COMMAND conformance_runner)

# M2 — substrate harness (links the built library; atomic refcounts).
add_executable(substrate_test substrate_test.cpp)
target_link_libraries(substrate_test PRIVATE libtracer)
libtracer_test_warnings(substrate_test)
target_compile_definitions(substrate_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME substrate COMMAND substrate_test)

# M3 — L4 graph-runtime harness (single-threaded checks + multithreaded stress).
add_executable(graph_test graph_test.cpp)
target_link_libraries(graph_test PRIVATE libtracer)
libtracer_test_warnings(graph_test)
target_compile_definitions(graph_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME graph COMMAND graph_test)

# RFC-0004 / ADR-0035 slice 2 — local FWD operation resolution + zero-copy FWD{REPLY}.
add_executable(op_resolve_test op_resolve_test.cpp)
target_link_libraries(op_resolve_test PRIVATE libtracer)
libtracer_test_warnings(op_resolve_test)
target_compile_definitions(op_resolve_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME op_resolve COMMAND op_resolve_test)

# RFC-0004 / ADR-0035 slice 3 — multi-hop FWD forwarding + zero-copy src
# accumulation over LIVE transport_ws (client -> A forwarder -> B terminus).
add_executable(fwd_multihop_test fwd_multihop_test.cpp)
target_link_libraries(fwd_multihop_test PRIVATE libtracer)
libtracer_test_warnings(fwd_multihop_test)
add_test(NAME fwd_multihop COMMAND fwd_multihop_test)

# RFC-0004 / ADR-0035 slice 4 — the route-handle: ws delivery-compaction over LIVE
# transport_ws (producer B -> A forwarder -> consumer C). Proves the byte-delta of a
# label-compacted COMPACT vs a full-route FWD, byte-exact ordered delivery, the
# stale-label drop + re-advertise self-heal, and that a non-compact flow stays stateless.
add_executable(fwd_compact_test fwd_compact_test.cpp)
target_link_libraries(fwd_compact_test PRIVATE libtracer)
libtracer_test_warnings(fwd_compact_test)
add_test(NAME fwd_compact COMMAND fwd_compact_test)

# RFC-0004 / ADR-0035 slice 4 (#136) — the PRODUCER auto fan-out: a plain graph.write to a
# vertex with a remote subscriber delivers a full-route FWD{WRITE} (or auto-promoted COMPACT)
# with no explicit advertise/send. Covers the transient-local latch, the compact byte-delta,
# reconnect self-heal, and a concurrent writer x clear_link race (the TSan gate).
add_executable(fwd_fanout_test fwd_fanout_test.cpp)
target_link_libraries(fwd_fanout_test PRIVATE libtracer)
libtracer_test_warnings(fwd_fanout_test)
add_test(NAME fwd_fanout COMMAND fwd_fanout_test)

# M4 — transport + bridge harness (loopback delivery, ROUTER, dedup, hop_count).
add_executable(bridge_test bridge_test.cpp)
target_link_libraries(bridge_test PRIVATE libtracer)
libtracer_test_warnings(bridge_test)
target_compile_definitions(bridge_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME bridge COMMAND bridge_test)

# M5 — UDP transport harness (raw frame + two-node end-to-end over localhost UDP).
add_executable(udp_test udp_test.cpp)
target_link_libraries(udp_test PRIVATE libtracer)
libtracer_test_warnings(udp_test)
target_compile_definitions(udp_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME udp COMMAND udp_test)

# #54 — transport_ws server socket layer (TCP accept + RFC6455 handshake + transport_t).
add_executable(ws_transport_test ws_transport_test.cpp)
target_link_libraries(ws_transport_test PRIVATE libtracer)
libtracer_test_warnings(ws_transport_test)
add_test(NAME ws_transport COMMAND ws_transport_test)

# #54 (interop) — standalone transport_ws_server harness driven by the TypeScript
# transport-ws interop test (a real TS TransportWs client over a live socket).
# Deliberately NOT an add_test(): it is a helper binary the TS test spawns (see
# bindings/typescript/packages/transport-ws/test/interop.test.mjs and the
# ws-interop CI job), so it must not run as part of the C++ ctest suite.
add_executable(ws_interop_server ws_interop_server.cpp)
target_link_libraries(ws_interop_server PRIVATE libtracer)
libtracer_test_warnings(ws_interop_server)

# #56 / RFC-0004 (interop) — standalone FWD-serving node harness driven by the
# TypeScript client SDK interop test (the web-UI<->device consumer proof). A
# graph_t seeded with /sensor/temp behind a fwd_router_t + op_resolver_t, served
# over a live transport_ws_server. Like ws_interop_server it is a helper binary
# the TS test spawns, NOT an add_test() (it would otherwise block on its deadline).
add_executable(fwd_node_server fwd_node_server.cpp)
target_link_libraries(fwd_node_server PRIVATE libtracer)
libtracer_test_warnings(fwd_node_server)

# #60 (hardening) — ws frame-decoder differential-fuzz harness. Reads one hex
# frame per stdin line and prints a canonical decode result (OK/NEED_MORE/ERR),
# byte-identical to the TS twin (transport-ws/fuzz/decode_harness.mjs), so
# tests/conformance/ws_diff_fuzz.py can diff the two decoders. Like
# ws_interop_server it is a helper binary the driver spawns, NOT an add_test().
add_executable(ws_fuzz_harness ws_fuzz_harness.cpp)
target_link_libraries(ws_fuzz_harness PRIVATE libtracer)
libtracer_test_warnings(ws_fuzz_harness)

# M2 — substrate harness in single-threaded mode. LIBTRACER_NO_ATOMIC changes the
# Segment refcount layout, so this variant compiles the library sources directly
# (rather than linking the atomic-built libtracer.a) to keep one consistent ABI.
add_executable(substrate_test_no_atomic
    substrate_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/frame.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mem_heap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mem_pool.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rope.cpp
)
target_include_directories(substrate_test_no_atomic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(substrate_test_no_atomic PRIVATE cxx_std_23)
libtracer_test_warnings(substrate_test_no_atomic)
target_compile_definitions(substrate_test_no_atomic PRIVATE
    LIBTRACER_NO_ATOMIC
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME substrate_no_atomic COMMAND substrate_test_no_atomic)

# mem_cuda GPU test — only with LIBTRACER_WITH_CUDA; run on a real GPU locally
# (scripts/test-cuda.sh), never in CI.
if(LIBTRACER_WITH_CUDA)
    add_executable(cuda_test cuda_test.cpp)
    target_link_libraries(cuda_test PRIVATE libtracer)
    libtracer_test_warnings(cuda_test)
    add_test(NAME cuda COMMAND cuda_test)
endif()
