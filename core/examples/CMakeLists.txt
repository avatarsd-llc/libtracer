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

# The two-node cross-transport examples (two_node_loopback / udp_two_node) were
# retired with bridge_t (ADR-0040 — the net plane is explicit-source-routed FWD only).
# Two-node FWD delivery over a real transport is covered by core/tests/udp_test.

# Run the examples as smoke tests when testing is enabled (they self-check and
# return non-zero on any mismatch, so the smoke tests guard behavior).
if(BUILD_TESTING)
    add_test(NAME example_in_process_pubsub COMMAND in_process_pubsub)
    add_test(NAME example_wire_roundtrip COMMAND wire_roundtrip)
endif()
