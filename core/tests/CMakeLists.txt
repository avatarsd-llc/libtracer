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

# u32-length-prefix stream reassembler (length_prefix_framer.hpp) — the extracted
# transport_quic/webtransport RX state machine, tested without a live connection.
add_executable(length_prefix_framer_test length_prefix_framer_test.cpp)
target_link_libraries(length_prefix_framer_test PRIVATE libtracer)
libtracer_test_warnings(length_prefix_framer_test)
add_test(NAME length_prefix_framer COMMAND length_prefix_framer_test)

# FWD offset-dispatch frame view (fwd_frame_view.hpp) — the extracted fwd_router
# forward-plane cluster, tested directly over hand-built frames (span + rope
# cursors): dst-seg / op / control peeks, the shrunk-dst/grown-src head rebuild
# proved byte-exact vs a reference re-encode, and malformed rejects.
add_executable(fwd_frame_view_test fwd_frame_view_test.cpp)
target_link_libraries(fwd_frame_view_test PRIVATE libtracer)
libtracer_test_warnings(fwd_frame_view_test)
add_test(NAME fwd_frame_view COMMAND fwd_frame_view_test)

# Canonical-key NAME navigation (key_view.hpp) — the locus the graph walks funnel through.
add_executable(key_view_test key_view_test.cpp)
target_link_libraries(key_view_test PRIVATE libtracer)
libtracer_test_warnings(key_view_test)
add_test(NAME key_view COMMAND key_view_test)

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

add_executable(rope_test rope_test.cpp)
target_link_libraries(rope_test PRIVATE libtracer)
libtracer_test_warnings(rope_test)
add_test(NAME rope COMMAND rope_test)

add_executable(rope_decode_test rope_decode_test.cpp)
target_link_libraries(rope_decode_test PRIVATE libtracer)
libtracer_test_warnings(rope_decode_test)
add_test(NAME rope_decode COMMAND rope_decode_test)

add_executable(tlv_view_test tlv_view_test.cpp)
target_link_libraries(tlv_view_test PRIVATE libtracer)
libtracer_test_warnings(tlv_view_test)
add_test(NAME tlv_view COMMAND tlv_view_test)

add_executable(rope_decode_fuzz_test rope_decode_fuzz_test.cpp)
target_link_libraries(rope_decode_fuzz_test PRIVATE libtracer)
libtracer_test_warnings(rope_decode_fuzz_test)
add_test(NAME rope_decode_fuzz COMMAND rope_decode_fuzz_test)

# #54 — transport_ws RFC 6455 protocol layer (handshake key + frame codec; header-only).
add_executable(ws_test ws_test.cpp)
target_link_libraries(ws_test PRIVATE libtracer)
libtracer_test_warnings(ws_test)
add_test(NAME ws COMMAND ws_test)

# #55 — transport_can pure framing layer (29-bit ID codec, view_can_frames,
# can_reassembly, advertise codec; no SocketCAN). Links libtracer for heap_alloc.
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

# ADR-0044 — stateless transport-peer enumeration + transparent per-peer FWD over
# the CAN bus binding: join hellos -> synthesized /net/can0:children[], directed
# forwarding to an announced peer with the reply routed back, peer expiry, and the
# per-distinct-node peer-table growth. In-memory fake link, no kernel CAN.
add_executable(transport_can_peers_test transport_can_peers_test.cpp)
target_link_libraries(transport_can_peers_test PRIVATE libtracer)
libtracer_test_warnings(transport_can_peers_test)
add_test(NAME transport_can_peers COMMAND transport_can_peers_test)

# #55 (increment 2) — real-bus smoke test over a kernel vcan0 device. Self-skips
# (clean exit) when vcan0 cannot bind, so it is harmless in the normal suite; the
# dedicated can-vcan-e2e CI job sets vcan0 up so the SocketCAN path runs for real.
add_executable(transport_can_vcan_test transport_can_vcan_test.cpp)
target_link_libraries(transport_can_vcan_test PRIVATE libtracer)
libtracer_test_warnings(transport_can_vcan_test)
add_test(NAME transport_can_vcan COMMAND transport_can_vcan_test)

# #383 — owned in-flight TX storage for asynchronous can_link_t backends (the
# TWAI UAF fix's host-testable half): ownership/exhaustion/reuse invariants,
# the acquirer-vs-tx-done-ISR cross-thread contract (TSan-meaningful), and the
# FULL policy — bounded backpressure loses nothing, zero-wait drops are counted.
add_executable(can_tx_pool_test can_tx_pool_test.cpp)
target_link_libraries(can_tx_pool_test PRIVATE libtracer)
libtracer_test_warnings(can_tx_pool_test)
add_test(NAME can_tx_pool COMMAND can_tx_pool_test)

# ADR-0050 — the pure ACL policy seam: both adapters + typed parse/build, no graph.
add_executable(security_acl_test security_acl_test.cpp)
target_link_libraries(security_acl_test PRIVATE libtracer)
libtracer_test_warnings(security_acl_test)
add_test(NAME security_acl COMMAND security_acl_test)

# ADR-0050 — the effective-ACL merge semantics (effective_acl_t, pure, synthetic ACE
# lists) + the graph's cached merge: subtree-precise invalidation (ADR-0057 child
# links) and the dirty-flag protocol under concurrent :acl rewrites (a TSan target).
add_executable(effective_acl_test effective_acl_test.cpp)
target_link_libraries(effective_acl_test PRIVATE libtracer)
libtracer_test_warnings(effective_acl_test)
add_test(NAME effective_acl COMMAND effective_acl_test)

# #81-A — per-endpoint :acl structural storage (store/serve raw ACL TLV; no enforcement).
add_executable(acl_test acl_test.cpp)
target_link_libraries(acl_test PRIVATE libtracer)
libtracer_test_warnings(acl_test)
add_test(NAME acl COMMAND acl_test)

# #407 — graph_t::retire() vertex retirement (RFC-0009 §B/§C/§E.6).
add_executable(retire_test retire_test.cpp)
target_link_libraries(retire_test PRIVATE libtracer)
libtracer_test_warnings(retire_test)
add_test(NAME retire COMMAND retire_test)

# #82 — in-band :children[] SPEC vertex creation (ADR-0017).
add_executable(children_test children_test.cpp)
target_link_libraries(children_test PRIVATE libtracer)
libtracer_test_warnings(children_test)
add_test(NAME children COMMAND children_test)

# L4 fold, Slice 0 — folded :children projection is byte-identical to the materialized
# serialize and is a genuine scatter-gather rope the tlv_view cursor walks.
add_executable(folded_children_test folded_children_test.cpp)
target_link_libraries(folded_children_test PRIVATE libtracer)
libtracer_test_warnings(folded_children_test)
add_test(NAME folded_children COMMAND folded_children_test)

# RFC-0005 — subtree subscriptions (vertical bubbling), branch-write (POINT)
# decomposition, and write-creates (mkdir-p, CREATE-gated).
add_executable(subtree_test subtree_test.cpp)
target_link_libraries(subtree_test PRIVATE libtracer)
libtracer_test_warnings(subtree_test)
add_test(NAME subtree COMMAND subtree_test)

# RFC-0005 §C follow-on — the composed subtree-snapshot read: a plain READ of a
# branch serves the folded POINT tree (per-node stored TLVs verbatim, ACL-pruned,
# zero flatten). Differential over randomized shapes, branch-write round-trip,
# ACL prune, leaf/handler regression, and zero-copy link accounting.
add_executable(subtree_read_test subtree_read_test.cpp)
target_link_libraries(subtree_read_test PRIVATE libtracer)
libtracer_test_warnings(subtree_read_test)
add_test(NAME subtree_snapshot COMMAND subtree_read_test)

# ADR-0041 — terminus arena decoder: decode<->decode_into equivalence over every
# conformance vector + the span/canonical/zero-spill contract.
add_executable(tlv_arena_test tlv_arena_test.cpp)
target_link_libraries(tlv_arena_test PRIVATE libtracer)
libtracer_test_warnings(tlv_arena_test)
target_compile_definitions(tlv_arena_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME tlv_arena COMMAND tlv_arena_test)

# Brick 4 (ADR-0038 §3 / ADR-0039) — per-connection pmr label tables incl. a
# zero-global-heap lifecycle over a slab resource with a null upstream.
add_executable(route_handle_test route_handle_test.cpp)
target_link_libraries(route_handle_test PRIVATE libtracer)
libtracer_test_warnings(route_handle_test)
add_test(NAME route_handle COMMAND route_handle_test)

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

# vertex_t verb interface — a BARE vertex (no graph_t): store/LKV, STREAM ring trim +
# drain cursor, await wake, edge snapshot under concurrent add, ACL verbs.
add_executable(vertex_test vertex_test.cpp)
target_link_libraries(vertex_test PRIVATE libtracer)
libtracer_test_warnings(vertex_test)
add_test(NAME vertex COMMAND vertex_test)

# M3 — L4 graph-runtime harness (single-threaded checks + multithreaded stress).
# The vertex RAM-diet regression gate (#361): compile-time sizeof(vertex_t) ceilings
# plus runtime probes of the lazy cold-extension allocation. static_asserts fire at
# build time; the runtime half runs as an ordinary ctest.
add_executable(vertex_size_test vertex_size_test.cpp)
target_link_libraries(vertex_size_test PRIVATE libtracer)
libtracer_test_warnings(vertex_size_test)
add_test(NAME vertex_size COMMAND vertex_size_test)

# #361 §5 / ADR-0039 §1 — graph_t's injected memory_resource: per-write LKV
# allocations route through it (counting pass-through), released in full at
# graph destruction; a default graph never touches a foreign resource.
add_executable(graph_pmr_test graph_pmr_test.cpp)
target_link_libraries(graph_pmr_test PRIVATE libtracer)
libtracer_test_warnings(graph_pmr_test)
add_test(NAME graph_pmr COMMAND graph_pmr_test)

add_executable(graph_test graph_test.cpp)
target_link_libraries(graph_test PRIVATE libtracer)
libtracer_test_warnings(graph_test)
target_compile_definitions(graph_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME graph COMMAND graph_test)

# RFC-0010 - owner app fields: the field descriptor table, `:settings.app.*` gating,
# the two-part `:schema`, container reads, and the announce-write convention.
# #406 / RFC-0011 — the node identity facet: the byte-exact 60-byte ed25519 record,
# served identically at EVERY vertex (the node-scoped cross-path key), PRE-AUTH through
# a closed ACL (the key is what an unauthenticated peer needs to pin), and absent —
# SCHEMA_NOT_FOUND — on a keyless node. No crypto: the record is a claim.
add_executable(identity_test identity_test.cpp)
target_link_libraries(identity_test PRIVATE libtracer)
libtracer_test_warnings(identity_test)
add_test(NAME identity COMMAND identity_test)

add_executable(app_fields_test app_fields_test.cpp)
target_link_libraries(app_fields_test PRIVATE libtracer)
libtracer_test_warnings(app_fields_test)
add_test(NAME app_fields COMMAND app_fields_test)

# RFC-0004 / ADR-0035 slice 2 — local FWD operation resolution + zero-copy FWD{REPLY}.
add_executable(op_resolve_test op_resolve_test.cpp)
target_link_libraries(op_resolve_test PRIVATE libtracer)
libtracer_test_warnings(op_resolve_test)
target_compile_definitions(op_resolve_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME op_resolve COMMAND op_resolve_test)

# ADR-0053 §7 (3c-ii) — differential oracle: the owning rope-tier resolver
# (resolve(tlv_view_t)) must produce byte-identical replies to the span-tier
# arena resolver for the same request under every rope fragmentation.
add_executable(op_resolve_view_test op_resolve_view_test.cpp)
target_link_libraries(op_resolve_view_test PRIVATE libtracer)
libtracer_test_warnings(op_resolve_view_test)
add_test(NAME op_resolve_view COMMAND op_resolve_view_test)

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

# ADR-0053 ④b — the FWD forward hop over a MULTI-LINK rope, without flattening: an oracle
# equality that every adversarial split routes byte-identically to the contiguous path,
# plus the terminus flatten fallback.
add_executable(fwd_rope_forward_test fwd_rope_forward_test.cpp)
target_link_libraries(fwd_rope_forward_test PRIVATE libtracer)
libtracer_test_warnings(fwd_rope_forward_test)
add_test(NAME fwd_rope_forward COMMAND fwd_rope_forward_test)

# #83 Stage-1 — transport/connection as a / vertex (shell over the live fwd path).
add_executable(transport_vertex_test transport_vertex_test.cpp)
target_link_libraries(transport_vertex_test PRIVATE libtracer)
libtracer_test_warnings(transport_vertex_test)
add_test(NAME transport_vertex COMMAND transport_vertex_test)


# M5 — UDP transport harness (raw frame + two-node end-to-end over localhost UDP).
add_executable(udp_test udp_test.cpp)
target_link_libraries(udp_test PRIVATE libtracer)
libtracer_test_warnings(udp_test)
target_compile_definitions(udp_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME udp COMMAND udp_test)

# M6 — TCP stream transport harness (length-prefix framing: split/coalesced writes,
# oversize-prefix rejection, ADR-0042 owning delivery + backpressure drain, two-node
# FWD end-to-end, and a config-created `kind=tcp` connection).
add_executable(tcp_test tcp_test.cpp)
target_link_libraries(tcp_test PRIVATE libtracer)
libtracer_test_warnings(tcp_test)
add_test(NAME tcp COMMAND tcp_test)

# #54 — transport_ws server socket layer (TCP accept + RFC6455 handshake + transport_t).
add_executable(ws_transport_test ws_transport_test.cpp)
target_link_libraries(ws_transport_test PRIVATE libtracer)
libtracer_test_warnings(ws_transport_test)
add_test(NAME ws_transport COMMAND ws_transport_test)

# transport_t seam conformance — ONE parameterized contract (send/receive, byte-identity,
# scatter-gather, bidirectional, multi-frame, ADR-0042 owning view) over every
# point-to-point adapter: in-process loopback + real localhost udp / tcp / ws.
add_executable(transport_conformance_test transport_conformance_test.cpp)
target_link_libraries(transport_conformance_test PRIVATE libtracer)
libtracer_test_warnings(transport_conformance_test)
add_test(NAME transport_conformance COMMAND transport_conformance_test)

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

# #408 — one node of the containerized decentralized mesh testbed: a real host
# node (graph_t + fwd_router_t + transport_vertex_t with the built-in socket
# factories) that creates only its own LISTENERS in band and waits, while the TS
# driver DIALS every inter-node link by writing a SPEC into its /net:children[]
# remotely (tests/testbed/ + the mesh-testbed CI job). Like the two helpers above
# it is a binary the driver spawns via docker compose, NOT an add_test() — it
# would otherwise block the ctest suite on its deadline.
add_executable(mesh_node mesh_node.cpp)
target_link_libraries(mesh_node PRIVATE libtracer)
libtracer_test_warnings(mesh_node)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/backend_set.cpp
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

# ADR-0043 Phase A — the libtracer_quic MODULE harness (a consumer that talks QUIC
# links the module target; nothing quic-related exists in a default configure):
# raw duplex, split/coalesced chunk reassembly + big-frame chunking, oversize-prefix
# rejection + teardown, ADR-0042 owning delivery + backpressure drain, two-node FWD
# end-to-end over localhost QUIC, and a config-created `kind=quic` connection via the
# register_transport_type extension seam. Only with LIBTRACER_WITH_QUIC (the dedicated
# quic CI job); the test generates its own self-signed dev cert via
# tools/gen-dev-cert.sh at startup.
if(LIBTRACER_WITH_QUIC)
    add_executable(quic_test quic_test.cpp)
    target_link_libraries(quic_test PRIVATE libtracer_quic)
    libtracer_test_warnings(quic_test)
    target_compile_definitions(quic_test PRIVATE
        LIBTRACER_DEV_CERT_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen-dev-cert.sh")
    add_test(NAME quic COMMAND quic_test)
    # libmsquic.so is not TSan-instrumented — suppress races INSIDE the library
    # (TSan cannot see its internal synchronization); libtracer's own code stays
    # fully gated (see tsan-msquic.supp). Harmless for non-TSan builds.
    set_tests_properties(quic PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan-msquic.supp")

    # ADR-0043 Phase B — the WebTransport endpoint harness: the module-private
    # minimal H3/QPACK layer pinned against RFC vectors, then self-contained
    # end-to-end coverage (the module's C++ WebTransport CLIENT half against
    # its server: session establishment, framing over the 0x41 frame channel,
    # ADR-0042 owning delivery + backpressure, the FWD READ round-trip — the
    # #92 shape without a browser) and a config-created `kind=webtransport`.
    add_executable(webtransport_test webtransport_test.cpp)
    target_link_libraries(webtransport_test PRIVATE libtracer_quic)
    libtracer_test_warnings(webtransport_test)
    target_compile_definitions(webtransport_test PRIVATE
        LIBTRACER_DEV_CERT_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen-dev-cert.sh")
    add_test(NAME webtransport COMMAND webtransport_test)
    set_tests_properties(webtransport PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan-msquic.supp")

    # ADR-0043 Phase B (interop) — standalone WebTransport echo harness driven
    # by the TS transport-webtransport browser interop test (puppeteer/chrome,
    # when available). Like ws_interop_server it is a helper binary the TS test
    # spawns, NOT an add_test().
    add_executable(wt_interop_server wt_interop_server.cpp)
    target_link_libraries(wt_interop_server PRIVATE libtracer_quic)
    libtracer_test_warnings(wt_interop_server)
endif()

# mem_cuda GPU test — only with LIBTRACER_WITH_CUDA; run on a real GPU locally
# (tools/test-cuda.sh), never in CI.
if(LIBTRACER_WITH_CUDA)
    add_executable(cuda_test cuda_test.cpp)
    target_link_libraries(cuda_test PRIVATE libtracer)
    libtracer_test_warnings(cuda_test)
    add_test(NAME cuda COMMAND cuda_test)
endif()
