# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

add_executable(in_process_pubsub in_process_pubsub.cpp)
target_link_libraries(in_process_pubsub PRIVATE libtracer)
find_package(Threads REQUIRED)
target_link_libraries(in_process_pubsub PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(in_process_pubsub PRIVATE /W4)
else()
    target_compile_options(in_process_pubsub PRIVATE -Wall -Wextra)
endif()

# L2/L3 wire codec round-trip (no threads) — the frame-codec worked example.
add_executable(wire_roundtrip wire_roundtrip.cpp)
target_link_libraries(wire_roundtrip PRIVATE libtracer)
if(MSVC)
    target_compile_options(wire_roundtrip PRIVATE /W4)
else()
    target_compile_options(wire_roundtrip PRIVATE -Wall -Wextra)
endif()

# L1 scatter-gather worked example (no threads) — the rope_t / to_iovec / flatten story.
add_executable(rope_scatter rope_scatter.cpp)
target_link_libraries(rope_scatter PRIVATE libtracer)
if(MSVC)
    target_compile_options(rope_scatter PRIVATE /W4)
else()
    target_compile_options(rope_scatter PRIVATE -Wall -Wextra)
endif()

# L4 pub/sub fan-out worked example — per-delivery dispatch cost as fan-out scales.
add_executable(pubsub_fanout pubsub_fanout.cpp)
target_link_libraries(pubsub_fanout PRIVATE libtracer)
find_package(Threads REQUIRED)
target_link_libraries(pubsub_fanout PRIVATE Threads::Threads)
if(MSVC)
    target_compile_options(pubsub_fanout PRIVATE /W4)
else()
    target_compile_options(pubsub_fanout PRIVATE -Wall -Wextra)
endif()

# L2/L3 wire codec deep-dive — encode/decode anatomy + throughput.
add_executable(wire_codec wire_codec.cpp)
target_link_libraries(wire_codec PRIVATE libtracer)
if(MSVC)
    target_compile_options(wire_codec PRIVATE /W4)
else()
    target_compile_options(wire_codec PRIVATE -Wall -Wextra)
endif()

# Two nodes over the in-process loopback "wire" — FWD delivery through fwd_router_t
# (ADR-0040), with cross-wire latency. A one-line swap to udp_transport_t makes it a
# real socket (core/tests/udp_test.cpp). The retired two_node_loopback/udp_two_node
# examples predated the explicit-source-routed FWD plane; this replaces them.
# Needs the FWD net plane (fwd_router_t), so it is built only when LIBTRACER_NET_PLANE
# is ON — a pure in-process node (net plane off) has no forwarding to demonstrate.
if(LIBTRACER_NET_PLANE)
    add_executable(two_node_fwd two_node_fwd.cpp)
    target_link_libraries(two_node_fwd PRIVATE libtracer)
    find_package(Threads REQUIRED)
    target_link_libraries(two_node_fwd PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(two_node_fwd PRIVATE /W4)
    else()
        target_compile_options(two_node_fwd PRIVATE -Wall -Wextra)
    endif()
endif()

# The three composition axes (L1 rope / L4 vertex tree / transport-as-identity) —
# why a node is a *tree of ropes*, not a *rope of ropes*. The transport-mount axis
# uses the in-process loopback link, so it needs the FWD net plane (fwd_router_t).
if(LIBTRACER_NET_PLANE)
    add_executable(tree_of_ropes tree_of_ropes.cpp)
    target_link_libraries(tree_of_ropes PRIVATE libtracer)
    find_package(Threads REQUIRED)
    target_link_libraries(tree_of_ropes PRIVATE Threads::Threads)
    if(MSVC)
        target_compile_options(tree_of_ropes PRIVATE /W4)
    else()
        target_compile_options(tree_of_ropes PRIVATE -Wall -Wextra)
    endif()
endif()

# Run the examples as smoke tests when testing is enabled (they self-check and
# return non-zero on any mismatch, so the smoke tests guard behavior).
if(BUILD_TESTING)
    add_test(NAME example_in_process_pubsub COMMAND in_process_pubsub)
    add_test(NAME example_wire_roundtrip COMMAND wire_roundtrip)
    add_test(NAME example_rope_scatter COMMAND rope_scatter)
    add_test(NAME example_pubsub_fanout COMMAND pubsub_fanout)
    add_test(NAME example_wire_codec COMMAND wire_codec)
    if(LIBTRACER_NET_PLANE)
        add_test(NAME example_two_node_fwd COMMAND two_node_fwd)
        add_test(NAME example_tree_of_ropes COMMAND tree_of_ropes)
    endif()
endif()
