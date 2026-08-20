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

# The auth / ACL domain (#1383) — who may do what, and the two halves of that question: the
# graph's gates (a resolved subject against a vertex's effective ACL) and the pure ADR-0050
# policy seam beneath them. Appended at EOF for the same reason every block above it was:
# `docs/examples/index.md` pins line-numbered citations into the first block.
#
# All eight are pure L4 over the always-compiled ACL headers: no net plane, no transports, no
# sockets, no threads. The one build knob in reach is `acl_policy_t` (ADR-0068 — plain C++ in
# config.hpp, which CMake cannot see), and `acl_policy_profiles` handles it by naming BOTH
# policies explicitly as template arguments instead of skipping an arm: it exercises the
# ALLOW-only and the full policy in every build and only PRINTS which one this target binds.
# So, as with the mem_* group, nothing here is conditional at build OR at run time.
set(LIBTRACER_ACL_EXAMPLES
    acl_open_by_default   # enforcement is opt-in twice; the first ACE is what closes a vertex
    acl_subject_resolver  # ADR-0018 caller context -> subject token; the error arm DENIES
    acl_right_bits        # access_mask is a bitfield; every gate tests exactly one bit
    acl_everyone_reserved # the one wildcard subject, reserved against the resolver's output
    acl_inherit           # effective ACL = own ACEs + INHERIT-flagged ancestor ACEs
    acl_expiry            # expires_ns is evaluated against the caller's `now`, nothing cached
    acl_policy_profiles   # ADR-0050 — DENY exists only where the evaluator can see it
    acl_parse_strict      # parse_acl refuses shapes encode_acl never emits; leniency widens
)

foreach(example IN LISTS LIBTRACER_ACL_EXAMPLES)
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
    foreach(example IN LISTS LIBTRACER_ACL_EXAMPLES)
        add_test(NAME example_${example} COMMAND ${example})
    endforeach()
endif()

# The routing domain (#1383) — how a frame gets from one node to another: the source route in
# the `dst`, the NAME→link table each hop resolves it against, and the label plane a repeating
# flow uses to stop re-carrying it. Appended at EOF for the same reason every block above it
# was: `docs/examples/index.md` pins line-numbered citations into the first block.
#
# These eight are the FIRST domain group that cannot be unconditional. `fwd_router_t`,
# `route_handle_t` and `op_resolve` are the net plane, compiled only under
# `if(LIBTRACER_NET_PLANE)` (`core/CMakeLists.txt:92`), so at `-DLIBTRACER_NET_PLANE=OFF` these
# targets do not merely skip — the types they name do not exist. That is a BUILD-time absence,
# and it is the honest one: the alternative, an unconditional target that returns early, would
# make ctest record eight passes for eight examples that ran nothing (the hazard
# `docs/examples/index.md` documents at length). The option defaults to ON, so every CI leg but
# the deliberate minimal-set one builds and runs all eight; the index says what the minimal-set
# leg is missing and how to see it.
#
# Within the group nothing is conditional at RUN time: no target has a `config_override.hpp`
# knob in reach, no sockets are opened, no ports are bound and no threads are started — every
# link is a recording stub driven synchronously, so a green `ctest -R example_route_` in a
# net-plane build is a pass for eight examples that all demonstrated something.
set(LIBTRACER_ROUTING_EXAMPLES
    route_dst_is_source_route # the route rides in the frame: dst shrinks, src grows, per hop
    route_terminus_or_forward # one test decides a hop: does the leading dst segment name a child?
    route_child_table         # one NAME→link table, one slot per name; a refusal registers nothing
    route_qualified_mount     # RFC-0014 strip-K: a mount name is any width, consumed in one step
    route_multi_hop           # a chain is the one-hop rule again; the middle hop stores nothing
    route_reply_home          # the accumulated src IS the return route — no correlation table
    route_label_compact       # ADR-0035 — advertise binds label↔route per link; COMPACT elides it
    route_label_stale         # an unknown label is dropped and NACK'd; the NACK re-advertises
)

if(LIBTRACER_NET_PLANE)
    foreach(example IN LISTS LIBTRACER_ROUTING_EXAMPLES)
        add_executable(${example} ${example}.cpp)
        target_link_libraries(${example} PRIVATE libtracer)
        if(MSVC)
            target_compile_options(${example} PRIVATE /W4)
        else()
            target_compile_options(${example} PRIVATE -Wall -Wextra)
        endif()
    endforeach()

    # The same alias as every block above, for the same anchor reason — and nested inside the
    # net-plane guard so the registrations follow the targets exactly.
    if(LIBTRACER_REGISTER_GRAPH_EXAMPLE_TESTS)
        foreach(example IN LISTS LIBTRACER_ROUTING_EXAMPLES)
            add_test(NAME example_${example} COMMAND ${example})
        endforeach()
    endif()
endif()

# The net-per-kind domain (#1383) — one page per wire technology, plus the seam they all
# implement and the registry that selects between them by name. Appended at EOF for the same
# reason every block above it was: `docs/examples/index.md` pins line-numbered citations into
# the first block, and anything inserted there moves them.
#
# This is the first group whose targets are guarded INDIVIDUALLY rather than as a whole. The
# routing block above is one `if(LIBTRACER_NET_PLANE)` because its subject is that one option;
# here each example's subject is a different option, and folding them into one guard would
# make a UDP-only build lose the UDP example. So the absence is recorded per kind:
#
#   net_transport_seam        (none)                      — transport_t + loopback are core
#   net_kind_catalog          LIBTRACER_NET_PLANE         — transport_vertex_t is the plane
#   net_udp_datagram          LIBTRACER_TRANSPORT_UDP
#   net_dial_and_listen       LIBTRACER_TRANSPORT_TCP
#   net_tcp_stream_framing    LIBTRACER_TRANSPORT_TCP
#   net_multi_peer_listener   LIBTRACER_TRANSPORT_TCP
#   net_ws_upgrade            LIBTRACER_TRANSPORT_WS
#   net_can_bus_peers         LIBTRACER_TRANSPORT_CAN
#
# `net_multi_peer_listener` is additionally the ONE target in the whole examples tree whose
# subject a build can compile out WITHOUT CMake being able to see it: the ADR-0044 bus module
# is bound by `kBusLinks` in config_override.hpp (ADR-0068), which is plain C++. It follows the
# binding with `if constexpr`, states the skip, and exits 77 — and the SKIP_RETURN_CODE
# property below is what makes ctest read that as `Skipped` rather than as a pass. That is
# strictly better than the two older run-time skips in this file, which exit 0 and are
# therefore indistinguishable from a real pass; prefer this shape for any new one.
set(LIBTRACER_NET_KIND_EXAMPLES_CORE
    net_transport_seam        # the three-call seam every kind implements, and nothing else
)
set(LIBTRACER_NET_KIND_EXAMPLES_PLANE
    net_kind_catalog          # a kind is a NAME: two registries, both open, both strict
)
set(LIBTRACER_NET_KIND_EXAMPLES_UDP
    net_udp_datagram          # a datagram already has boundaries, so udp adds no framing
)
set(LIBTRACER_NET_KIND_EXAMPLES_TCP
    net_dial_and_listen       # DIAL and LISTEN are two constructors of one type
    net_tcp_stream_framing    # a stream has no boundaries, so the kind supplies a prefix
    net_multi_peer_listener   # one listener, many slots; p<slot> is a POSITION, not an identity
)
set(LIBTRACER_NET_KIND_EXAMPLES_WS
    net_ws_upgrade            # no frame crosses a ws link until the HTTP Upgrade completes
)
set(LIBTRACER_NET_KIND_EXAMPLES_CAN
    net_can_bus_peers         # the one BUS kind: peers synthesized from traffic, ADR-0044
)

set(LIBTRACER_NET_KIND_EXAMPLES ${LIBTRACER_NET_KIND_EXAMPLES_CORE})
if(LIBTRACER_NET_PLANE)
    list(APPEND LIBTRACER_NET_KIND_EXAMPLES ${LIBTRACER_NET_KIND_EXAMPLES_PLANE})
endif()
if(LIBTRACER_TRANSPORT_UDP)
    list(APPEND LIBTRACER_NET_KIND_EXAMPLES ${LIBTRACER_NET_KIND_EXAMPLES_UDP})
endif()
if(LIBTRACER_TRANSPORT_TCP)
    list(APPEND LIBTRACER_NET_KIND_EXAMPLES ${LIBTRACER_NET_KIND_EXAMPLES_TCP})
endif()
if(LIBTRACER_TRANSPORT_WS)
    list(APPEND LIBTRACER_NET_KIND_EXAMPLES ${LIBTRACER_NET_KIND_EXAMPLES_WS})
endif()
if(LIBTRACER_TRANSPORT_CAN)
    list(APPEND LIBTRACER_NET_KIND_EXAMPLES ${LIBTRACER_NET_KIND_EXAMPLES_CAN})
endif()

foreach(example IN LISTS LIBTRACER_NET_KIND_EXAMPLES)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE libtracer)
    if(MSVC)
        target_compile_options(${example} PRIVATE /W4)
    else()
        target_compile_options(${example} PRIVATE -Wall -Wextra)
    endif()
endforeach()

# `Threads::Threads` was already found in this directory scope; every kind here spawns a
# receive thread (or, for the loopback and the in-memory CAN bus, drives one of its own).
foreach(example IN LISTS LIBTRACER_NET_KIND_EXAMPLES)
    target_link_libraries(${example} PRIVATE Threads::Threads)
endforeach()

# The same alias as every block above, for the same anchor reason.
if(LIBTRACER_REGISTER_GRAPH_EXAMPLE_TESTS)
    foreach(example IN LISTS LIBTRACER_NET_KIND_EXAMPLES)
        add_test(NAME example_${example} COMMAND ${example})
    endforeach()
    # 77 is the autotools convention ctest documents, spelled `kSkipExitCode` on the C++ side
    # of `net_multi_peer_listener.cpp` — change both together. Without this property that exit
    # is a FAILURE; with it, a bus-closed build reports the example as Skipped, which is the
    # honest answer and the one no `return 0` can give.
    if(TEST example_net_multi_peer_listener)
        set_property(TEST example_net_multi_peer_listener PROPERTY SKIP_RETURN_CODE 77)
    endif()
endif()
