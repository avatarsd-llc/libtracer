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

# --- The L4 graph domain: one concept per example (#1383) --------------------
#
# Deliberately APPENDED below the pre-existing block rather than merged into it: the
# doc-citation gate pins `core/examples/CMakeLists.txt:59,74,87,92`, so anything inserted
# above line 97 moves an anchored citation on `docs/examples/index.md`.
#
# None of these needs the FWD net plane (they are pure in-process graph), so all eight are
# unconditional; none needs Threads except `graph_await`, which parks a waiter thread.
set(LIBTRACER_GRAPH_EXAMPLES
    graph_register
    graph_read_write
    graph_await
    graph_write_creates
    graph_children
    graph_retire
    graph_handler_vertex
    graph_stream)

foreach(example IN LISTS LIBTRACER_GRAPH_EXAMPLES)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE libtracer)
    if(MSVC)
        target_compile_options(${example} PRIVATE /W4)
    else()
        target_compile_options(${example} PRIVATE -Wall -Wextra)
    endif()
endforeach()

find_package(Threads REQUIRED)
target_link_libraries(graph_await PRIVATE Threads::Threads)

# Spelled through a local alias rather than a second bare test-guard condition: the
# doc-citation gate pins the guard at line 87 by its TEXT, and a second occurrence of that
# text anywhere in this file makes the anchor ambiguous. Same condition, one anchor.
set(LIBTRACER_REGISTER_GRAPH_EXAMPLE_TESTS ${BUILD_TESTING})
if(LIBTRACER_REGISTER_GRAPH_EXAMPLE_TESTS)
    foreach(example IN LISTS LIBTRACER_GRAPH_EXAMPLES)
        add_test(NAME example_${example} COMMAND ${example})
    endforeach()
endif()

# The subscriptions domain (#1383) — one concept per target, no threads, no net plane.
# Declared as a list rather than seven near-identical blocks: they differ only in name,
# and a new one is then a single line rather than a copied block that can drift.
#
# Every one of them builds under EVERY binding, including `sub_unsubscribe_from_dispatch`,
# whose subject `reclaim_strict_t` forbids. There is deliberately no exclusion for it here:
# the reclamation policy is a C++ type bound in `config_override.hpp`, not a CMake variable
# (ADR-0068), so CMake cannot see which one is in force — the example follows the binding
# with `if constexpr` and skips at run time, exactly as `reclaim_test` does.
set(LIBTRACER_SUBSCRIPTION_EXAMPLES
    sub_callback                    # subscribe to one vertex; the delivery-callback contract
    sub_subtree                     # one edge covers a whole subtree (RFC-0005)
    sub_terminal_delivery           # delivery terminates at the target (RFC-0007)
    sub_durability_latch            # the per-subscription delivery policy (RFC-0022 §3.A)
    sub_unsubscribe                 # unsubscribe + the release hook (ADR-0080)
    sub_unsubscribe_from_dispatch   # the re-entrant case and its deferred grace point
    sub_retire                      # retiring a producer drops its subscriptions (RFC-0009 §B)
)
foreach(example IN LISTS LIBTRACER_SUBSCRIPTION_EXAMPLES)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE libtracer)
    if(MSVC)
        target_compile_options(${example} PRIVATE /W4)
    else()
        target_compile_options(${example} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# Same alias as the graph block above, and for the same reason: a second bare spelling of
# the test guard anywhere in this file would make the line-87 citation anchor ambiguous.
if(LIBTRACER_REGISTER_GRAPH_EXAMPLE_TESTS)
    foreach(example IN LISTS LIBTRACER_SUBSCRIPTION_EXAMPLES)
        add_test(NAME example_${example} COMMAND ${example})
    endforeach()
endif()

# The wire domain (#1383) — the L2/L3 codec, one concept per target. Appended at EOF for the
# same reason the graph block was: `docs/examples/index.md` pins line-numbered citations into
# the block above, and anything inserted there moves them.
#
# All eight are pure L2/L3 over L0/L1: no net plane, no transports, no threads. They build and
# run identically under `-DLIBTRACER_NET_PLANE=OFF`.
set(LIBTRACER_WIRE_EXAMPLES
    wire_tlv_header            # the 4-byte header and the opt byte; the LL widen rule
    wire_structured_vs_opaque  # opt.PL — children vs payload over the SAME body bytes
    wire_packed_path           # RFC-0018 packed segment records; the body IS the vertex key
    wire_path_escape           # RFC-0018 §5.4: skippable in a frame, rejected as a key
    wire_trailer               # the opt-in CRC and timestamp trailer, and its loud refusal
    wire_decode_refusals       # the four decode verdicts; the injected walk-stack bound
    wire_arena_decode          # decode_into over a stack bump source — a flat, heap-free arena
    wire_lazy_view             # ADR-0053 tlv_view_t: a link boundary that falls mid-header
)

# The view / ownership domain (#1383) — the L0/L1 substrate every other example stands on.
# `view_sync_pool` is the ONE target here a build configuration can forbid: a target that sets
# `tr::mem::kSpinWaitSafe = false` may not instantiate `sync_pool_t` at all. That is a C++ type
# knob in `config_override.hpp` (ADR-0068), invisible to CMake, so there is deliberately no
# exclusion for it here — the example puts its body in a template, follows the binding with
# `if constexpr`, and SKIPS at run time. An example a binding does not apply to must never
# break the build.
set(LIBTRACER_VIEW_EXAMPLES
    view_segment_refcount  # the refcounted segment; copy == clone; reclaim at the last drop
    view_subview           # the {owner, offset, length} window, narrowed without a copy
    view_borrow            # route the app's OWN bytes (ADR-0012) vs over_bytes, which copies
    view_rope_compose      # the rope: assembly is chaining, never memcpy
    view_rope_subrope      # a sub-range starting mid-link; walk() and the iovec egress
    view_pool_backend      # a bounded caller-owned slab; exhaustion by value, not by OOM
    view_device_rope       # a DEVICE link: NOT_HOST is permanent, NO_MEMORY is not
    view_sync_pool         # ADR-0060 §2 — a shared seam needs a thread-safe backend
)

foreach(example IN LISTS LIBTRACER_WIRE_EXAMPLES LIBTRACER_VIEW_EXAMPLES)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE libtracer)
    if(MSVC)
        target_compile_options(${example} PRIVATE /W4)
    else()
        target_compile_options(${example} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# `Threads::Threads` was already found in this directory scope (the graph block above).
target_link_libraries(view_sync_pool PRIVATE Threads::Threads)

# The same alias as the two blocks above, and for the same reason: a second bare spelling of
# the test guard anywhere in this file would make the line-87 citation anchor ambiguous.
if(LIBTRACER_REGISTER_GRAPH_EXAMPLE_TESTS)
    foreach(example IN LISTS LIBTRACER_WIRE_EXAMPLES LIBTRACER_VIEW_EXAMPLES)
        add_test(NAME example_${example} COMMAND ${example})
    endforeach()
endif()

# The mem / substrate domain (#1383) — L0 itself: the two seams a node injects, and the three
# sources plus one container built on the failable one. Appended at EOF for the same reason
# every block above it was: `docs/examples/index.md` pins line-numbered citations into the
# first block, and anything inserted there moves them.
#
# All eight are pure L0: no net plane, no transports, no threads, no build-configuration knob
# that could forbid one of them. Nothing here skips at run time, so a `ctest -R example_mem_`
# pass is a pass for eight examples that all demonstrated something.
set(LIBTRACER_MEM_EXAMPLES
    mem_block_source      # the seam: nothrow try_alloc, sized release, nullptr on exhaustion
    mem_source_vs_backend # refcounted segment vs single-owner block — the owner count picks
    mem_bump_source       # a cursor over a caller buffer; release is a no-op, reset() is not
    mem_bump_upstream     # the upstream decides: a fast path, or the hard bound
    mem_pool_source       # ADR-0067 — a long-lived bounded seam has to RECYCLE
    mem_size_classes      # classes do not share; classes_used()/overflowed() size the span
    mem_block_array       # a growable array whose growth returns false instead of throwing
    mem_source_resource   # ADR-0079 — the std::pmr adapter: placement and bounding, not failability
)

foreach(example IN LISTS LIBTRACER_MEM_EXAMPLES)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE libtracer)
    if(MSVC)
        target_compile_options(${example} PRIVATE /W4)
    else()
        target_compile_options(${example} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# The same alias again, for the same anchor reason.
if(LIBTRACER_REGISTER_GRAPH_EXAMPLE_TESTS)
    foreach(example IN LISTS LIBTRACER_MEM_EXAMPLES)
        add_test(NAME example_${example} COMMAND ${example})
    endforeach()
endif()
