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

# The nothrow control-plane block seam (mem_source.hpp, #551) — its contract, the
# structural "not a memory_resource" assertions the ruling rests on, and the
# appended graph_t parameter's zero-diff promise.
add_executable(mem_source_test mem_source_test.cpp)
target_link_libraries(mem_source_test PRIVATE libtracer)
libtracer_test_warnings(mem_source_test)
add_test(NAME mem_source COMMAND mem_source_test)

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

# FWD offset-dispatch frame view (fwd_frame_view.hpp) — the extracted fwd_router
# forward-plane cluster, tested directly over hand-built frames (span + rope
# cursors): dst-seg / op / control peeks, the shrunk-dst/grown-src head rebuild
# proved byte-exact vs a reference re-encode, and malformed rejects.
add_executable(fwd_frame_view_test fwd_frame_view_test.cpp)
target_link_libraries(fwd_frame_view_test PRIVATE libtracer)
libtracer_test_warnings(fwd_frame_view_test)
add_test(NAME fwd_frame_view COMMAND fwd_frame_view_test)

# The shared SPEC-config SETTINGS walk (config_reader.hpp) — the pair-consuming scan
# every transport config parser funnels through (#927): the value-read-as-a-key hijack
# vectors, the forward-compat tolerance that must SURVIVE the fix (the opposite ruling
# from the ACL parse), and the representative per-kind configs pinned unchanged.
add_executable(config_reader_test config_reader_test.cpp)
target_link_libraries(config_reader_test PRIVATE libtracer)
libtracer_test_warnings(config_reader_test)
add_test(NAME config_reader COMMAND config_reader_test)

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

# RFC-0024 car 3 — the FORWARDER hop and the origin-side bind, over a three-node
# loopback chain with production `/net:children[]` wiring.
add_executable(bound_forward_test bound_forward_test.cpp)
target_link_libraries(bound_forward_test PRIVATE libtracer)
libtracer_test_warnings(bound_forward_test)
target_compile_definitions(bound_forward_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME bound_forward COMMAND bound_forward_test)

add_executable(path_ref_test path_ref_test.cpp)
target_link_libraries(path_ref_test PRIVATE libtracer)
libtracer_test_warnings(path_ref_test)
add_test(NAME path_ref COMMAND path_ref_test)

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

# #848 — the transport EGRESS framing path soft-fails: a refusing allocator yields a
# DROPPED FRAME AND A LIVE NODE (never abort()), across ws / tcp / udp / can, plus the
# RFC 6455 §5.5 control-frame rules that make the PONG unfailable. This TU overrides the
# global operator new with a fail-the-k-th-allocation injector, so it must stay its OWN
# executable (the override is process-wide).
add_executable(transport_alloc_softfail_test transport_alloc_softfail_test.cpp)
target_link_libraries(transport_alloc_softfail_test PRIVATE libtracer)
libtracer_test_warnings(transport_alloc_softfail_test)
add_test(NAME transport_alloc_softfail COMMAND transport_alloc_softfail_test)

# ADR-0044 — stateless transport-peer enumeration + transparent per-peer FWD over
# the CAN bus binding: join hellos -> synthesized /net/can0:children[], directed
# forwarding to an announced peer with the reply routed back, peer expiry, and the
# per-distinct-node peer-table growth. In-memory fake link, no kernel CAN.
add_executable(transport_can_peers_test transport_can_peers_test.cpp)
target_link_libraries(transport_can_peers_test PRIVATE libtracer)
libtracer_test_warnings(transport_can_peers_test)
add_test(NAME transport_can_peers COMMAND transport_can_peers_test)

# #55 (increment 2) — real-bus smoke test over a kernel vcan0 device, plus the #931
# can_link_t seam-rule vectors (an injected RTR / 11-bit standard frame must not
# cross ingress; an over-length classic frame must not cross egress). Self-skips
# (clean exit) when vcan0 cannot bind, so it is harmless in the normal suite; the
# dedicated can-vcan-e2e CI job sets vcan0 up so the SocketCAN path runs for real.
# LINUX-ONLY: it speaks <linux/can.h> directly to inject the frames no well-formed
# link would emit — platform selection is a build-system concern, never an in-source
# #ifdef (the same doctrine that picks socketcan_link.cpp over its stub).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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

    # #944 — the TX WORK SLOT's lifetime, on the same fake. The event the three suites
    # above never spend is the one the fake already models: an enqueue DROPPED inside
    # lwIP while httpd_queue_work still returns ESP_OK (set_queue_capacity). The work
    # item never runs, and it used to be the only thing that could release the slot it
    # had claimed — four of those and the pool was dead for the rest of the boot.
    add_executable(httpd_ws_tx_strand_test
        "${LIBTRACER_ESP_DIR}/tests/host/httpd_ws_tx_strand_test.cpp"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_httpd.cpp"
        "${LIBTRACER_ESP_DIR}/libtracer/httpd_ws_link.cpp")
    target_include_directories(httpd_ws_tx_strand_test PRIVATE
        "${LIBTRACER_ESP_DIR}/tests/host"
        "${LIBTRACER_ESP_DIR}/tests/host/fake_idf"
        "${LIBTRACER_ESP_DIR}/libtracer/include")
    target_link_libraries(httpd_ws_tx_strand_test PRIVATE libtracer)
    target_link_options(httpd_ws_tx_strand_test PRIVATE "-Wl,--wrap=send" "-Wl,--wrap=shutdown")
    libtracer_test_warnings(httpd_ws_tx_strand_test)
    add_test(NAME httpd_ws_tx_strand COMMAND httpd_ws_tx_strand_test)
    # Adopted-mode teardown leaks the handler gate BY DESIGN (the #815 precedent), and
    # every case here constructs an adopting link — same exemption as the suites above.
    set_tests_properties(httpd_ws_tx_strand
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
else()
    message(STATUS "httpd_ws_teardown_test skipped: integrations/esp-idf is not in this tree")
endif()

# ADR-0050 — the pure ACL policy seam: both adapters + typed parse/build, no graph.
add_executable(security_acl_test security_acl_test.cpp)
target_link_libraries(security_acl_test PRIVATE libtracer)
libtracer_test_warnings(security_acl_test)
add_test(NAME security_acl COMMAND security_acl_test)

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

# #81-A — per-endpoint :acl structural storage (store/serve raw ACL TLV; no enforcement).
add_executable(acl_test acl_test.cpp)
target_link_libraries(acl_test PRIVATE libtracer)
libtracer_test_warnings(acl_test)
add_test(NAME acl COMMAND acl_test)

# #407 — graph_t::retire() vertex retirement (RFC-0009 §B/§C/§E.6).
add_executable(mount_routing_test mount_routing_test.cpp)
target_link_libraries(mount_routing_test PRIVATE libtracer)
libtracer_test_warnings(mount_routing_test)
add_test(NAME mount_routing COMMAND mount_routing_test)

# `add_child` under a REFUSING allocator (#523): the registry chunk is the one soft-failing
# allocation on the control path, and it is driven by replacing the global NOTHROW
# `operator new` — a whole-program decision, hence its own binary.
add_executable(mount_add_oom_test mount_add_oom_test.cpp)
target_link_libraries(mount_add_oom_test PRIVATE libtracer)
libtracer_test_warnings(mount_add_oom_test)
add_test(NAME mount_add_oom COMMAND mount_add_oom_test)

# `make_connection` under the same REFUSING allocator (#930): a connection whose link cannot
# be wired into the router is rolled back rather than published as a live-looking dead
# connection. Its own binary for the same whole-program reason as mount_add_oom_test.
add_executable(conn_add_oom_test conn_add_oom_test.cpp)
target_link_libraries(conn_add_oom_test PRIVATE libtracer)
libtracer_test_warnings(conn_add_oom_test)
add_test(NAME conn_add_oom COMMAND conn_add_oom_test)

add_executable(mount_width_test mount_width_test.cpp)
target_link_libraries(mount_width_test PRIVATE libtracer)
libtracer_test_warnings(mount_width_test)
add_test(NAME mount_width COMMAND mount_width_test)

add_executable(compact_cache_test compact_cache_test.cpp)
target_link_libraries(compact_cache_test PRIVATE libtracer)
libtracer_test_warnings(compact_cache_test)
add_test(NAME compact_cache COMMAND compact_cache_test)

# ADR-0063 §3: control-plane create/remove churn against a live forward reader. Its value is
# under `-fsanitize=thread` (the tsan CI lane runs the whole suite), where it reports the
# writer-vs-writer and rebind-vs-reader races; without TSan it still catches a lost or
# duplicated child. RUN_SERIAL so a loaded CI machine does not starve its four threads.
add_executable(net_control_plane_race_test net_control_plane_race_test.cpp)
target_link_libraries(net_control_plane_race_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(net_control_plane_race_test)
add_test(NAME net_control_plane_race COMMAND net_control_plane_race_test)
set_tests_properties(net_control_plane_race PROPERTIES RUN_SERIAL TRUE TIMEOUT 300)

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

add_executable(registry_teardown_test registry_teardown_test.cpp)
target_link_libraries(registry_teardown_test PRIVATE libtracer)
libtracer_test_warnings(registry_teardown_test)
add_test(NAME registry_teardown COMMAND registry_teardown_test)

# #884 — the router's OWN per-child table under create/remove churn: the twin of the
# registry teardown contract above, one layer out. Every pointer involved stays live, so the
# failures this pins are wrong-tenancy resolutions and unbounded growth, NOT memory errors —
# which is why it is an ordinary host test and its ASan value is the report it does NOT make.
add_executable(child_rx_churn_test child_rx_churn_test.cpp)
target_link_libraries(child_rx_churn_test PRIVATE libtracer)
libtracer_test_warnings(child_rx_churn_test)
add_test(NAME child_rx_churn COMMAND child_rx_churn_test)

add_executable(retire_test retire_test.cpp)
target_link_libraries(retire_test PRIVATE libtracer)
libtracer_test_warnings(retire_test)
add_test(NAME retire COMMAND retire_test)

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

# #739 — subscribe_toward: a local producer bound toward a mount-path target through
# the shared strip-K descent (no caller-side link/route hand-split).
add_executable(subscribe_toward_test subscribe_toward_test.cpp)
target_link_libraries(subscribe_toward_test PRIVATE libtracer)
libtracer_test_warnings(subscribe_toward_test)
add_test(NAME subscribe_toward COMMAND subscribe_toward_test)

# #684 — the rebind-vs-forward race on the published mount run (a TSan vehicle that is
# also a functional misroute detector on every build).
add_executable(registry_rebind_race_test registry_rebind_race_test.cpp)
target_link_libraries(registry_rebind_race_test PRIVATE libtracer)
libtracer_test_warnings(registry_rebind_race_test)
add_test(NAME registry_rebind_race COMMAND registry_rebind_race_test)

# #914 — the router's five sink pairs are published as a unit: a setter racing the frame
# path can no longer hand a new `fn` the previous sink's `ctx` (TSan vehicle + a functional
# wrong-context detector on every build).
add_executable(fwd_sink_race_test fwd_sink_race_test.cpp)
target_link_libraries(fwd_sink_race_test PRIVATE libtracer Threads::Threads)
libtracer_test_warnings(fwd_sink_race_test)
add_test(NAME fwd_sink_race COMMAND fwd_sink_race_test)

# #419 — the two-mount route: one dst crossing TWO net/<module>/<name> mounts, driven
# through three in-process routers chained by loopback link pairs. The behavioural binding
# for the fwd/fwd-routed-two-mount conformance vector (HARNESS.md: a vector gates the codec
# only), and the first FWD test in the tree with more than one forwarder.
add_executable(fwd_two_mount_test fwd_two_mount_test.cpp)
target_link_libraries(fwd_two_mount_test PRIVATE libtracer)
libtracer_test_warnings(fwd_two_mount_test)
add_test(NAME fwd_two_mount COMMAND fwd_two_mount_test)

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

# #477 — the store/delivery path's nothrow soft-fail discipline: OOM injected through
# tr::detail::probe_fail_hook (store => BACKPRESSURE, delivery => drop/defer, never abort).
add_executable(graph_oom_softfail_test graph_oom_softfail_test.cpp)
target_link_libraries(graph_oom_softfail_test PRIVATE libtracer)
libtracer_test_warnings(graph_oom_softfail_test)
add_test(NAME graph_oom_softfail COMMAND graph_oom_softfail_test)

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

# RFC-0022 (as amended) — the per-subscription delivery policy, the removed vertex knobs,
# and §3.F: the two surviving storage declarations are owner-side and inherited by NOBODY.
add_executable(qos_policy_test qos_policy_test.cpp)
target_link_libraries(qos_policy_test PRIVATE libtracer)
libtracer_test_warnings(qos_policy_test)
target_compile_definitions(qos_policy_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME qos_policy COMMAND qos_policy_test)

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

# RFC-0004 §E.1/§F (#974) — a COMPACT delivery is ACL-gated under the INBOUND LINK's name,
# like the full-route FWD{WRITE} it compacts. Both terminus write arms of on_compact (the
# cold deliver_local one and the memoized warm one) are proven separately, each paired with
# a positive control, plus the no-resolver default where nothing is enforced.
add_executable(fwd_compact_acl_test fwd_compact_acl_test.cpp)
target_link_libraries(fwd_compact_acl_test PRIVATE libtracer)
libtracer_test_warnings(fwd_compact_acl_test)
add_test(NAME fwd_compact_acl COMMAND fwd_compact_acl_test)

# RFC-0004 §E.1 (#716) — the MID-CHAIN reconnect self-heal: clear_link sweeps every ingress
# binding, on ANY link, whose downstream half crossed the cleared link, so an origin that never
# saw the reconnect stops streaming onto a dead out-label. Asserts RECOVERY end to end over a
# 3-node in-process chain, not merely the NACK.
add_executable(fwd_reconnect_selfheal_test fwd_reconnect_selfheal_test.cpp)
target_link_libraries(fwd_reconnect_selfheal_test PRIVATE libtracer)
libtracer_test_warnings(fwd_reconnect_selfheal_test)
add_test(NAME fwd_reconnect_selfheal COMMAND fwd_reconnect_selfheal_test)

# #913 — a re-advertise cycle grows NOTHING: both ADVERTISE-emitting sites (the producer door
# and the mid-chain forwarding swap) reuse the label already bound to an identical route, so a
# reconnect loop cannot walk a link's 16-bit label space or its egress table. Counts distinct
# labels and table entries across 51 cycles at every node of a two-hop chain.
add_executable(fwd_readvertise_reuse_test fwd_readvertise_reuse_test.cpp)
target_link_libraries(fwd_readvertise_reuse_test PRIVATE libtracer)
libtracer_test_warnings(fwd_readvertise_reuse_test)
add_test(NAME fwd_readvertise_reuse COMMAND fwd_readvertise_reuse_test)

# RFC-0004 / ADR-0035 slice 4 (#136) — the PRODUCER auto fan-out: a plain graph.write to a
# vertex with a remote subscriber delivers a full-route FWD{WRITE} (or auto-promoted COMPACT)
# with no explicit advertise/send. Covers the transient-local latch, the compact byte-delta,
# reconnect self-heal, and a concurrent writer x clear_link race (the TSan gate).
add_executable(fwd_fanout_test fwd_fanout_test.cpp)
target_link_libraries(fwd_fanout_test PRIVATE libtracer)
libtracer_test_warnings(fwd_fanout_test)
add_test(NAME fwd_fanout COMMAND fwd_fanout_test)

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

add_executable(edge_eviction_test edge_eviction_test.cpp)
target_link_libraries(edge_eviction_test PRIVATE libtracer)
libtracer_test_warnings(edge_eviction_test)
add_test(NAME edge_eviction COMMAND edge_eviction_test)

# ADR-0053 ④b — the FWD forward hop over a MULTI-LINK rope, without flattening: an oracle
# equality that every adversarial split routes byte-identically to the contiguous path,
# plus the terminus flatten fallback.
add_executable(fwd_terminus_reject_test fwd_terminus_reject_test.cpp)
target_link_libraries(fwd_terminus_reject_test PRIVATE libtracer)
libtracer_test_warnings(fwd_terminus_reject_test)
add_test(NAME fwd_terminus_reject COMMAND fwd_terminus_reject_test)

add_executable(fwd_malformed_control_test fwd_malformed_control_test.cpp)
target_link_libraries(fwd_malformed_control_test PRIVATE libtracer)
libtracer_test_warnings(fwd_malformed_control_test)
add_test(NAME fwd_malformed_control COMMAND fwd_malformed_control_test)

# #730 — the router's flatten seam: every materialize() draws from the injected
# mem_backend_t, and a refused flatten is answered by value (never stored empty).
add_executable(fwd_flatten_backend_test fwd_flatten_backend_test.cpp)
target_link_libraries(fwd_flatten_backend_test PRIVATE libtracer)
libtracer_test_warnings(fwd_flatten_backend_test)
add_test(NAME fwd_flatten_backend COMMAND fwd_flatten_backend_test)

# #766 — the TERMINUS half of that seam: the rope-tier flattens one call below
# resolve_terminus_rope draw from the same injection, and a refusal is answered by
# value (drop, or an addressed BACKPRESSURE) instead of read as a short span.
add_executable(terminus_flatten_backend_test terminus_flatten_backend_test.cpp)
target_link_libraries(terminus_flatten_backend_test PRIVATE libtracer)
libtracer_test_warnings(terminus_flatten_backend_test)
add_test(NAME terminus_flatten_backend COMMAND terminus_flatten_backend_test)

# #795 — the terminus reply's EGRESS-construction seam (ADR-0074): the FWD{REPLY}
# head and the RFC-0024 mint draw from the injected `egress` backend, not the global
# heap, and a refusal degrades by value (drop, or an addressed BACKPRESSURE).
add_executable(terminus_egress_backend_test terminus_egress_backend_test.cpp)
target_link_libraries(terminus_egress_backend_test PRIVATE libtracer)
libtracer_test_warnings(terminus_egress_backend_test)
add_test(NAME terminus_egress_backend COMMAND terminus_egress_backend_test)

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

# #903 — the stream_endpoint_t full-write helpers under a REAL signal storm: a
# socketpair with a small SO_SNDBUF, a deliberately slow reader, and SIGUSR1
# (no SA_RESTART) fired at the writer thread, so a blocked send returns EINTR
# mid-frame. Both write_all and write_all_iov must resume, not truncate.
add_executable(write_all_eintr_test write_all_eintr_test.cpp)
target_link_libraries(write_all_eintr_test PRIVATE libtracer)
libtracer_test_warnings(write_all_eintr_test)
add_test(NAME write_all_eintr COMMAND write_all_eintr_test)

# #948 — the write-fault classification: a malformed send/sendmsg call (EOPNOTSUPP,
# EINVAL) is OUR defect, not a dead socket, so it must be re-attempted and counted
# rather than swallowed as a disconnect. Driven through the
# tr::detail::write_fault_inject_hook seam, since a host kernel emits none of those
# errnos here — a real-socket-only test would classify nothing.
add_executable(write_fault_class_test write_fault_class_test.cpp)
target_link_libraries(write_fault_class_test PRIVATE libtracer)
libtracer_test_warnings(write_fault_class_test)
add_test(NAME write_fault_class COMMAND write_fault_class_test)

# #54 — transport_ws server socket layer (TCP accept + RFC6455 handshake + transport_t).
add_executable(ws_transport_test ws_transport_test.cpp)
target_link_libraries(ws_transport_test PRIVATE libtracer)
libtracer_test_warnings(ws_transport_test)
add_test(NAME ws_transport COMMAND ws_transport_test)

# #872 — the WS DATA path is bounded by the injected (backend, max_frame) seam: an over-cap
# declared length is refused off the frame HEADER (measured against the process peak RSS, not
# inferred), fragments cannot sum past the cap, and backend exhaustion is counted backpressure.
add_executable(ws_rx_bound_test ws_rx_bound_test.cpp)
target_link_libraries(ws_rx_bound_test PRIVATE libtracer)
libtracer_test_warnings(ws_rx_bound_test)
add_test(NAME ws_rx_bound COMMAND ws_rx_bound_test)

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
endif()

# mem_cuda GPU test — only with LIBTRACER_WITH_CUDA; run on a real GPU locally
# (tools/test-cuda.sh), never in CI.
if(LIBTRACER_WITH_CUDA)
    add_executable(cuda_test cuda_test.cpp)
    target_link_libraries(cuda_test PRIVATE libtracer)
    libtracer_test_warnings(cuda_test)
    add_test(NAME cuda COMMAND cuda_test)
endif()
