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

# MODULE GATING (#1293). core/CMakeLists.txt compiles the net/routing plane and each
# transport only when its option is ON, so a test target that NAMES a symbol from one of
# those TUs must be gated on the SAME option — otherwise the minimal module set
# (NET_PLANE=OFF plus every transport OFF) configures and compiles the library and then
# fails to LINK its tests. The guards below are target-level on purpose: a test either
# belongs to a module set or it does not, and an in-source #ifdef would hide which.
#
# The conditions mirror the library's own: `tr::net::stream_endpoint_t` and the rest of
# posix_endpoint.cpp are compiled when ANY socket transport is on, so the few tests that
# exercise that shared scaffold directly (rather than one transport) gate on this.
if(LIBTRACER_TRANSPORT_TCP OR LIBTRACER_TRANSPORT_UDP OR LIBTRACER_TRANSPORT_WS)
    set(LIBTRACER_ANY_SOCKET_TRANSPORT ON)
else()
    set(LIBTRACER_ANY_SOCKET_TRANSPORT OFF)
endif()

# Low-level primitive — little-endian (de)serialization (byteorder.hpp).
add_executable(byteorder_test byteorder_test.cpp)
target_link_libraries(byteorder_test PRIVATE libtracer)
libtracer_test_warnings(byteorder_test)
add_test(NAME byteorder COMMAND byteorder_test)

# The nothrow control-plane block seam (mem_source.hpp, #551) — its contract, the
# structural "not a memory_resource" assertions the ruling rests on, and the
# appended graph_t parameter's zero-diff promise.
add_executable(mem_source_test mem_source_test.cpp)
target_link_libraries(mem_source_test PRIVATE libtracer)
libtracer_test_warnings(mem_source_test)
add_test(NAME mem_source COMMAND mem_source_test)

# The std::pmr ADAPTER over the block seam (mem_source_pmr.hpp, #873) — the one direction
# ADR-0079 leaves std::pmr alive in: block_source_t -> std::pmr::memory_resource. Pins the
# pass-through, the sized+aligned release round-trip that makes a pool_source_t RECYCLE
# under it, the family-5 consumer (can_reassembly_t, whose refcounted view_t map cannot be
# retyped to block_array_t), and the adapter's throwing boundary.
#
# THE ABLATION, run by hand (deliberately not an add_test — it is a build-flag variant, and
# an add_test that is expected to FAIL is a green nobody reads):
#   g++ -std=c++23 -O2 -DLIBTRACER_ABLATE_PMR_ADAPTER -I core/include -I core/tests \
#       core/tests/mem_source_pmr_test.cpp core/src/mem_source.cpp core/src/mem_heap.cpp \
#       core/src/backend_set.cpp core/src/mem_pool.cpp core/src/rope.cpp -o ablate_pmr
# (Only these TUs — core/src/*.cpp would drag in the msquic-dependent transports.)
# Ablated, the containers sit on std::pmr::new_delete_resource() and sections 2/3/4/5 go
# RED — 6 failures, because the injected source serves nothing (`allocs_ > 0`, the
# alignment witness, and both `pool.used() > 0` claims all fail). Unablated the same binary
# reports ALL PASS.
add_executable(mem_source_pmr_test mem_source_pmr_test.cpp)
target_link_libraries(mem_source_pmr_test PRIVATE libtracer)
libtracer_test_warnings(mem_source_pmr_test)
add_test(NAME mem_source_pmr COMMAND mem_source_pmr_test)

# The bounded RECYCLING source (pool_source_t, #597) — the long-lived-seam gap that
# heap_source (unbounded) and bump_source (never recycles) between them left open.
add_executable(mem_pool_source_test mem_pool_source_test.cpp)
target_link_libraries(mem_pool_source_test PRIVATE libtracer)
libtracer_test_warnings(mem_pool_source_test)
add_test(NAME mem_pool_source COMMAND mem_pool_source_test)

# u32-length-prefix stream reassembler (length_prefix_framer.hpp) — the extracted
# transport_quic/webtransport RX state machine, tested without a live connection.
add_executable(length_prefix_framer_test length_prefix_framer_test.cpp)
target_link_libraries(length_prefix_framer_test PRIVATE libtracer)
libtracer_test_warnings(length_prefix_framer_test)
add_test(NAME length_prefix_framer COMMAND length_prefix_framer_test)

if(LIBTRACER_NET_PLANE)
    # FWD offset-dispatch frame view (fwd_frame_view.hpp) — the extracted fwd_router
    # forward-plane cluster, tested directly over hand-built frames (span + rope
    # cursors): dst-seg / op / control peeks, the shrunk-dst/grown-src head rebuild
    # proved byte-exact vs a reference re-encode, and malformed rejects.
    add_executable(fwd_frame_view_test fwd_frame_view_test.cpp)
    target_link_libraries(fwd_frame_view_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_frame_view_test)
    add_test(NAME fwd_frame_view COMMAND fwd_frame_view_test)
endif()

# The shared SPEC-config SETTINGS walk (config_reader.hpp) — the pair-consuming scan
# every transport config parser funnels through (#927): the value-read-as-a-key hijack
# vectors, the forward-compat tolerance that must SURVIVE the fix (the opposite ruling
# from the ACL parse), and the representative per-kind configs pinned unchanged.
add_executable(config_reader_test config_reader_test.cpp)
target_link_libraries(config_reader_test PRIVATE libtracer)
libtracer_test_warnings(config_reader_test)
add_test(NAME config_reader COMMAND config_reader_test)
# Pins the shared #995 walk-parity vector on the reader, so it needs
# LIBTRACER_VECTORS_DIR.
target_compile_definitions(config_reader_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")

# Canonical-key NAME navigation (key_view.hpp) — the locus the graph walks funnel through.
add_executable(key_view_test key_view_test.cpp)
target_link_libraries(key_view_test PRIVATE libtracer)
libtracer_test_warnings(key_view_test)
add_test(NAME key_view COMMAND key_view_test)

# Path parsing / validation — reserved chars, limits, field tail (reference/03).
# Reads the shared path/path-reserved-brackets vector (#996), so it needs
# LIBTRACER_VECTORS_DIR.
add_executable(path_test path_test.cpp)
target_link_libraries(path_test PRIVATE libtracer)
libtracer_test_warnings(path_test)
target_compile_definitions(path_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME path COMMAND path_test)

if(LIBTRACER_NET_PLANE)
    # PATH_REF (0x14) codec — the bound-path envelope, element layout, count bound and
    # the structural rejects (RFC-0024 §4), plus the published vectors pinned against
    # the builder.
    # RFC-0024 car 2 — what a PATH_REF MEANS: the node-scoped slot index, the mint exchange,
    # and every way a bound path is refused rather than mis-routed.
    add_executable(bound_path_test bound_path_test.cpp)
    target_link_libraries(bound_path_test PRIVATE libtracer)
    libtracer_test_warnings(bound_path_test)
    target_compile_definitions(bound_path_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    add_test(NAME bound_path COMMAND bound_path_test)
endif()

if(LIBTRACER_NET_PLANE)
    # RFC-0024 car 3 — the FORWARDER hop and the origin-side bind, over a three-node
    # loopback chain with production `/net:children[]` wiring.
    add_executable(bound_forward_test bound_forward_test.cpp)
    target_link_libraries(bound_forward_test PRIVATE libtracer)
    libtracer_test_warnings(bound_forward_test)
    target_compile_definitions(bound_forward_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    add_test(NAME bound_forward COMMAND bound_forward_test)
endif()

add_executable(path_ref_test path_ref_test.cpp)
target_link_libraries(path_ref_test PRIVATE libtracer)
libtracer_test_warnings(path_ref_test)
add_test(NAME path_ref COMMAND path_ref_test)

# RFC-0027's path label (path_label.hpp) and its mint table (path_label_table.cpp, so
# NET_PLANE-gated like every other net-plane TU's test). Carries §4.3.1's
# saturate-and-retire test, which nothing else can catch: the rule is invisible on the wire.
# It also pins the §12.5 `path-label/` vectors byte-exactly against the emitter, so it needs
# LIBTRACER_VECTORS_DIR.
if(LIBTRACER_NET_PLANE)
    add_executable(path_label_test path_label_test.cpp)
    target_link_libraries(path_label_test PRIVATE libtracer)
    target_compile_definitions(path_label_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    libtracer_test_warnings(path_label_test)
    add_test(NAME path_label COMMAND path_label_test)
endif()

# RFC-0027 car 3 — the MIXED-element codec (path_element.hpp) and the origin-side label
# cache on path_t (§6.1). Deliberately NOT net-plane-gated: both live at L2/L4 and neither
# links a net-plane TU, so a core built without the transport plane still gets the layering
# checks of §9 — which is the arm most likely to erode, since it is a rule about what must
# NOT appear on a surface.
add_executable(path_label_codec_test path_label_codec_test.cpp)
target_link_libraries(path_label_codec_test PRIVATE libtracer)
libtracer_test_warnings(path_label_codec_test)
add_test(NAME path_label_codec COMMAND path_label_codec_test)

# RFC-0027 car 4 — the FORWARDER: mint on reply, deref on receipt, NOT_FOUND on stale, and
# §8.2's mandated label-vs-string ACL pair. Every claim here is behavioural, and the
# conformance harness routes nothing, so this is the only place they can be false — the same
# reason `bound_forward_test` exists for `fwd/fwd-bound-forward`. NET_PLANE-gated (it drives
# `fwd_router_t`) and vector-reading (it pins four vectors byte-exact against the router).
if(LIBTRACER_NET_PLANE)
    add_executable(path_label_forward_test path_label_forward_test.cpp)
    target_link_libraries(path_label_forward_test PRIVATE libtracer)
    target_compile_definitions(path_label_forward_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    libtracer_test_warnings(path_label_forward_test)
    add_test(NAME path_label_forward COMMAND path_label_forward_test)

    # RFC-0027 §6.1 point 3 — the TERMINUS: the residual half of the same reply-leg rewrite,
    # bound against the production `fwd_router_t` + `op_resolver_t` wiring for the same reason.
    add_executable(path_label_terminus_test path_label_terminus_test.cpp)
    target_link_libraries(path_label_terminus_test PRIVATE libtracer)
    target_compile_definitions(path_label_terminus_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    libtracer_test_warnings(path_label_terminus_test)
    add_test(NAME path_label_terminus COMMAND path_label_terminus_test)

    # RFC-0027 origin car — the ORIGIN: adopt the minted `src`, spend it as `dst`, fall back on
    # `NOT_FOUND`. The two above pin the hop's EMISSION; this one closes the round trip, and it
    # is where `fwd/fwd-label-stale`'s "the bytes an origin sends" stops being hand-built.
    add_executable(path_label_origin_test path_label_origin_test.cpp)
    target_link_libraries(path_label_origin_test PRIVATE libtracer)
    target_compile_definitions(path_label_origin_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    libtracer_test_warnings(path_label_origin_test)
    add_test(NAME path_label_origin COMMAND path_label_origin_test)
endif()

# The wire grammar's total-encoded-size bound (grammar.hpp, #921) — pinned at
# 32-bit-`size_t` width, because the truncation it closes is unreachable at the
# host's 64-bit width and a host-only vector would be a vacuous guard.
add_executable(grammar_bounds_test grammar_bounds_test.cpp)
target_link_libraries(grammar_bounds_test PRIVATE libtracer)
libtracer_test_warnings(grammar_bounds_test)
add_test(NAME grammar_bounds COMMAND grammar_bounds_test)

# Frame codec — nesting depth cap / iterative-parse contract (reference/01).
add_executable(frame_test frame_test.cpp)
target_link_libraries(frame_test PRIVATE libtracer)
libtracer_test_warnings(frame_test)
add_test(NAME frame COMMAND frame_test)

add_executable(rope_test rope_test.cpp)
target_link_libraries(rope_test PRIVATE libtracer)
libtracer_test_warnings(rope_test)
add_test(NAME rope COMMAND rope_test)

# rope_cursor bounds preconditions (#916) — the debug-assert parity with
# view_t::subview. The asserts are debug-only, so this TU forces NDEBUG off and
# observes each abort in a forked child; POSIX-only (fork/waitpid).
if(UNIX)
    add_executable(rope_cursor_assert_test rope_cursor_assert_test.cpp)
    target_link_libraries(rope_cursor_assert_test PRIVATE libtracer)
    libtracer_test_warnings(rope_cursor_assert_test)
    add_test(NAME rope_cursor_assert COMMAND rope_cursor_assert_test)
endif()

# The RELEASE half of the same contract (#986): the clamp-and-latch that survives
# NDEBUG, which the assert test above cannot speak for. The TU defines NDEBUG itself
# (before any include) so it holds under every CMAKE_BUILD_TYPE, and this target adds
# it again so the intent is visible in the build graph rather than only in the source.
add_executable(rope_cursor_release_guard_test rope_cursor_release_guard_test.cpp)
target_link_libraries(rope_cursor_release_guard_test PRIVATE libtracer)
target_compile_definitions(rope_cursor_release_guard_test PRIVATE NDEBUG)
libtracer_test_warnings(rope_cursor_release_guard_test)
add_test(NAME rope_cursor_release_guard COMMAND rope_cursor_release_guard_test)

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

if(LIBTRACER_TRANSPORT_CAN)
    # #55 (increment 2) — transport_can SocketCAN binding, exercised over an in-memory
    # fake link (no kernel CAN needed; the real vcan path runs in the can-vcan-e2e CI job).
    add_executable(transport_can_test transport_can_test.cpp)
    target_link_libraries(transport_can_test PRIVATE libtracer)
    libtracer_test_warnings(transport_can_test)
    add_test(NAME transport_can COMMAND transport_can_test)
endif()

if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_CAN AND LIBTRACER_TRANSPORT_TCP AND LIBTRACER_TRANSPORT_UDP AND LIBTRACER_TRANSPORT_WS)
    # #848 — the transport EGRESS framing path soft-fails: a refusing allocator yields a
    # DROPPED FRAME AND A LIVE NODE (never abort()), across ws / tcp / udp / can, plus the
    # RFC 6455 §5.5 control-frame rules that make the PONG unfailable. This TU overrides the
    # global operator new with a fail-the-k-th-allocation injector, so it must stay its OWN
    # executable (the override is process-wide).
    add_executable(transport_alloc_softfail_test transport_alloc_softfail_test.cpp)
    target_link_libraries(transport_alloc_softfail_test PRIVATE libtracer)
    libtracer_test_warnings(transport_alloc_softfail_test)
    add_test(NAME transport_alloc_softfail COMMAND transport_alloc_softfail_test)
endif()

if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_CAN)
    # ADR-0044 — stateless transport-peer enumeration + transparent per-peer FWD over
    # the CAN bus binding: join hellos -> synthesized /net/can0:children[], directed
    # forwarding to an announced peer with the reply routed back, peer expiry, and the
    # per-distinct-node peer-table growth. In-memory fake link, no kernel CAN.
    add_executable(transport_can_peers_test transport_can_peers_test.cpp)
    target_link_libraries(transport_can_peers_test PRIVATE libtracer)
    libtracer_test_warnings(transport_can_peers_test)
    add_test(NAME transport_can_peers COMMAND transport_can_peers_test)
endif()

# #55 (increment 2) — real-bus smoke test over a kernel vcan0 device, plus the #931
# can_link_t seam-rule vectors (an injected RTR / 11-bit standard frame must not
# cross ingress; an over-length classic frame must not cross egress). Self-skips
# (clean exit) when vcan0 cannot bind, so it is harmless in the normal suite; the
# dedicated can-vcan-e2e CI job sets vcan0 up so the SocketCAN path runs for real.
# LINUX-ONLY: it speaks <linux/can.h> directly to inject the frames no well-formed
# link would emit — platform selection is a build-system concern, never an in-source
# #ifdef (the same doctrine that picks socketcan_link.cpp over its stub). It also links
# socketcan_link_t itself, so it needs the CAN module as well as the platform (#1293).
if(LIBTRACER_TRANSPORT_CAN AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(transport_can_vcan_test transport_can_vcan_test.cpp)
  target_link_libraries(transport_can_vcan_test PRIVATE libtracer)
  libtracer_test_warnings(transport_can_vcan_test)
  add_test(NAME transport_can_vcan COMMAND transport_can_vcan_test)
endif()

# #383 — owned in-flight TX storage for asynchronous can_link_t backends (the
# TWAI UAF fix's host-testable half): ownership/exhaustion/reuse invariants,
# the acquirer-vs-tx-done-ISR cross-thread contract (TSan-meaningful), and the
# FULL policy — bounded backpressure loses nothing, zero-wait drops are counted.
add_executable(can_tx_pool_test can_tx_pool_test.cpp)
target_link_libraries(can_tx_pool_test PRIVATE libtracer)
libtracer_test_warnings(can_tx_pool_test)
add_test(NAME can_tx_pool COMMAND can_tx_pool_test)

# #816 — the ESP-IDF httpd WS link's ADOPTED-MODE TEARDOWN. The real chip TU
# (integrations/esp-idf/libtracer/httpd_ws_link.cpp) is compiled here against a host
# fake of the one IDF facility the teardown contract rests on: the session table
# (free_ctx stored by the server, run later, on the server's task) and the control work
# queue. That is what turns "the destructor left a callback pointing into freed memory"
# into something a host sanitizer can catch. The rest of the link (sockets, framing) is
# still proved only by the on-silicon esp-idf workflow build.
#
# This is the ONE target in core/ whose sources live outside core/, so it is guarded on
# their presence: core/ must still configure as a standalone tree (the mesh-testbed image
# copies core/ and nothing else, and any consumer may do the same). Without the guard the
# file list resolves to nothing and CMake aborts with "No SOURCES given to target".
set(LIBTRACER_ESP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../integrations/esp-idf")
if(EXISTS "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    add_executable(httpd_ws_teardown_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_teardown_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_teardown_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_teardown_test PRIVATE libtracer)
    # The fake's socket layer interposes the ONE ::send the link's send override does
    # (see the send-stall target below); every target that links fake_httpd.cpp needs it.
    target_link_options(httpd_ws_teardown_test PRIVATE "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_teardown_test)
    add_test(NAME httpd_ws_teardown COMMAND httpd_ws_teardown_test)
    # Several cases here PROVE a deliberate leak (a teardown that cannot retire the
    # server's callbacks must leak the session shells and the handler gate rather than
    # free them under a live callback — the #815 precedent), so LeakSanitizer would flag
    # the very behaviour under test. ASan's use-after-free detection, which is the point
    # of running this suite under sanitizers, stays on.
    set_tests_properties(httpd_ws_teardown PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #835 — the BOUNDED send and the aim of the brokenness detector, on the same fake:
    # a jammed control queue must strike nobody, a failing send strikes its destination,
    # and a SHORT write closes the session instead of being reported as delivered. The
    # starvation itself (SO_SNDTIMEO under fan-out) is silicon-only and stays on the HIL.
    add_executable(httpd_ws_send_stall_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_send_stall_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_send_stall_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_send_stall_test PRIVATE libtracer)
    # The link's send override does a plain BSD ::send on the peer's fd (esp_http_server
    # keeps its own default send private, and routing back through httpd_socket_send would
    # re-enter the override). Interposing that ONE call is what lets the fake script a
    # FULL / timed-out / SHORT write; descriptors the fake did not hand out fall through
    # to the real syscall, so nothing else in the binary changes behaviour. `shutdown` is
    # interposed on the same rule: it is the link's forced close, and the only close path
    # that does not ride the control socket a jammed queue starves.
    target_link_options(httpd_ws_send_stall_test PRIVATE "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_send_stall_test)
    add_test(NAME httpd_ws_send_stall COMMAND httpd_ws_send_stall_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent the
    # teardown suite pins); every case here constructs an adopting link, so LeakSanitizer
    # would flag that deliberate leak. ASan's use-after-free detection stays on.
    set_tests_properties(httpd_ws_send_stall PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #954 — SESSION IDENTITY on the TX path, on the same fake. The event neither suite
    # above stages is a DESCRIPTOR being reused: the fake hands one socket number to two
    # unrelated sessions in turn, while the first one's frames are still in the control
    # queue. That is what makes "the send resolved its destination by bare fd" observable
    # as a cross-session misdelivery rather than a code reading.
    add_executable(httpd_ws_session_identity_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_session_identity_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_session_identity_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_session_identity_test PRIVATE libtracer)
    target_link_options(httpd_ws_session_identity_test PRIVATE
        "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_session_identity_test)
    add_test(NAME httpd_ws_session_identity COMMAND httpd_ws_session_identity_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the two suites above.
    set_tests_properties(httpd_ws_session_identity
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    if(LIBTRACER_NET_PLANE)
        # #1071 — the DEPARTURE's cost, on the same fake but with a real graph_t + fwd_router_t
        # behind the link: the subject is what a peer hanging up charges the httpd task, which is
        # paid inside graph_t::evict_link_edges. Needs core/tests on the include path for the
        # frame/value builders the wire-subscribe door takes.
        add_executable(httpd_ws_departure_cost_test
            "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_departure_cost_test.cpp"
            "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
            "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
        target_include_directories(httpd_ws_departure_cost_test PRIVATE
            "${LIBTRACER_ESP_DIR}/tests/host"
            "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
            "${LIBTRACER_ESP_DIR}/libtracer/include"
            "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(httpd_ws_departure_cost_test PRIVATE libtracer)
        target_link_options(httpd_ws_departure_cost_test PRIVATE
            "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
        libtracer_test_warnings(httpd_ws_departure_cost_test)
        add_test(NAME httpd_ws_departure_cost COMMAND httpd_ws_departure_cost_test)
        # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and this
        # suite constructs an adopting link — same exemption as the suites around it.
        set_tests_properties(httpd_ws_departure_cost
            PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
    endif()

    # #1146 — close_peer: the administrative teardown of ONE inbound session, on the same
    # fake. The subject here is the CONTROL QUEUE rather than the socket: the close must be
    # marshalled onto the httpd task (the task that owns the descriptor's lifetime), so the
    # return value is only as honest as httpd_queue_work's verdict — which is why the suite
    # stages a FULL mbox and a refusing control socket, the two refusal routes the
    # component's ESP-IDF floor makes visible.
    add_executable(httpd_ws_close_peer_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_close_peer_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_close_peer_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_close_peer_test PRIVATE libtracer)
    target_link_options(httpd_ws_close_peer_test PRIVATE "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_close_peer_test)
    add_test(NAME httpd_ws_close_peer COMMAND httpd_ws_close_peer_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_close_peer
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #949 — the TX SLOT POOL as the link's outstanding-send bound, on the same fake.
    # It replaces the #944 strand suite: above the component's ESP-IDF floor (>=5.5.5) a
    # full control mbox refuses the enqueue instead of binning it in silence, so the slot
    # always comes back, and a send that finds no free slot is dropped and counted rather
    # than posted from a heap work item.
    add_executable(httpd_ws_tx_pool_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_tx_pool_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_tx_pool_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_tx_pool_test PRIVATE libtracer)
    target_link_options(httpd_ws_tx_pool_test PRIVATE "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_tx_pool_test)
    add_test(NAME httpd_ws_tx_pool COMMAND httpd_ws_tx_pool_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_tx_pool
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #1494 — the IN-CALL REPLY leaves by the socket, not by the control queue. The pool
    # suite above pins what a QUEUED send costs; this one pins that a reply serviced inside
    # the handler frame pays none of it, and survives the full pool + refusing queue that
    # used to silence the session permanently.
    add_executable(httpd_ws_in_call_reply_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_in_call_reply_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_in_call_reply_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_in_call_reply_test PRIVATE libtracer)
    target_link_options(httpd_ws_in_call_reply_test PRIVATE
        "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_in_call_reply_test)
    add_test(NAME httpd_ws_in_call_reply COMMAND httpd_ws_in_call_reply_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_in_call_reply
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #961 — the FAN-OUT path's allocation, on the same fake. The instrument is what is
    # new: the suite replaces the WHOLE global operator new/delete family (whole because a
    # subset makes ASan abort on a mismatched pair), so "a broadcast allocates" is a number
    # rather than a code reading. The destination snapshot used to be a std::vector, whose
    # throwing allocator put an abort() ahead of every nothrow fallback on the push path.
    add_executable(httpd_ws_fanout_alloc_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_fanout_alloc_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_fanout_alloc_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_fanout_alloc_test PRIVATE libtracer)
    target_link_options(httpd_ws_fanout_alloc_test PRIVATE
        "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_fanout_alloc_test)
    add_test(NAME httpd_ws_fanout_alloc COMMAND httpd_ws_fanout_alloc_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_fanout_alloc
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #1187 — the fan-out WAIT: the TX pool is an in-flight bound, not a fan-out ceiling, so
    # a sweep wider than it costs latency rather than losing the same publish-order tail
    # every pass. What this target adds over the two suites above is the SCHEDULING model —
    # its drainer runs only while the producer is blocked, which is the unicore ordering that
    # made the loss deterministic on silicon and which a free-running host thread hides.
    add_executable(httpd_ws_fanout_wait_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_fanout_wait_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_fanout_wait_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_fanout_wait_test PRIVATE libtracer)
    target_link_options(httpd_ws_fanout_wait_test PRIVATE
        "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_fanout_wait_test)
    add_test(NAME httpd_ws_fanout_wait COMMAND httpd_ws_fanout_wait_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_fanout_wait
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #1218 — the fan-out WIDTH: the in-call reserve is a slot of its own, never a slice out
    # of the pool, so a sweep that FITS tx_slot_capacity() still fits it on every pass and
    # never waits. The complement of the suite above (which asks what a sweep WIDER than the
    # pool does): carving the reserve out made the two the same question and starved the
    # steady-state tail.
    add_executable(httpd_ws_fanout_width_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_fanout_width_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_fanout_width_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_fanout_width_test PRIVATE libtracer)
    target_link_options(httpd_ws_fanout_width_test PRIVATE
        "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_fanout_width_test)
    add_test(NAME httpd_ws_fanout_width COMMAND httpd_ws_fanout_width_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_fanout_width
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #960 — the peer-departure eviction notifier's LOCK DISCIPLINE, on the same fake. The
    # event none of the suites above stages is a departure notification held OPEN: the
    # notifier parks the server task while another task tries to acquire the handler gate.
    # That is what turns "an unbounded foreign callback runs under gate_t::m" into a number
    # instead of a code reading, and it pins the other half at the same time — the
    # destructor must still join a notification in flight, now through the gate's barrier
    # rather than its mutex.
    add_executable(httpd_ws_eviction_lock_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_eviction_lock_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_eviction_lock_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_eviction_lock_test PRIVATE libtracer)
    target_link_options(httpd_ws_eviction_lock_test PRIVATE
        "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_eviction_lock_test)
    add_test(NAME httpd_ws_eviction_lock COMMAND httpd_ws_eviction_lock_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_eviction_lock
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #957 — the httpd WS link NOTICES a peer that vanished without a FIN. The seam none
    # of the suites above reaches is the SOCKET OPTION one: `setsockopt` is interposed
    # here for the descriptors the fake hands out, so what admission applies to a peer's
    # socket is a measurement. Nothing else in the binary is affected — descriptors the
    # fake did not hand out fall through to the real syscall, the `--wrap=send` rule.
    add_executable(httpd_ws_keepalive_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_keepalive_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_keepalive_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_keepalive_test PRIVATE libtracer)
    target_link_options(httpd_ws_keepalive_test PRIVATE
        "-Wl,--wrap=send" "-Wl,--wrap=shutdown" "-Wl,--wrap=setsockopt")
    libtracer_test_warnings(httpd_ws_keepalive_test)
    add_test(NAME httpd_ws_keepalive COMMAND httpd_ws_keepalive_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_keepalive
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #958 — the admission predicate decides the opening HANDSHAKE. The seam none of the
    # suites above reaches is the pre-handshake one: the fake runs the registration's
    # ws_pre_handshake_cb before it admits a socket and latches the WS route, the order
    # httpd_uri.c uses, so whether a refusal actually stops the upgrade is a measurement.
    # No extra interposition — this suite asks nothing of the socket layer.
    add_executable(httpd_ws_admission_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_admission_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_admission_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_admission_test PRIVATE libtracer)
    # The fake's socket layer interposes the ONE ::send the link's send override does;
    # every target that links fake_httpd.cpp needs it.
    target_link_options(httpd_ws_admission_test PRIVATE "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_admission_test)
    add_test(NAME httpd_ws_admission COMMAND httpd_ws_admission_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # three of the four cases here construct an adopting link — same exemption as above.
    set_tests_properties(httpd_ws_admission
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #1184 — the post-handshake AUTH frame, which is the seam after the one above: the
    # predicate decides the handshake, this decides the session. Two fakes carry it — the
    # http server's, plus the esp_timer whose periodic sweep bounds how long a session may
    # stay unauthenticated. The timer fake never fires on its own, so the deadline is a
    # decision the test makes rather than a wall-clock race it has to win.
    add_executable(httpd_ws_auth_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_auth_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_auth_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_auth_test PRIVATE libtracer)
    # The close path writes a CLOSE frame and then shuts the socket down, so this suite needs
    # both interpositions the fake's socket layer provides.
    target_link_options(httpd_ws_auth_test PRIVATE "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_auth_test)
    add_test(NAME httpd_ws_auth COMMAND httpd_ws_auth_test)
    # Every case constructs an adopting link, whose teardown leaks the handler gate BY
    # DESIGN (the #815 precedent) — same exemption as the suites above.
    set_tests_properties(httpd_ws_auth
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #953 — the LINK-level failure tally: the drop classes with no session to charge,
    # plus the two session-lifecycle facts nothing else records.
    add_executable(httpd_ws_counters_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_counters_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_counters_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_counters_test PRIVATE libtracer)
    target_link_options(httpd_ws_counters_test PRIVATE "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_counters_test)
    add_test(NAME httpd_ws_counters COMMAND httpd_ws_counters_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent); every
    # case here constructs an adopting link.
    set_tests_properties(httpd_ws_counters
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #955 — what the link owes an ADOPTED server. The facts none of the suites above can
    # see are server-level: the `httpd_config_t` a `httpd_start` was given (the real server
    # keeps it private, which is precisely why the adopting ctor cannot check it) and each
    # session's LRU counter, which IDF advances from inbound processing only. Both are
    # modelled in the fake here, so "the owning ctor applies the public stack figure" and
    # "a pushed-to subscriber stops being the purge victim" are measurements.
    add_executable(httpd_ws_adopted_host_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_adopted_host_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_adopted_host_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_adopted_host_test PRIVATE libtracer)
    target_link_options(httpd_ws_adopted_host_test PRIVATE
        "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_adopted_host_test)
    add_test(NAME httpd_ws_adopted_host COMMAND httpd_ws_adopted_host_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and most
    # cases here construct an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_adopted_host
        PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")

    # #900 + #901 — the WS *CLIENT* link's receive path, on the same construction: the
    # chip TU (integrations/esp-idf/libtracer/esp_ws_client_link.cpp) compiled against a
    # host fake of esp_transport_ws's frame stream and esp_pthread's stack-sizing seam.
    # Those are the two IDF facilities the contracts rest on — a `recv_stack` that never
    # reaches esp_pthread_set_cfg never reaches the FreeRTOS task, and a read capped at
    # the offered buffer is what splits ONE oversized frame across reads that each report
    # the same fin. NO socket is interposed or opened here (contrast the httpd targets
    # above, which wrap ::send): the fake's esp_transport_get_socket answers -1, keeping
    # the suite faithful to the ruling that this plane never uses POSIX sockets (#947).
    add_executable(esp_ws_client_recv_test
        "${LIBTRACER_ESP_DIR}/tests/host/esp_ws_client_recv_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_esp_transport.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/esp_ws_client_link.cpp")
    target_include_directories(esp_ws_client_recv_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(esp_ws_client_recv_test PRIVATE libtracer)
    libtracer_test_warnings(esp_ws_client_recv_test)
    add_test(NAME esp_ws_client_recv COMMAND esp_ws_client_recv_test)

    # #952 — the WS CLIENT link's BOUNDED-BLOCKING discipline, on the same TU and the
    # same fake: what a send may cost with the send serializer held, what a teardown may
    # cost, and who may still be inside the transport when the handles are destroyed.
    # The receive-path suite above scripts the fake's inbound frames; this one scripts
    # its BLOCKING (a dial that hangs, a dial that fails, a write that parks), because
    # every defect here is a wait nobody bounded. The use-after-free observable is the
    # fake's handle LIVENESS: its parked write re-checks it on the way out, which is what
    # makes "the handles were destroyed under a sender" a measurement rather than a code
    # reading. Still no socket anywhere (#947). Its last case has a second oracle: it
    # sends straight through a failing-dial cycle, where the recv thread rewrites the
    # transport handles with no lock held, so the `tsan` leg judges whether the sender's
    # handle read is behind the `connected_` acquire gate that publishes them.
    add_executable(esp_ws_client_teardown_test
        "${LIBTRACER_ESP_DIR}/tests/host/esp_ws_client_teardown_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_esp_transport.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/esp_ws_client_link.cpp")
    target_include_directories(esp_ws_client_teardown_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(esp_ws_client_teardown_test PRIVATE libtracer)
    libtracer_test_warnings(esp_ws_client_teardown_test)
    add_test(NAME esp_ws_client_teardown COMMAND esp_ws_client_teardown_test)
    # Three cases spend a real wait on purpose (a 600 ms held write, a 2.5 s hung dial,
    # and two 1.5 s reconnect backoffs to straddle a pair of handle rebuilds).
    # Generous against a loaded runner, and still far under CTest's default.
    set_tests_properties(esp_ws_client_teardown PROPERTIES TIMEOUT 60)

    # #957 — the WS CLIENT link's PEER-DEATH seam, on the same TU and the same fake:
    # whether a dial asks the stack to notice a peer that vanished without a FIN, and
    # whether the link REPORTS a departure to the router at all (it used to fire
    # notify_down from nowhere, so a peer reboot left this node producing into a session
    # that no longer existed). Unlike the two client suites above, this one opts the fake
    # into answering a descriptor NUMBER from esp_transport_get_socket and interposes
    # `setsockopt`, which is what makes the link's best-effort option block observable.
    # Still no socket: nothing is opened and no option reaches the kernel (#947).
    add_executable(esp_ws_client_liveness_test
        "${LIBTRACER_ESP_DIR}/tests/host/esp_ws_client_liveness_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_esp_transport.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/esp_ws_client_link.cpp")
    target_include_directories(esp_ws_client_liveness_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(esp_ws_client_liveness_test PRIVATE libtracer)
    target_link_options(esp_ws_client_liveness_test PRIVATE "-Wl,--wrap=setsockopt")
    libtracer_test_warnings(esp_ws_client_liveness_test)
    add_test(NAME esp_ws_client_liveness COMMAND esp_ws_client_liveness_test)
    # Two cases spend real reconnect backoffs on purpose (1.5 s each, twice over for the
    # failed-dial case). Generous against a loaded runner, still far under CTest's default.
    set_tests_properties(esp_ws_client_liveness PROPERTIES TIMEOUT 60)

    # #959 — the WS CLIENT link's DIAL/CONFIG surface, on the same TU and the same fake.
    # The seam none of the client suites above reaches is ORDERING at construction: the
    # ctor spawns the thread that dials, so anything a caller supplied afterwards raced that
    # dial — for a token, leaving UNDEFINED whether dial ONE carried what admission gates on.
    # The fake's observable is therefore PER DIAL (fake_ws::dial_headers), because "the
    # LAST dial carried the token" is exactly what the broken surface would also report. No
    # socket and no setsockopt interposition here (#947), unlike the liveness target above:
    # this suite is about what the handshake requested, not the socket it went out on.
    # Registered after the three suites it shares a TU with so their ctest indices do not
    # move — #1118 tracks a known flake by index as well as by name.
    add_executable(esp_ws_client_dial_test
        "${LIBTRACER_ESP_DIR}/tests/host/esp_ws_client_dial_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_esp_transport.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/esp_ws_client_link.cpp")
    target_include_directories(esp_ws_client_dial_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(esp_ws_client_dial_test PRIVATE libtracer)
    libtracer_test_warnings(esp_ws_client_dial_test)
    add_test(NAME esp_ws_client_dial COMMAND esp_ws_client_dial_test)
    # One case spends a real reconnect backoff on purpose (1.5 s), to make the second dial
    # a genuine handle rebuild rather than a re-use. Generous against a loaded runner,
    # still far under CTest's default.
    set_tests_properties(esp_ws_client_dial PROPERTIES TIMEOUT 60)

    # #962 — the TWAI link's TX BACKPRESSURE window, on the same construction: the chip
    # TU (integrations/esp-idf/libtracer/twai_link.cpp) compiled against a host fake of
    # esp_driver_twai's node API plus the FreeRTOS counting semaphore and queue it is
    # built on (fake_twai.hpp). This is the first host target to compile that TU at all,
    # leaving the TWAI link's own FULL policy untested on any host until now. It is NOT
    # the first host coverage of the CAN plane: transport_can_vcan_test drives real
    # socketcan_link_t instances over kernel vcan0 and has its own CI leg
    # (.github/workflows/can-vcan-e2e.yml). The gap this closes is the TWAI link, not CAN.
    #
    # The fake's semaphore counts callers parked in xSemaphoreTake AT ONCE, which is what
    # makes "the wait was serialized behind the write lock" observable rather than a code
    # reading. No socket is interposed and none is opened: the TWAI plane has none.
    add_executable(twai_backpressure_test
        "${LIBTRACER_ESP_DIR}/tests/host/twai_backpressure_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_twai.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/twai_link.cpp")
    target_include_directories(twai_backpressure_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(twai_backpressure_test PRIVATE libtracer)
    libtracer_test_warnings(twai_backpressure_test)
    add_test(NAME twai_backpressure COMMAND twai_backpressure_test)
    # Wall-clock separations of 3-5x, on a suite that also parks threads on purpose.
    # Generous against a loaded runner, and still far under CTest's default.
    set_tests_properties(twai_backpressure PROPERTIES TIMEOUT 60)

    # #1029 — the same chip TU and the same fake, driven from the OTHER direction. The
    # fake modelled TX only: twai_node_receive_from_isr failed unconditionally and the
    # registered rx-done callback was never invoked, so twai_link_t::on_rx_done_isr ran
    # only on silicon. With script_rx/deliver_one_rx (the mirror of complete_one_tx) this
    # pins what that body decides: the seam's shared ingress rule (can_rx_admissible, #931
    # — the rule the two platform links diverged on once), the DLC decode capped at the
    # classic width, and the ISR->dispatch handoff DROPPING rather than blocking when
    # rx_queue_ is full. Production behaviour is unchanged by that issue; this is coverage.
    add_executable(twai_rx_ingress_test
        "${LIBTRACER_ESP_DIR}/tests/host/twai_rx_ingress_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_twai.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/twai_link.cpp")
    target_include_directories(twai_rx_ingress_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(twai_rx_ingress_test PRIVATE libtracer)
    libtracer_test_warnings(twai_rx_ingress_test)
    add_test(NAME twai_rx_ingress COMMAND twai_rx_ingress_test)
    # One case parks the dispatch thread on purpose and then waits out a stragglers'
    # window; the rest are sub-second. Generous against a loaded runner.
    set_tests_properties(twai_rx_ingress PROPERTIES TIMEOUT 60)
else()
    message(STATUS "httpd_ws_teardown_test skipped: integrations/esp-idf is not in this tree")
endif()

# ADR-0050 — the pure ACL policy seam: both adapters + typed parse/build, no graph.
add_executable(security_acl_test security_acl_test.cpp)
target_link_libraries(security_acl_test PRIVATE libtracer)
libtracer_test_warnings(security_acl_test)
# RFC-0026 (#993) — the typed-builder-vs-vector byte-compare needs the vector bytes.
target_compile_definitions(security_acl_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME security_acl COMMAND security_acl_test)
# Pins the shared acl/ace-duplicate-key #995 vector on parse_acl, so it needs
# LIBTRACER_VECTORS_DIR.
target_compile_definitions(security_acl_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")

# ADR-0050 — the effective-ACL merge semantics (effective_acl_t, pure, synthetic ACE
# lists) + the graph's cached merge: subtree-precise invalidation (ADR-0057 child
# links) and the ADR-0078 stamp protocol under concurrent :acl rewrites (a TSan target).
add_executable(effective_acl_test effective_acl_test.cpp)
target_link_libraries(effective_acl_test PRIVATE libtracer)
libtracer_test_warnings(effective_acl_test)
add_test(NAME effective_acl COMMAND effective_acl_test)

# ADR-0078 (#880) — the ACL cache's published-generation stamp under a LOCK-FREE subtree
# invalidation: an ancestor :acl rewriter racing a descendant's gated eval. A TSan target.
add_executable(acl_cache_race_test acl_cache_race_test.cpp)
target_link_libraries(acl_cache_race_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(acl_cache_race_test)
add_test(NAME acl_cache_race COMMAND acl_cache_race_test)

# #895 — RFC-0008 sweep-set exclusion under a concurrent `set_delivery_mode`: the flip must
# not leave one vertex in BOTH `pending_` and `unconditional_` (a sweep would deliver it
# twice). Also a TSan target — the marker's lock-free `delivery_mode()` read against the
# flipper's store is what made the plain byte a data race.
add_executable(delivery_mode_race_test delivery_mode_race_test.cpp)
target_link_libraries(delivery_mode_race_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(delivery_mode_race_test)
add_test(NAME delivery_mode_race COMMAND delivery_mode_race_test)

if(LIBTRACER_NET_PLANE)
    # #81-A — per-endpoint :acl structural storage (store/serve raw ACL TLV; no enforcement).
    add_executable(acl_test acl_test.cpp)
    target_link_libraries(acl_test PRIVATE libtracer)
    libtracer_test_warnings(acl_test)
    add_test(NAME acl COMMAND acl_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #407 — graph_t::retire() vertex retirement (RFC-0009 §B/§C/§E.6).
    add_executable(mount_routing_test mount_routing_test.cpp)
    target_link_libraries(mount_routing_test PRIVATE libtracer)
    libtracer_test_warnings(mount_routing_test)
    add_test(NAME mount_routing COMMAND mount_routing_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #882 — a reconnect rebind that FLIPS a mount's shape (point-to-point <-> bus) must never
    # let a forward pair one publication's shape with another's link: that routes a bus link
    # point-to-point and its send() BROADCASTS (the #409 one-request/N-replies misroute).
    add_executable(mount_shape_flip_test mount_shape_flip_test.cpp)
    target_link_libraries(mount_shape_flip_test PRIVATE libtracer Threads::Threads)
    libtracer_test_warnings(mount_shape_flip_test)
    add_test(NAME mount_shape_flip COMMAND mount_shape_flip_test)
    set_tests_properties(mount_shape_flip PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
endif()

if(LIBTRACER_NET_PLANE)
    # `add_child` under a REFUSING allocator (#523): the registry chunk is the one soft-failing
    # allocation on the control path, and it is driven by replacing the global NOTHROW
    # `operator new` — a whole-program decision, hence its own binary.
    add_executable(mount_add_oom_test mount_add_oom_test.cpp)
    target_link_libraries(mount_add_oom_test PRIVATE libtracer)
    libtracer_test_warnings(mount_add_oom_test)
    add_test(NAME mount_add_oom COMMAND mount_add_oom_test)
endif()

if(LIBTRACER_NET_PLANE)
    # `make_connection` under the same REFUSING allocator (#930): a connection whose link cannot
    # be wired into the router is rolled back rather than published as a live-looking dead
    # connection. Its own binary for the same whole-program reason as mount_add_oom_test.
    add_executable(conn_add_oom_test conn_add_oom_test.cpp)
    target_link_libraries(conn_add_oom_test PRIVATE libtracer)
    libtracer_test_warnings(conn_add_oom_test)
    add_test(NAME conn_add_oom COMMAND conn_add_oom_test)
endif()

if(LIBTRACER_NET_PLANE)
    add_executable(mount_width_test mount_width_test.cpp)
    target_link_libraries(mount_width_test PRIVATE libtracer)
    libtracer_test_warnings(mount_width_test)
    add_test(NAME mount_width COMMAND mount_width_test)
endif()

if(LIBTRACER_NET_PLANE)
    add_executable(compact_cache_test compact_cache_test.cpp)
    target_link_libraries(compact_cache_test PRIVATE libtracer)
    libtracer_test_warnings(compact_cache_test)
    add_test(NAME compact_cache COMMAND compact_cache_test)
endif()

if(LIBTRACER_NET_PLANE)
    # ADR-0063 §3: control-plane create/remove churn against a live forward reader. Its value is
    # under `-fsanitize=thread` (the tsan CI lane runs the whole suite), where it reports the
    # writer-vs-writer and rebind-vs-reader races; without TSan it still catches a lost or
    # duplicated child. RUN_SERIAL so a loaded CI machine does not starve its four threads.
    add_executable(net_control_plane_race_test net_control_plane_race_test.cpp)
    target_link_libraries(net_control_plane_race_test PRIVATE libtracer Threads::Threads)
    libtracer_test_warnings(net_control_plane_race_test)
    add_test(NAME net_control_plane_race COMMAND net_control_plane_race_test)
    set_tests_properties(net_control_plane_race PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)
endif()

# The published edge array + its edge pin (#635): a snapshot sees the old or the new edge
# set and never a torn mixture, an unsubscribe stops delivering at once, the pin is never
# held across a dispatch, and the retire list is flushed at teardown. Read by the TSan leg
# (the announce/validate protocol) and the ASan/LSan leg (nothing freed under a reader,
# nothing parked forever). RUN_SERIAL: it saturates every core on purpose.
add_executable(edge_publish_test edge_publish_test.cpp)
target_link_libraries(edge_publish_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(edge_publish_test)
add_test(NAME edge_publish COMMAND edge_publish_test)
set_tests_properties(edge_publish PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)

# #1442 — the republish SHARES each pre-existing edge's cold half instead of deep-copying it.
# Its own binary because it REPLACES global operator new/delete to count allocations, which no
# other test in this file may inherit. The assertion is a shape (an admission's allocation
# count is flat in the edge count), so restoring the deep copy reddens it by ~3 allocations per
# pre-existing edge.
# #1505 — a HANDLER write allocates exactly one block fewer per write than a retaining one,
# at every link count. Its own binary because it owns a global `operator new` counter.
# #1532: the ownership-stamp identity primitive. Header-only and platform-branching, so the
# host arm is unit-tested here and the FreeRTOS arm is compiled by the esp-idf full_node gate.
add_executable(thread_id_test thread_id_test.cpp)
target_link_libraries(thread_id_test PRIVATE libtracer)
libtracer_test_warnings(thread_id_test)
add_test(NAME thread_id COMMAND thread_id_test)

add_executable(batch_compose_alloc_test batch_compose_alloc_test.cpp)
target_link_libraries(batch_compose_alloc_test PRIVATE libtracer)
libtracer_test_warnings(batch_compose_alloc_test)
add_test(NAME batch_compose_alloc COMMAND batch_compose_alloc_test)

add_executable(handler_write_alloc_test handler_write_alloc_test.cpp)
target_link_libraries(handler_write_alloc_test PRIVATE libtracer)
libtracer_test_warnings(handler_write_alloc_test)
add_test(NAME handler_write_alloc COMMAND handler_write_alloc_test)

add_executable(edge_cold_half_share_test edge_cold_half_share_test.cpp)
target_link_libraries(edge_cold_half_share_test PRIVATE libtracer)
libtracer_test_warnings(edge_cold_half_share_test)
add_test(NAME edge_cold_half_share COMMAND edge_cold_half_share_test)

if(LIBTRACER_NET_PLANE)
    add_executable(registry_teardown_test registry_teardown_test.cpp)
    target_link_libraries(registry_teardown_test PRIVATE libtracer)
    libtracer_test_warnings(registry_teardown_test)
    add_test(NAME registry_teardown COMMAND registry_teardown_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #884 — the router's OWN per-child table under create/remove churn: the twin of the
    # registry teardown contract above, one layer out. Every pointer involved stays live, so the
    # failures this pins are wrong-tenancy resolutions and unbounded growth, NOT memory errors —
    # which is why it is an ordinary host test and its ASan value is the report it does NOT make.
    add_executable(child_rx_churn_test child_rx_churn_test.cpp)
    target_link_libraries(child_rx_churn_test PRIVATE libtracer)
    libtracer_test_warnings(child_rx_churn_test)
    add_test(NAME child_rx_churn COMMAND child_rx_churn_test)
endif()

add_executable(retire_test retire_test.cpp)
target_link_libraries(retire_test PRIVATE libtracer)
libtracer_test_warnings(retire_test)
add_test(NAME retire COMMAND retire_test)

if(LIBTRACER_NET_PLANE)
    # #576 — graph_t::collect() / parked_seam_count(): the explicit end of retirement's
    # value-seam park. The load-bearing case is that the free runs OUTSIDE every graph lock
    # (a seam destructor that re-enters the graph must not deadlock), so this test carries a
    # watchdog and reports rather than hanging. It also pins the two contract properties an
    # embedder acts on: the park is keyed on handler PRESENCE (not role_t), and only a BUS
    # link's connection teardown parks anything (point-to-point parks zero).
    add_executable(collect_test collect_test.cpp)
    target_link_libraries(collect_test PRIVATE libtracer Threads::Threads)
    libtracer_test_warnings(collect_test)
    add_test(NAME collect COMMAND collect_test)
endif()

# #82 — in-band :children[] SPEC vertex creation (ADR-0017). Reads the shared `spec/`
# conformance vectors (#877), so it needs LIBTRACER_VECTORS_DIR.
add_executable(children_test children_test.cpp)
target_link_libraries(children_test PRIVATE libtracer)
libtracer_test_warnings(children_test)
target_compile_definitions(children_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
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

# RFC-0025 §4.1.2 (Amendment 3, clause 5) — propagate's FOLD emission mode: ONE
# RFC-0005 §B branch-write frame per swept subtree instead of RFC-0008 §D's one
# FWD{WRITE} per selected vertex. Asserts the default does not move, the fold-vs-
# default per-leaf differential is byte-identical, the root carries its leading
# NAME, trailer-carrying nodes are rejected (not stripped), a selected STREAM is
# refused, and disjoint subtrees are SEVERAL frames (the retired-LIST ban).
add_executable(propagate_fold_test propagate_fold_test.cpp)
target_link_libraries(propagate_fold_test PRIVATE libtracer)
# Pins stream/fold-carries-composed-batch byte-exactly against the emitted frame (#1468), so it
# needs LIBTRACER_VECTORS_DIR.
target_compile_definitions(propagate_fold_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
libtracer_test_warnings(propagate_fold_test)
add_test(NAME propagate_fold COMMAND propagate_fold_test)

# ADR-0041 — terminus arena decoder: decode<->decode_into equivalence over every
# conformance vector + the span/canonical/zero-spill contract.
add_executable(tlv_arena_test tlv_arena_test.cpp)
target_link_libraries(tlv_arena_test PRIVATE libtracer)
libtracer_test_warnings(tlv_arena_test)
target_compile_definitions(tlv_arena_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME tlv_arena COMMAND tlv_arena_test)

if(LIBTRACER_NET_PLANE)
    # Brick 4 (ADR-0038 §3 / ADR-0039) — per-connection pmr label tables incl. a
    # zero-global-heap lifecycle over a slab resource with a null upstream.
    add_executable(route_handle_test route_handle_test.cpp)
    target_link_libraries(route_handle_test PRIVATE libtracer)
    libtracer_test_warnings(route_handle_test)
    add_test(NAME route_handle COMMAND route_handle_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #739 — subscribe_toward: a local producer bound toward a mount-path target through
    # the shared strip-K descent (no caller-side link/route hand-split).
    add_executable(subscribe_toward_test subscribe_toward_test.cpp)
    target_link_libraries(subscribe_toward_test PRIVATE libtracer)
    libtracer_test_warnings(subscribe_toward_test)
    add_test(NAME subscribe_toward COMMAND subscribe_toward_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #684 — the rebind-vs-forward race on the published mount run (a TSan vehicle that is
    # also a functional misroute detector on every build).
    add_executable(registry_rebind_race_test registry_rebind_race_test.cpp)
    target_link_libraries(registry_rebind_race_test PRIVATE libtracer)
    libtracer_test_warnings(registry_rebind_race_test)
    add_test(NAME registry_rebind_race COMMAND registry_rebind_race_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #914 — the router's five sink pairs are published as a unit: a setter racing the frame
    # path can no longer hand a new `fn` the previous sink's `ctx` (TSan vehicle + a functional
    # wrong-context detector on every build).
    add_executable(fwd_sink_race_test fwd_sink_race_test.cpp)
    target_link_libraries(fwd_sink_race_test PRIVATE libtracer Threads::Threads)
    libtracer_test_warnings(fwd_sink_race_test)
    add_test(NAME fwd_sink_race COMMAND fwd_sink_race_test)
endif()

# #1049 — the L4 analogue: graph_t's five configuration seams under a setter storm. The
# three callback seams carry the same wrong-context detector as #914's; the child-type
# catalog and the identity record (served ABOVE the READ gate, to an unauthenticated peer)
# assert their legal-outcome sets instead, and are TSan/ASan vehicles.
add_executable(graph_config_race_test graph_config_race_test.cpp)
target_link_libraries(graph_config_race_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(graph_config_race_test)
add_test(NAME graph_config_race COMMAND graph_config_race_test)

if(LIBTRACER_NET_PLANE)
    # #419 — the two-mount route: one dst crossing TWO net/<module>/<name> mounts, driven
    # through three in-process routers chained by loopback link pairs. The behavioural binding
    # for the fwd/fwd-routed-two-mount conformance vector (HARNESS.md: a vector gates the codec
    # only), and the first FWD test in the tree with more than one forwarder.
    add_executable(fwd_two_mount_test fwd_two_mount_test.cpp)
    target_link_libraries(fwd_two_mount_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_two_mount_test)
    add_test(NAME fwd_two_mount COMMAND fwd_two_mount_test)
endif()

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

# #652 — the leaf/branch fork of graph_t::read, now answered from a bit on the vertex
# instead of a map-lock-guarded child walk. Setting the bit was already covered; CLEARING
# it was not (nothing in the suite retired a vertex and then read its parent), and that
# transition is the whole risk: a stuck bit sends a leaf read down the composed path or
# drops every descendant from a branch read. Ends on a reader/registrar race for TSan.
add_executable(read_fork_test read_fork_test.cpp)
target_link_libraries(read_fork_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(read_fork_test)
add_test(NAME read_fork COMMAND read_fork_test)

# ADR-0069 — both LKV slot policies, named explicitly rather than through the
# `lkv_slot_t` alias, so ONE leg covers the binding this build did not choose. The
# hazard slot is lock-free reclamation, so the legs that matter are the sanitizer
# ones: contract, ownership across a replace, reclamation, concurrent read/write,
# the parked-set bound ADR-0069 §2 chose hazard for, and the exhausted registry.
add_executable(lkv_slot_test lkv_slot_test.cpp)
target_link_libraries(lkv_slot_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(lkv_slot_test)
add_test(NAME lkv_slot COMMAND lkv_slot_test)

# #361 §5 / ADR-0039 §1 — graph_t's injected memory_resource: per-write LKV
# allocations route through it (counting pass-through), released in full at
# graph destruction; a default graph never touches a foreign resource.
add_executable(graph_pmr_test graph_pmr_test.cpp)
target_link_libraries(graph_pmr_test PRIVATE libtracer)
libtracer_test_warnings(graph_pmr_test)
add_test(NAME graph_pmr COMMAND graph_pmr_test)

add_executable(graph_value_backend_test graph_value_backend_test.cpp)
target_link_libraries(graph_value_backend_test PRIVATE libtracer)
libtracer_test_warnings(graph_value_backend_test)
add_test(NAME graph_value_backend COMMAND graph_value_backend_test)

# #831 — the composed-root folded READ's per-node POINT headers draw from the ADR-0060 value
# seam, not view::heap_alloc's global heap; exhaustion answered by value.
add_executable(folded_read_backend_test folded_read_backend_test.cpp)
target_link_libraries(folded_read_backend_test PRIVATE libtracer)
libtracer_test_warnings(folded_read_backend_test)
add_test(NAME folded_read_backend COMMAND folded_read_backend_test)

if(LIBTRACER_NET_PLANE)
    # #981 — the two try_* sites that migrated off the -fno-exceptions probe window
    # (read_subtree_folded's collect stack, deliver_remote's egress iov table) really draw from
    # the injected block_source_t, and answer its exhaustion by value. The injector refuses ONE
    # exact block shape so the refusal is attributable, and every armed case asserts a positive
    # "the seam was asked" instrument first — revert either migration and both halves redden.
    # (Links fwd_router_t::deliver_remote, so the target needs the net plane.)
    add_executable(try_probe_window_test try_probe_window_test.cpp)
    target_link_libraries(try_probe_window_test PRIVATE libtracer)
    libtracer_test_warnings(try_probe_window_test)
    add_test(NAME try_probe_window COMMAND try_probe_window_test)

    # #477 — the store/delivery path's nothrow soft-fail discipline: OOM injected through
    # tr::detail::probe_fail_hook (store => BACKPRESSURE, delivery => drop/defer, never abort).
    add_executable(graph_oom_softfail_test graph_oom_softfail_test.cpp)
    target_link_libraries(graph_oom_softfail_test PRIVATE libtracer)
    libtracer_test_warnings(graph_oom_softfail_test)
    add_test(NAME graph_oom_softfail COMMAND graph_oom_softfail_test)
endif()

# ADR-0060 §2 thread-safe pool value_backend: concurrent alloc + cross-thread reclaim
# (the tsan job in core-ci.yml builds this with -fsanitize=thread and runs it).
add_executable(mem_sync_pool_test mem_sync_pool_test.cpp)
target_link_libraries(mem_sync_pool_test PRIVATE libtracer)
libtracer_test_warnings(mem_sync_pool_test)
add_test(NAME mem_sync_pool COMMAND mem_sync_pool_test)

# #770 — the synchronisation POLICY seam of mem::synchronized_pool_t: the injected
# critical section is taken on every alloc/destroy, no slot is handed to two live
# segments, and the free list survives the storm. Sanitizer-independent (it counts and
# compares), so it fires in the ordinary ctest run as well as under the tsan job.
#
# THE ABLATION, run by hand (deliberately not an add_test — "the race manifests" is a
# probabilistic pass condition, and a flaky green is worse than none):
#   g++ -std=c++23 -O2 -pthread -DLIBTRACER_ABLATE_POOL_SYNC -I core/include \
#       core/tests/mem_sync_policy_test.cpp core/src/mem_pool.cpp core/src/mem_heap.cpp \
#       core/src/backend_set.cpp core/src/mem_source.cpp -o ablate
# (Only these TUs — core/src/*.cpp would drag in the msquic-dependent transports.)
# The ablated build dies on the corruption (typically SIGSEGV in the mangled free list
# before the counters even print); the unablated build reports OK.
add_executable(mem_sync_policy_test mem_sync_policy_test.cpp)
target_link_libraries(mem_sync_policy_test PRIVATE libtracer)
libtracer_test_warnings(mem_sync_policy_test)
add_test(NAME mem_sync_policy COMMAND mem_sync_policy_test)

# RFC-0008 Amendment 2 (#1506) — the non-retaining-vertex contract: `await` serves the
# role-dispatched read, and `assign` / `propagate` refuse by value at a vertex that retains
# no last-known-value. Five vectors, each with a positive control.
add_executable(nonretaining_contract_test nonretaining_contract_test.cpp)
target_link_libraries(nonretaining_contract_test PRIVATE libtracer)
libtracer_test_warnings(nonretaining_contract_test)
add_test(NAME nonretaining_contract COMMAND nonretaining_contract_test)

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

# #1503 step 5 / RFC-0010 Amendment 1 — the node-scoped `:stats` seam census: one READ is
# one seam is the whole counter block in ONE SETTINGS TLV, node-scoped like `:identity` but
# with the gating INVERTED (the VALUE sits BELOW the READ gate; only NAME validity resolves
# above it), read-only, and never awaitable.
add_executable(stats_field_test stats_field_test.cpp)
target_link_libraries(stats_field_test PRIVATE libtracer)
libtracer_test_warnings(stats_field_test)
add_test(NAME stats_field COMMAND stats_field_test)

add_executable(app_fields_test app_fields_test.cpp)
target_link_libraries(app_fields_test PRIVATE libtracer)
libtracer_test_warnings(app_fields_test)
add_test(NAME app_fields COMMAND app_fields_test)

# #869 — the field surface's READ/WRITE dispatch parity. The full field x selector matrix
# through both doors (the characterization net behind the shared shape classifiers), the
# three-door subscriber admission parity, and the pinned read/write divergences
# (:subscribers[*], :acl[0], :settings[0].app.<name>) that need an RFC to close.
add_executable(field_dispatch_test field_dispatch_test.cpp)
target_link_libraries(field_dispatch_test PRIVATE libtracer)
libtracer_test_warnings(field_dispatch_test)
add_test(NAME field_dispatch COMMAND field_dispatch_test)

if(LIBTRACER_NET_PLANE)
    # RFC-0022 (as amended) — the per-subscription delivery policy, the removed vertex knobs,
    # and §3.F: the two surviving storage declarations are owner-side and inherited by NOBODY.
    add_executable(qos_policy_test qos_policy_test.cpp)
    target_link_libraries(qos_policy_test PRIVATE libtracer)
    libtracer_test_warnings(qos_policy_test)
    target_compile_definitions(qos_policy_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    add_test(NAME qos_policy COMMAND qos_policy_test)
endif()

if(LIBTRACER_NET_PLANE)
    # RFC-0004 / ADR-0035 slice 2 — local FWD operation resolution + zero-copy FWD{REPLY}.
    add_executable(op_resolve_test op_resolve_test.cpp)
    target_link_libraries(op_resolve_test PRIVATE libtracer)
    libtracer_test_warnings(op_resolve_test)
    target_compile_definitions(op_resolve_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    add_test(NAME op_resolve COMMAND op_resolve_test)
endif()

if(LIBTRACER_NET_PLANE)
    # ADR-0053 §7 (3c-ii) — differential oracle: the owning rope-tier resolver
    # (resolve(tlv_view_t)) must produce byte-identical replies to the span-tier
    # arena resolver for the same request under every rope fragmentation.
    add_executable(op_resolve_view_test op_resolve_view_test.cpp)
    target_link_libraries(op_resolve_view_test PRIVATE libtracer)
    libtracer_test_warnings(op_resolve_view_test)
    add_test(NAME op_resolve_view COMMAND op_resolve_view_test)
endif()

if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_WS)
    # RFC-0004 / ADR-0035 slice 3 — multi-hop FWD forwarding + zero-copy src
    # accumulation over LIVE transport_ws (client -> A forwarder -> B terminus).
    add_executable(fwd_multihop_test fwd_multihop_test.cpp)
    target_link_libraries(fwd_multihop_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_multihop_test)
    # RFC-0025 §4.2.1 clause 4: it binds `stream/tf1-reserved`'s relay arm — a TF=1 root
    # crossing live hops verbatim — so it needs LIBTRACER_VECTORS_DIR.
    target_compile_definitions(fwd_multihop_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    add_test(NAME fwd_multihop COMMAND fwd_multihop_test)
endif()

if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_WS)
    # #491 / RFC-0021 — third-party orchestration end-to-end over LIVE production wiring
    # (three full nodes, config-constructed ws sockets): orchestrator C creates B's link
    # toward A through the creator surface, writes the subscription with the offline-composed
    # B-rooted target, departs; delivery B -> A must continue.
    add_executable(orchestrate_depart_test orchestrate_depart_test.cpp)
    target_link_libraries(orchestrate_depart_test PRIVATE libtracer)
    libtracer_test_warnings(orchestrate_depart_test)
    add_test(NAME orchestrate_depart COMMAND orchestrate_depart_test)

    # RFC-0004 / ADR-0035 slice 4 — the route-handle: ws delivery-compaction over LIVE
    # transport_ws (producer B -> A forwarder -> consumer C). Proves the byte-delta of a
    # label-compacted COMPACT vs a full-route FWD, byte-exact ordered delivery, the
    # stale-label drop + re-advertise self-heal, and that a non-compact flow stays stateless.
    add_executable(fwd_compact_test fwd_compact_test.cpp)
    target_link_libraries(fwd_compact_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_compact_test)
    add_test(NAME fwd_compact COMMAND fwd_compact_test)
endif()

if(LIBTRACER_NET_PLANE)
    # RFC-0004 §E.1/§F (#974) — a COMPACT delivery is ACL-gated under the INBOUND LINK's name,
    # like the full-route FWD{WRITE} it compacts. Both terminus write arms of on_compact (the
    # cold deliver_local one and the memoized warm one) are proven separately, each paired with
    # a positive control, plus the no-resolver default where nothing is enforced.
    add_executable(fwd_compact_acl_test fwd_compact_acl_test.cpp)
    target_link_libraries(fwd_compact_acl_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_compact_acl_test)
    add_test(NAME fwd_compact_acl COMMAND fwd_compact_acl_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #375 — a HANDLER's on_write receives the writer's SUBJECT (tr::graph::write_ctx_t). Pins the
    # empty-token OWNER sentinel on both local doors, two links reaching one handler as two
    # distinct subjects, TWO PEERS ON ONE LINK staying distinguishable (the collapse #375
    # reported), and a delivered subscription presenting the EDGE's fan-in context.
    add_executable(write_ctx_test write_ctx_test.cpp)
    target_link_libraries(write_ctx_test PRIVATE libtracer)
    libtracer_test_warnings(write_ctx_test)
    add_test(NAME write_ctx COMMAND write_ctx_test)
endif()

if(LIBTRACER_NET_PLANE)
    # RFC-0004 §E.1 (#716) — the MID-CHAIN reconnect self-heal: clear_link sweeps every ingress
    # binding, on ANY link, whose downstream half crossed the cleared link, so an origin that never
    # saw the reconnect stops streaming onto a dead out-label. Asserts RECOVERY end to end over a
    # 3-node in-process chain, not merely the NACK.
    add_executable(fwd_reconnect_selfheal_test fwd_reconnect_selfheal_test.cpp)
    target_link_libraries(fwd_reconnect_selfheal_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_reconnect_selfheal_test)
    add_test(NAME fwd_reconnect_selfheal COMMAND fwd_reconnect_selfheal_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #913 — a re-advertise cycle grows NOTHING: both ADVERTISE-emitting sites (the producer door
    # and the mid-chain forwarding swap) reuse the label already bound to an identical route, so a
    # reconnect loop cannot walk a link's 16-bit label space or its egress table. Counts distinct
    # labels and table entries across 51 cycles at every node of a two-hop chain.
    add_executable(fwd_readvertise_reuse_test fwd_readvertise_reuse_test.cpp)
    target_link_libraries(fwd_readvertise_reuse_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_readvertise_reuse_test)
    add_test(NAME fwd_readvertise_reuse COMMAND fwd_readvertise_reuse_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #833 — a REFUSED forwarding bind strands neither the out-label it took nor the egress route
    # retained with it. Drives the ingress table to its injected bound so the BIND is the step that
    # refuses, then asserts the downstream table returns to its pre-advertise census, the label
    # itself comes back, nothing went on the wire, and the shared entry an established flow rides
    # is left alone.
    add_executable(fwd_bind_refusal_unwind_test fwd_bind_refusal_unwind_test.cpp)
    target_link_libraries(fwd_bind_refusal_unwind_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_bind_refusal_unwind_test)
    add_test(NAME fwd_bind_refusal_unwind COMMAND fwd_bind_refusal_unwind_test)
endif()

if(LIBTRACER_NET_PLANE)
    # RFC-0004 / ADR-0035 slice 4 (#136) — the PRODUCER auto fan-out: a plain graph.write to a
    # vertex with a remote subscriber delivers a full-route FWD{WRITE} (or auto-promoted COMPACT)
    # with no explicit advertise/send. Covers the transient-local latch, the compact byte-delta,
    # reconnect self-heal, and a concurrent writer x clear_link race (the TSan gate).
    add_executable(fwd_fanout_test fwd_fanout_test.cpp)
    target_link_libraries(fwd_fanout_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_fanout_test)
    add_test(NAME fwd_fanout COMMAND fwd_fanout_test)
endif()

# RFC-0009 §D extended to peer departure — subscriber-edge eviction on link teardown:
# evict_link_edges scoping + RFC-0005 counter unwind, §D.2 slot reuse/index stability,
# fwd_router link_down (evict + clear_link), the add_child departure-notifier seam
# (p2p down / bus peer-down), and a concurrent writer x evict race (the TSan gate).
# Per-cause delivery-drop counters: a path-target edge whose delivery cannot happen drops
# it (specified) and now says so (graph_t::delivery_drops).
add_executable(delivery_drops_test delivery_drops_test.cpp)
target_link_libraries(delivery_drops_test PRIVATE libtracer)
libtracer_test_warnings(delivery_drops_test)
add_test(NAME delivery_drops COMMAND delivery_drops_test)

if(LIBTRACER_NET_PLANE)
    add_executable(edge_eviction_test edge_eviction_test.cpp)
    target_link_libraries(edge_eviction_test PRIVATE libtracer)
    libtracer_test_warnings(edge_eviction_test)
    add_test(NAME edge_eviction COMMAND edge_eviction_test)

    # #1266 / #1417 — the CARRIED link token. The parts of the carry whose failure mode is a
    # leaked subscriber edge rather than a slow one: node-scoped identity, same-NAME redial,
    # slot reuse, a wrong token refused, the seam's laziness, and the census-bus lazy mint.
    # Deliberately its own file: edge_eviction_test.cpp stays UNMODIFIED by the carry, and
    # that it does is half the evidence that the name doors did not move.
    add_executable(link_token_carry_test link_token_carry_test.cpp)
    target_link_libraries(link_token_carry_test PRIVATE libtracer)
    libtracer_test_warnings(link_token_carry_test)
    add_test(NAME link_token_carry COMMAND link_token_carry_test)
endif()

if(LIBTRACER_NET_PLANE)
    # ADR-0053 ④b — the FWD forward hop over a MULTI-LINK rope, without flattening: an oracle
    # equality that every adversarial split routes byte-identically to the contiguous path,
    # plus the terminus flatten fallback.
    add_executable(fwd_terminus_reject_test fwd_terminus_reject_test.cpp)
    target_link_libraries(fwd_terminus_reject_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_terminus_reject_test)
    add_test(NAME fwd_terminus_reject COMMAND fwd_terminus_reject_test)
endif()

if(LIBTRACER_NET_PLANE)
    add_executable(fwd_malformed_control_test fwd_malformed_control_test.cpp)
    target_link_libraries(fwd_malformed_control_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_malformed_control_test)
    add_test(NAME fwd_malformed_control COMMAND fwd_malformed_control_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #730 — the router's flatten seam: every materialize() draws from the injected
    # mem_backend_t, and a refused flatten is answered by value (never stored empty).
    add_executable(fwd_flatten_backend_test fwd_flatten_backend_test.cpp)
    target_link_libraries(fwd_flatten_backend_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_flatten_backend_test)
    add_test(NAME fwd_flatten_backend COMMAND fwd_flatten_backend_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #1503 step 3 — the router's four seam accessors and its COUNTED cold drops: every
    # per-cause counter moves only on its own failure arm, and never on a success.
    add_executable(router_drop_stats_test router_drop_stats_test.cpp)
    target_link_libraries(router_drop_stats_test PRIVATE libtracer)
    libtracer_test_warnings(router_drop_stats_test)
    add_test(NAME router_drop_stats COMMAND router_drop_stats_test)
endif()

if(LIBTRACER_NET_PLANE)
    # RFC-0010 Amendment 2 (#1503 residual) — the NET-PLANE `:stats` seams: the router
    # registers a sampler UP into the graph, so `router` / `labels` / `link.<child>` are
    # wire-readable; with no router every one of them stays SCHEMA_NOT_FOUND.
    add_executable(stats_net_seam_test stats_net_seam_test.cpp)
    target_link_libraries(stats_net_seam_test PRIVATE libtracer)
    libtracer_test_warnings(stats_net_seam_test)
    add_test(NAME stats_net_seam COMMAND stats_net_seam_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #766 — the TERMINUS half of that seam: the rope-tier flattens one call below
    # resolve_terminus_rope draw from the same injection, and a refusal is answered by
    # value (drop, or an addressed BACKPRESSURE) instead of read as a short span.
    add_executable(terminus_flatten_backend_test terminus_flatten_backend_test.cpp)
    target_link_libraries(terminus_flatten_backend_test PRIVATE libtracer)
    libtracer_test_warnings(terminus_flatten_backend_test)
    add_test(NAME terminus_flatten_backend COMMAND terminus_flatten_backend_test)
endif()

if(LIBTRACER_NET_PLANE)
    # #795 — the terminus reply's EGRESS-construction seam (ADR-0074): the FWD{REPLY}
    # head and the RFC-0024 mint draw from the injected `egress` backend, not the global
    # heap, and a refusal degrades by value (drop, or an addressed BACKPRESSURE).
    add_executable(terminus_egress_backend_test terminus_egress_backend_test.cpp)
    target_link_libraries(terminus_egress_backend_test PRIVATE libtracer)
    libtracer_test_warnings(terminus_egress_backend_test)
    add_test(NAME terminus_egress_backend COMMAND terminus_egress_backend_test)
endif()

if(LIBTRACER_NET_PLANE)
    add_executable(fwd_rope_forward_test fwd_rope_forward_test.cpp)
    target_link_libraries(fwd_rope_forward_test PRIVATE libtracer)
    libtracer_test_warnings(fwd_rope_forward_test)
    add_test(NAME fwd_rope_forward COMMAND fwd_rope_forward_test)
endif()

if(LIBTRACER_NET_PLANE)
    # RFC-0014 §4 S5 (#492) — the link-liveness engine over a scripted transport kind:
    # dormant creation, demand dial, self-heal, refcount gating. Threaded by design;
    # runs under the sanitizer set like the other net-plane tests.
    add_executable(link_liveness_test link_liveness_test.cpp)
    target_link_libraries(link_liveness_test PRIVATE libtracer)
    libtracer_test_warnings(link_liveness_test)
    # RFC-0014 S7-B: it binds the `conn/liveness-enum` vector to the live connection
    # vertex, so it needs LIBTRACER_VECTORS_DIR.
    target_compile_definitions(link_liveness_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    add_test(NAME link_liveness COMMAND link_liveness_test)

    # RFC-0014 S6 (#492) — the control-plane lock-order invariant: `ctl_m_` is never held
    # across a call that can re-enter transport_vertex_t (a subscriber fan-out, an engine
    # or socket thread join). The regressions are DEADLOCKS, so each exercise runs on its
    # own thread behind a watchdog and reports [FAIL] rather than hanging out to the ctest
    # timeout; the TIMEOUT below is only the backstop for the backstop.
    add_executable(net_lock_order_test net_lock_order_test.cpp)
    target_link_libraries(net_lock_order_test PRIVATE libtracer)
    libtracer_test_warnings(net_lock_order_test)
    add_test(NAME net_lock_order COMMAND net_lock_order_test)
    set_tests_properties(net_lock_order PROPERTIES TIMEOUT 180)
endif()

if(LIBTRACER_NET_PLANE)
    # #492 S2c — RFC-0014 Amendment 2 (§5.1): the payload-type => required-ACL-right table a
    # handler vertex declares, and §5 discharged on the creator endpoint (SPEC=>CREATE,
    # NAME=>WRITE). Also pins Amendment 3's empty-SETTINGS catalog envelope.
    add_executable(payload_right_table_test payload_right_table_test.cpp)
    target_link_libraries(payload_right_table_test PRIVATE libtracer)
    libtracer_test_warnings(payload_right_table_test)
    add_test(NAME payload_right_table COMMAND payload_right_table_test)
endif()

if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_UDP AND LIBTRACER_TRANSPORT_WS)
    # #83 Stage-1 — transport/connection as a / vertex (shell over the live fwd path).
    add_executable(transport_vertex_test transport_vertex_test.cpp)
    target_link_libraries(transport_vertex_test PRIVATE libtracer)
    libtracer_test_warnings(transport_vertex_test)
    # RFC-0014 S7-A: it binds the seven `conn/` control-plane vectors to the running
    # creator endpoint, so it needs LIBTRACER_VECTORS_DIR.
    target_compile_definitions(transport_vertex_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    add_test(NAME transport_vertex COMMAND transport_vertex_test)
endif()


if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_UDP)
    # M5 — UDP transport harness (raw frame + two-node end-to-end over localhost UDP).
    add_executable(udp_test udp_test.cpp)
    target_link_libraries(udp_test PRIVATE libtracer)
    libtracer_test_warnings(udp_test)
    target_compile_definitions(udp_test PRIVATE
        LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
    add_test(NAME udp COMMAND udp_test)
endif()

if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_TCP)
    # M6 — TCP stream transport harness (length-prefix framing: split/coalesced writes,
    # oversize-prefix rejection, ADR-0042 owning delivery + backpressure drain, two-node
    # FWD end-to-end, and a config-created `kind=tcp` connection).
    add_executable(tcp_test tcp_test.cpp)
    target_link_libraries(tcp_test PRIVATE libtracer)
    libtracer_test_warnings(tcp_test)
    add_test(NAME tcp COMMAND tcp_test)
endif()

if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_TCP)
    # #1294 — the peer-receiver seam carries an opaque per-peer HANDLE, not a name string:
    # the POD contract (8 B, reserved-zero generation, no null arm), `peer_name` as the one
    # bridge back to the routing plane's name, the minted-handle round trip through delivery,
    # and the generation bump that keeps a recycled slot from inheriting a session identity.
    add_executable(peer_handle_seam_test peer_handle_seam_test.cpp)
    target_link_libraries(peer_handle_seam_test PRIVATE libtracer)
    libtracer_test_warnings(peer_handle_seam_test)
    add_test(NAME peer_handle_seam COMMAND peer_handle_seam_test)
endif()

if(LIBTRACER_TRANSPORT_TCP)
    # #838 — the host twin of #835: a stream send into a peer that stopped reading is BOUNDED
    # (the write mutex is held across it, so an unbounded one froze every sender on the link),
    # the shed frame is counted, and a peer that keeps stalling — or half-takes one record — is
    # closed with attribution instead of blocked on forever. The helper-level arms run over a
    # socketpair whose buffers are full; the last arm drives a real tcp_transport_t against a
    # peer that never reads, which HANGS on the pre-fix code.
    add_executable(send_stall_test send_stall_test.cpp)
    target_link_libraries(send_stall_test PRIVATE libtracer)
    libtracer_test_warnings(send_stall_test)
    add_test(NAME send_stall COMMAND send_stall_test)
    set_tests_properties(send_stall PROPERTIES TIMEOUT 60)
endif()

if(LIBTRACER_ANY_SOCKET_TRANSPORT)
    # #903 — the stream_endpoint_t full-write helpers under a REAL signal storm: a
    # socketpair with a small SO_SNDBUF, a deliberately slow reader, and SIGUSR1
    # (no SA_RESTART) fired at the writer thread, so a blocked send returns EINTR
    # mid-frame. Both write_all and write_all_iov must resume, not truncate.
    add_executable(write_all_eintr_test write_all_eintr_test.cpp)
    target_link_libraries(write_all_eintr_test PRIVATE libtracer)
    libtracer_test_warnings(write_all_eintr_test)
    add_test(NAME write_all_eintr COMMAND write_all_eintr_test)
endif()

if(LIBTRACER_ANY_SOCKET_TRANSPORT)
    # #948 — the write-fault classification: a malformed send/sendmsg call (EOPNOTSUPP,
    # EINVAL) is OUR defect, not a dead socket, so it must be re-attempted and counted
    # rather than swallowed as a disconnect. Driven through the
    # tr::detail::write_fault_inject_hook seam, since a host kernel emits none of those
    # errnos here — a real-socket-only test would classify nothing.
    add_executable(write_fault_class_test write_fault_class_test.cpp)
    target_link_libraries(write_fault_class_test PRIVATE libtracer)
    libtracer_test_warnings(write_fault_class_test)
    add_test(NAME write_fault_class COMMAND write_fault_class_test)
endif()

if(LIBTRACER_TRANSPORT_WS)
    # #54 — transport_ws server socket layer (TCP accept + RFC6455 handshake + transport_t).
    add_executable(ws_transport_test ws_transport_test.cpp)
    target_link_libraries(ws_transport_test PRIVATE libtracer)
    libtracer_test_warnings(ws_transport_test)
    add_test(NAME ws_transport COMMAND ws_transport_test)
endif()

if(LIBTRACER_TRANSPORT_WS)
    # #872 — the WS DATA path is bounded by the injected (backend, max_frame) seam: an over-cap
    # declared length is refused off the frame HEADER (measured against the process peak RSS, not
    # inferred), fragments cannot sum past the cap, and backend exhaustion is counted backpressure.
    add_executable(ws_rx_bound_test ws_rx_bound_test.cpp)
    target_link_libraries(ws_rx_bound_test PRIVATE libtracer)
    libtracer_test_warnings(ws_rx_bound_test)
    add_test(NAME ws_rx_bound COMMAND ws_rx_bound_test)
endif()

if(LIBTRACER_TRANSPORT_WS)
    # #934 car B — the WS OPENING HANDSHAKE is bounded by an injected PRE-AUTH budget: a
    # never-terminated Upgrade request is refused BEFORE it is buffered (measured against the
    # process peak RSS, not inferred), the refusal is counted and closes the link, the honored
    # budget is the configured tighten-only `max_handshake`, and a legal dribbled request still
    # completes — the budget is the whole request, not one read of it.
    add_executable(ws_handshake_bound_test ws_handshake_bound_test.cpp)
    target_link_libraries(ws_handshake_bound_test PRIVATE libtracer)
    libtracer_test_warnings(ws_handshake_bound_test)
    add_test(NAME ws_handshake_bound COMMAND ws_handshake_bound_test)
endif()

if(LIBTRACER_TRANSPORT_TCP AND LIBTRACER_TRANSPORT_UDP AND LIBTRACER_TRANSPORT_WS)
    # transport_t seam conformance — ONE parameterized contract (send/receive, byte-identity,
    # scatter-gather, bidirectional, multi-frame, ADR-0042 owning view) over every
    # point-to-point adapter: in-process loopback + real localhost udp / tcp / ws.
    add_executable(transport_conformance_test transport_conformance_test.cpp)
    target_link_libraries(transport_conformance_test PRIVATE libtracer)
    libtracer_test_warnings(transport_conformance_test)
    add_test(NAME transport_conformance COMMAND transport_conformance_test)
endif()

if(LIBTRACER_TRANSPORT_WS)
    # #54 (interop) — standalone transport_ws_server harness driven by the TypeScript
    # transport-ws interop test (a real TS TransportWs client over a live socket).
    # Deliberately NOT an add_test(): it is a helper binary the TS test spawns (see
    # bindings/typescript/packages/transport-ws/test/interop.test.mjs and the
    # ws-interop CI job), so it must not run as part of the C++ ctest suite.
    add_executable(ws_interop_server ws_interop_server.cpp)
    target_link_libraries(ws_interop_server PRIVATE libtracer)
    libtracer_test_warnings(ws_interop_server)
endif()

if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_WS)
    # #56 / RFC-0004 (interop) — standalone FWD-serving node harness driven by the
    # TypeScript client SDK interop test (the web-UI<->device consumer proof). A
    # graph_t seeded with /sensor/temp behind a fwd_router_t + op_resolver_t, served
    # over a live transport_ws_server. Like ws_interop_server it is a helper binary
    # the TS test spawns, NOT an add_test() (it would otherwise block on its deadline).
    add_executable(fwd_node_server fwd_node_server.cpp)
    target_link_libraries(fwd_node_server PRIVATE libtracer)
    libtracer_test_warnings(fwd_node_server)
endif()

if(LIBTRACER_NET_PLANE)
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
endif()

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
    # backend_set.cpp's multi-member `transfer` dispatches its DEVICE arm here (#1381).
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/device_backend.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mem_heap.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mem_pool.cpp
    # frame.cpp's decode spills its walk stack to mem::heap_source() (#588).
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mem_source.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/rope.cpp
)
target_include_directories(substrate_test_no_atomic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(substrate_test_no_atomic PRIVATE cxx_std_23)
libtracer_test_warnings(substrate_test_no_atomic)
target_compile_definitions(substrate_test_no_atomic PRIVATE
    LIBTRACER_NO_ATOMIC
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME substrate_no_atomic COMMAND substrate_test_no_atomic)

# #922 — the SINGLE-MEMBER module-set dispatch (LIBTRACER_BACKEND_SET_POOL_ONLY, the MCU
# profile). Nothing else in the suite compiles that fold, which is how its destroy leg
# shipped with an unconditional static_cast<pool_t*> while the `transfer` two lines below
# it checked the tag. Like substrate_test_no_atomic this compiles the (few) library
# sources it needs directly rather than linking libtracer.a — the library's own
# backend_set.o carries the MULTI-member dispatch, so linking both would be a duplicate
# definition of destroy_dispatch/transfer.
add_executable(pool_only_dispatch_test
    pool_only_dispatch_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/backend_set.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mem_pool.cpp
)
target_include_directories(pool_only_dispatch_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_compile_features(pool_only_dispatch_test PRIVATE cxx_std_23)
libtracer_test_warnings(pool_only_dispatch_test)
target_compile_definitions(pool_only_dispatch_test PRIVATE LIBTRACER_BACKEND_SET_POOL_ONLY)
add_test(NAME pool_only_dispatch COMMAND pool_only_dispatch_test)

# #923 — the nothrow try_* growth helpers must never let an allocation failure cross their
# `noexcept` boundary. This TU OWNS global operator new (it must refuse specific
# allocations, which `probe_fail_hook` cannot do — the hook gates the probe, not the
# vector's own reserve), so it links the library like any other consumer but its
# replacement operators bind process-wide. Threads: the race leg.
add_executable(try_grow_race_test try_grow_race_test.cpp)
target_link_libraries(try_grow_race_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(try_grow_race_test)
add_test(NAME try_grow_race COMMAND try_grow_race_test)

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

    # #1182 — the OUT-OF-PROCESS WebTransport peer. `probe_fail_hook` is process-wide,
    # so gating the transport's two peer-driven allocations means putting the peer in
    # another process; this binary is raw_wt_client.hpp behind a line protocol, and it
    # is spawned by webtransport_alloc_gate_test, NOT an add_test() of its own.
    add_executable(wt_peer_driver wt_peer_driver.cpp)
    target_link_libraries(wt_peer_driver PRIVATE libtracer_quic)
    libtracer_test_warnings(wt_peer_driver)

    add_executable(webtransport_alloc_gate_test webtransport_alloc_gate_test.cpp)
    target_link_libraries(webtransport_alloc_gate_test PRIVATE libtracer_quic)
    libtracer_test_warnings(webtransport_alloc_gate_test)
    add_dependencies(webtransport_alloc_gate_test wt_peer_driver)
    target_compile_definitions(webtransport_alloc_gate_test PRIVATE
        LIBTRACER_DEV_CERT_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/../../tools/gen-dev-cert.sh"
        LIBTRACER_WT_PEER_DRIVER="$<TARGET_FILE:wt_peer_driver>")
    add_test(NAME webtransport_alloc_gate COMMAND webtransport_alloc_gate_test)
    set_tests_properties(webtransport_alloc_gate PROPERTIES ENVIRONMENT
        "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tsan-msquic.supp")
endif()

# The GPU backend's test moved out with the backend itself (#1381): it is
# `backends/cuda/tests/cuda_test.cpp`, built and run by the tier's own CMake project
# (tools/test-cuda.sh), never in CI. What core keeps under test is the SEAM the tier
# plugs into — the device-backend registry properties in substrate_test, which need no GPU.

# #874 — the shared runner (test_support.hpp), under test by itself. Every other harness
# here reports its verdict THROUGH that header, so a runner that stopped counting would
# turn the whole suite green at once and no other target could see it. Registered last on
# purpose: this file's line numbers are cited by docs/modules/segment.md and pinned in
# tools/check_doc_citations.py, so new targets append rather than insert.
add_executable(test_support_test test_support_test.cpp)
target_link_libraries(test_support_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(test_support_test)
add_test(NAME test_support COMMAND test_support_test)

# #1158 — the spin-pool guard. Not an executable: what is under test is that a build is
# REJECTED, so the check (cmake/spin_pool_guard.cmake) drives the compiler over one probe TU
# under two configurations and asserts BOTH arms — the allowed one compiles, the forbidden one
# fails AND names critical_pool_t. The forbidden arm is an ORDINARY override fragment (ADR-0068
# §Erratum 1): three lines that inherit the defaults and flip the one knob, so it cannot drift
# from them the way a second full copy of config.hpp could.
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/spin-forbidden/libtracer/config_override.hpp
"/**
 * @file
 * @brief GENERATED test fixture — the spin-forbidden configuration for the #1158 guard.
 */
#pragma once

namespace tr::graph {

/** @brief The defaults with spin-waiting refused, as an RTOS target declares it. */
struct spin_forbidden_config_t : default_config_t {
    static constexpr bool kSpinWaitSafe = false;
};

using config_t = spin_forbidden_config_t;

}  // namespace tr::graph
")
add_test(NAME spin_pool_guard COMMAND ${CMAKE_COMMAND}
         -DLT_CXX=${CMAKE_CXX_COMPILER}
         -DLT_CORE_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/../include
         -DLT_FORBIDDEN_INCLUDE=${CMAKE_CURRENT_BINARY_DIR}/spin-forbidden
         -DLT_PROBE=${CMAKE_CURRENT_SOURCE_DIR}/spin_pool_guard_probe.cpp
         -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spin_pool_guard.cmake)

if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_TCP)
    # #1223 step 2 — an accepted slot_server_t session's identity anchor: a real
    # transport_tcp_server, real dialled sockets, and the two hazards the shape had to clear
    # (unaddressable below the mount; :children[] untouched). Real threads ⇒ built under TSan.
    add_executable(session_anchor_test session_anchor_test.cpp)
    target_link_libraries(session_anchor_test PRIVATE libtracer)
    libtracer_test_warnings(session_anchor_test)
    add_test(NAME session_anchor COMMAND session_anchor_test)
endif()

# The #830 ablation: target_canonical_resolves() proves WHICH of dispatch_edge_target's two
# resolve spellings a local target edge took. The bound and canonical legs deliver identically
# by design, so a delivery assertion measures nothing here — the counter, together with the two
# control arms that make it MOVE (an edge admitted before its target existed; a retired-then-
# revived target), is the whole instrument. Appended at the END of this file deliberately: the
# citation pins at core/tests/CMakeLists.txt:1575,1588-1589 are enrolled non-source anchors that
# `check_doc_citations.py --repin` cannot move, so an insertion above them costs a hand re-pin
# of docs/modules/segment.md and of the anchor table for nothing.
add_executable(target_binding_ablation_test target_binding_ablation_test.cpp)
target_link_libraries(target_binding_ablation_test PRIVATE libtracer)
libtracer_test_warnings(target_binding_ablation_test)
add_test(NAME target_binding_ablation COMMAND target_binding_ablation_test)

# #894 / #1376 / ADR-0080 — the reclamation seam. Runs under whatever `reclaim_policy_t` this
# build bound: the re-entrant cases compile out under `reclaim_strict_t`, which forbids them, and
# the cross-thread case compiles out unless the bound policy's grace point spans threads. So ONE
# executable serves all three bindings and the CI matrix needs no second target. `Threads::Threads`
# is what property (g) needs — it parks a real second thread inside a delivery.
add_executable(reclaim_test reclaim_test.cpp)
target_link_libraries(reclaim_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(reclaim_test)
add_test(NAME reclaim COMMAND reclaim_test)

# #375 deliverable 3 / ADR-0044 + ADR-0047 §1 — the BUS MODULE as a build-time-closed seam.
# Runs under whatever `kBusLinks` this build bound: the present arm asserts the module works,
# the closed arm asserts it is absent AND that asking for it is refused rather than quietly
# demoted. ONE executable serves both bindings by `if constexpr`, exactly as reclaim_test does
# for ADR-0080, so the CI matrix needs no second target. It is deliberately NOT labelled `bus`
# below: it is the one test that must run in BOTH legs.
if(LIBTRACER_NET_PLANE AND LIBTRACER_TRANSPORT_TCP AND LIBTRACER_TRANSPORT_WS)
    add_executable(bus_module_test bus_module_test.cpp)
    target_link_libraries(bus_module_test PRIVATE libtracer Threads::Threads)
    libtracer_test_warnings(bus_module_test)
    add_test(NAME bus_module COMMAND bus_module_test)
endif()

# The `bus` CTest LABEL — the tests that need the ADR-0044 bus module PRESENT (#375
# deliverable 3), so a `kBusLinks = false` build can deselect them with `ctest -LE bus`.
#
# It is a label rather than a set of in-test `if constexpr` skips because the fact it records
# is a property of the TARGET, not of a case: each executable below either constructs a
# peer-named listener, wires a multi-peer link into the routing plane, or (the httpd_ws_*
# family) exercises a link that is a bus by construction and therefore never comes up on a
# bus-less build. Stating it here keeps it in ONE diffable place and makes it self-correcting:
# a new bus-dependent test that forgets the label reds the bus-closed leg rather than passing
# silently — which is the failure direction a deselection list is allowed to have.
#
# The converse is deliberate and belongs here so it is not "fixed" by a later reader:
# `link_token_carry` is NOT on this list even though one of its eight cases drives a census bus.
# There the bus dependence really is a property of a CASE — the other seven are tier-blind and one
# of them IS the FLAT tier, the shape a bus-closed node actually runs — so that one case gates
# itself on `tr::net::kBusLinks` in the test's `main` and PRINTS its skip. Labelling the target
# instead would deselect the carry from the only configuration that ships without a bus.
#
# `if(TEST ...)` because the set is module-gated above: a build without CAN, without the net
# plane, or without integrations/esp-idf/ present registers only a subset.
set(LIBTRACER_BUS_TESTS
    child_rx_churn
    collect
    edge_eviction
    fwd_flatten_backend
    mount_routing
    mount_shape_flip
    orchestrate_depart
    peer_handle_seam
    send_stall
    session_anchor
    tcp
    transport_can
    transport_can_peers
    transport_vertex
    write_ctx
    ws_transport
    httpd_ws_admission
    httpd_ws_adopted_host
    httpd_ws_auth
    httpd_ws_close_peer
    httpd_ws_departure_cost
    httpd_ws_eviction_lock
    httpd_ws_fanout_alloc
    httpd_ws_fanout_wait
    httpd_ws_fanout_width
    httpd_ws_in_call_reply
    httpd_ws_keepalive
    httpd_ws_send_stall
    httpd_ws_session_identity
    httpd_ws_teardown
    httpd_ws_tx_pool)
foreach(_libtracer_bus_test IN LISTS LIBTRACER_BUS_TESTS)
    if(TEST ${_libtracer_bus_test})
        set_property(TEST ${_libtracer_bus_test} APPEND PROPERTY LABELS bus)
    endif()
endforeach()

# The suites above that SEGFAULTED rather than failed under `kBusLinks = false` (#1438). The
# label already deselects them; this is about what happens to someone who runs the closed build
# WITHOUT the label filter, which used to be five crashes. Each of these now states a skip from
# its own `main` (tr::testing::skipped) and exits 77, which this property is what makes CTest
# read as `Skipped` rather than as a failure. 77 is the autotools convention CTest documents and
# `tr::testing::kSkipExitCode` is the single spelling of it on the C++ side — change both
# together. Only these five: the remaining `bus` suites already fail CLEANLY on that build,
# which is an acceptable answer for a test whose subject was compiled out, and converting all of
# them would be a much larger diff for no additional safety.
set(LIBTRACER_BUS_SKIPPABLE_TESTS edge_eviction send_stall session_anchor tcp ws_transport)
foreach(_libtracer_bus_test IN LISTS LIBTRACER_BUS_SKIPPABLE_TESTS)
    if(TEST ${_libtracer_bus_test})
        set_property(TEST ${_libtracer_bus_test} PROPERTY SKIP_RETURN_CODE 77)
    endif()
endforeach()

# The suites whose SUBJECT is the RFC-0014 S5 link-liveness engine (#1470). On a
# `kSelfHealLinks = false` build the engine is not in the image and
# `register_transport_type` refuses their `self_heal_dial` kind, so neither can do anything
# but crash or fail about a module the binary was never built with. Both state a skip from
# their own `main` (tr::testing::skipped) and exit 77; the label is the primary deselection
# (`ctest -LE selfheal`) and the property is what makes CTest read the exit as `Skipped` for
# anyone who runs them anyway. Same two-part shape as the `bus` block above, and 77 must stay
# in step with `tr::testing::kSkipExitCode`.
set(LIBTRACER_SELF_HEAL_TESTS link_liveness net_lock_order)
foreach(_libtracer_sh_test IN LISTS LIBTRACER_SELF_HEAL_TESTS)
    if(TEST ${_libtracer_sh_test})
        set_property(TEST ${_libtracer_sh_test} APPEND PROPERTY LABELS selfheal)
        set_property(TEST ${_libtracer_sh_test} PROPERTY SKIP_RETURN_CODE 77)
    endif()
endforeach()

# The other half of the refusal (#375 deliverable 3): `transport_can.cpp`'s
# `static_assert(kBusLinks, ...)`. A rejection is not observable from a compiled binary — by the
# time a test runs, the TU that should have been rejected has already compiled — so the check
# drives the compiler over the REAL TU under both bindings, exactly as the #1158 spin-pool guard
# does. Gated on the CAN module because that is the option whose combination with `kBusLinks =
# false` is the contradiction; the bus-closed CI leg turns CAN off and so never reaches it, which
# is precisely why this guard has to exist separately.
if(LIBTRACER_TRANSPORT_CAN)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/bus-closed/libtracer/config_override.hpp
"/**
 * @file
 * @brief GENERATED test fixture — the ADR-0044 bus module closed out (#375 deliverable 3).
 */
#pragma once

namespace tr::graph {

/** @brief The defaults, with no peer-named links at all (a point-to-point node). */
struct bus_closed_config_t : default_config_t {
    static constexpr bool kBusLinks = false;
};

using config_t = bus_closed_config_t;

}  // namespace tr::graph
")
    add_test(NAME bus_can_guard COMMAND ${CMAKE_COMMAND}
             -DLT_CXX=${CMAKE_CXX_COMPILER}
             -DLT_CORE_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/../include
             -DLT_BUS_CLOSED_INCLUDE=${CMAKE_CURRENT_BINARY_DIR}/bus-closed
             -DLT_PROBE=${CMAKE_CURRENT_SOURCE_DIR}/../src/transport_can.cpp
             -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bus_can_guard.cmake)
endif()

# #1460 phase (b) / RFC-0025 §7 vector 12 `stream/receiver-ring-flood` — the PLANE-ISOLATION
# arm: a flood that exhausts the net-plane store (the #603 / fe6adcd6 injected block_source_t
# seam) must leave the graph plane still allocating. Carries its own ablation as a control arm
# (both planes folded onto one source), so the suite proves its own instrument is reachable.
# Net-plane guarded: it drives a real fwd_router_t.
if(LIBTRACER_NET_PLANE)
    add_executable(plane_isolation_test plane_isolation_test.cpp)
    target_link_libraries(plane_isolation_test PRIVATE libtracer)
    libtracer_test_warnings(plane_isolation_test)
    add_test(NAME plane_isolation COMMAND plane_isolation_test)
endif()

# #1460 phases (a)+(c) / RFC-0025 §7 vector 12 `stream/receiver-ring-flood` — the RING-PRESSURE
# and RECOVERY arms: at exhaustion the RECEIVER's ring declines under §4.4 (best-effort sheds the
# oldest and accounts it on both faces; reliable refuses and answers BACKPRESSURE), and once the
# pressure clears it resumes, stops counting, and leaks no reservation. Not net-plane guarded —
# unlike its phase (b) sibling it drives no router, only the graph's own store verb.
add_executable(ring_pressure_test ring_pressure_test.cpp)
target_link_libraries(ring_pressure_test PRIVATE libtracer)
libtracer_test_warnings(ring_pressure_test)
add_test(NAME ring_pressure COMMAND ring_pressure_test)

# RFC-0025 §4.1 delivery classes, HONOURED (#1204 phase 3): what each class selects once
# Amendments 2/3/4 moved the machinery — the receiving vertex's ROLE and the source's role at
# flush, never a bit consulted in the fan-out loop. Every case carries its own ablation.
add_executable(delivery_class_honour_test delivery_class_honour_test.cpp)
target_link_libraries(delivery_class_honour_test PRIVATE libtracer)
libtracer_test_warnings(delivery_class_honour_test)
add_test(NAME delivery_class_honour COMMAND delivery_class_honour_test)

# RFC-0025 §4.2.1 / §4.1.2 — the BATCH record (batch.hpp): the fold, the sample clock DERIVED
# from the descriptor's rate at 0 bytes/sample, the non-uniform packed i32 offset array, and
# the reader refusals the codec does NOT make. Pins the `stream/batch-time-roundtrip` vector
# byte-exactly against the emitter, so it needs LIBTRACER_VECTORS_DIR. Not module-gated: the
# record is an L2/L3 codec concern and links no net-plane TU.
add_executable(batch_test batch_test.cpp)
target_link_libraries(batch_test PRIVATE libtracer)
target_compile_definitions(batch_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
libtracer_test_warnings(batch_test)
add_test(NAME batch COMMAND batch_test)

# RFC-0025 §4.7 (#1546) — the reference PLAYOUT helper (playout.hpp): the receiver-side
# derivation of the one clock never transmitted. Pins what it derives (uniform times as
# arithmetic over the descriptor's rate at 0 B/sample, non-uniform out of the packed i32 run,
# late flagged against the CALLER's now) and what its ruled scope REFUSES — no reorder, no
# drop, no de-jitter, no pacing — plus the zero-allocation claim, with a live-counter positive
# control. Host tests only: a receiver-side derivation has no wire-observable bytes.
add_executable(playout_test playout_test.cpp)
target_link_libraries(playout_test PRIVATE libtracer)
libtracer_test_warnings(playout_test)
add_test(NAME playout COMMAND playout_test)

if(LIBTRACER_NET_PLANE)
    # RFC-0004 Amendment 2 (#1502) — the six empty-src conformance vectors: a zero-length
    # `src` is "no reply requested", so the terminus applies the WRITE and emits nothing,
    # and an empty-src READ/AWAIT/mint-flagged frame is malformed and dropped.
    add_executable(empty_src_unacked_test empty_src_unacked_test.cpp)
    target_link_libraries(empty_src_unacked_test PRIVATE libtracer)
    libtracer_test_warnings(empty_src_unacked_test)
    add_test(NAME empty_src_unacked COMMAND empty_src_unacked_test)
endif()
