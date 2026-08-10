# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# ESP-IDF component wrapper for the libtracer C++ reference core.
#
# The sources live in the repo's core/; this component pulls them in so an
# ESP-IDF project can depend on libtracer through the component manager. It
# packages the FULL NODE (#183): the wire codec, the L0/L1 substrate, the L4
# graph runtime, AND the net plane — fwd_router / op_resolve / route_handle /
# transport_vertex (child_registry is header-only) plus the socket transports
# (udp / tcp over lwIP BSD sockets), loopback, and transport_can with the
# ESP TWAI can_link_t (twai_link.cpp, this directory). WebSocket on a chip target
# is IDF-native, NOT a socket transport — see the WS-plane block below.
#
# Platform TU selection happens HERE, in the build system — never via in-source
# #ifdefs (the no-feature-macro ruling):
#   * chip targets compile socketcan_link_stub.cpp (SocketCAN is Linux-only;
#     socketcan_link_t::ok() stays false) and twai_link.cpp (the TWAI-backed
#     can_link_t, needs esp_driver_twai + FreeRTOS headers);
#   * the `linux` (POSIX host) target compiles the REAL socketcan_link.cpp
#     (glibc has <linux/can.h>) and no twai_link (no TWAI peripheral);
#   * chip targets compile the IDF-native WS links (httpd_ws_link.cpp,
#     esp_ws_client_link.cpp) and NOT core's POSIX-socket transport_ws.cpp; the
#     `linux` target does the opposite (#947).
#
# Requires the GCC 13+ toolchain (C++23: std::expected, std::span) shipped with
# ESP-IDF >= 5.3; CI builds with release-v6.0 (GCC 15).

# Where core/ lives, resolved for BOTH consumption modes:
#   * In-repo: this component dir is integrations/esp-idf/libtracer/, so the repo
#     root (which holds core/) is three levels up. An ESP-IDF project consumes the
#     component in place by pointing `EXTRA_COMPONENT_DIRS` at it (see the bundled
#     examples' top-level CMakeLists.txt).
#   * Published registry component: the release job vendors core/ INTO this
#     directory so the archive is self-contained (a consumer only gets the
#     component, not the whole repo). When that vendored copy is present, prefer
#     it. This keeps one CMakeLists correct in both modes.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/core/src/frame.cpp")
    set(LIBTRACER_ROOT "${CMAKE_CURRENT_LIST_DIR}")
else()
    set(LIBTRACER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
endif()

set(LIBTRACER_SRCS
    # L2/L3 wire codec + L0/L1 substrate + L4 graph runtime (the P0 profile)
    "${LIBTRACER_ROOT}/core/src/frame.cpp"
    "${LIBTRACER_ROOT}/core/src/tlv_arena.cpp"
    "${LIBTRACER_ROOT}/core/src/backend_set.cpp"
    "${LIBTRACER_ROOT}/core/src/mem_heap.cpp"
    "${LIBTRACER_ROOT}/core/src/mem_source.cpp"
    "${LIBTRACER_ROOT}/core/src/mem_pool.cpp"
    "${LIBTRACER_ROOT}/core/src/rope.cpp"
    "${LIBTRACER_ROOT}/core/src/rope_decode.cpp"
    "${LIBTRACER_ROOT}/core/src/tlv_view.cpp"
    "${LIBTRACER_ROOT}/core/src/path.cpp"
    "${LIBTRACER_ROOT}/core/src/graph.cpp"
    # net plane (ADR-0038/0040): FWD routing + transport/connection vertices
    "${LIBTRACER_ROOT}/core/src/fwd_reply.cpp"
    "${LIBTRACER_ROOT}/core/src/op_resolve.cpp"
    "${LIBTRACER_ROOT}/core/src/op_resolve_view.cpp"
    "${LIBTRACER_ROOT}/core/src/route_handle.cpp"
    "${LIBTRACER_ROOT}/core/src/fwd_router.cpp"
    "${LIBTRACER_ROOT}/core/src/transport_vertex.cpp"
    "${LIBTRACER_ROOT}/core/src/loopback.cpp"
)

# --- Built-in transport modules (#393) — per-module gating, mirroring the core CMake
# options LIBTRACER_TRANSPORT_{UDP,TCP,WS}. Each transport's glue factory + socket TU
# compile ONLY when its Kconfig bool is on; all default y, so a stock node is
# byte-identical to before. Turning one off sheds its TUs from libtracer.a and makes a
# SPEC of that `kind` resolve to SCHEMA_NOT_FOUND — the RAM-audit "compose the C6 for
# its role" lever, with NO in-source feature macros (selection is which TUs compile).
#
# NB the two-pass model: CONFIG_* is UNDEFINED in IDF's early requirement-expansion
# pass, so these guards read as OFF there. That is harmless — SRCS are consumed only in
# the second (build) pass, where sdkconfig (default y) is loaded. The transport
# PRIV_REQUIRES below therefore stay UNCONDITIONAL, for the same reason a Kconfig-gated
# REQUIRES never propagates (it evaluates false in the early pass).

# --- Which WS PLANE this target gets (#947, ruling; #948) ---------------------------
# ESP-IDF WebSocket never uses POSIX sockets. core's transport_ws_server /
# transport_ws_client are the HOST implementation: they open their own ::socket, run
# their own poll thread, and egress through posix_endpoint's sendmsg(MSG_NOSIGNAL) —
# a flag lwIP DEFINES but lwip_sendmsg REJECTS with EOPNOTSUPP, which write_all_iov
# reads as peer-gone. On a chip target they are therefore not merely 18 KB of code no
# node reaches; every scatter-gather data frame they emit is silently discarded while
# the handshake and PING/PONG still work (#948). So on chip targets they are ABSENT,
# not fixed: the sanctioned plane is the IDF-native links below (httpd_ws_link_t on
# esp_http_server, esp_ws_client_link_t on esp_websocket_client), which the
# application binds via provide_link.
#
# This is a TU choice, exactly as socketcan_link.cpp vs. socketcan_link_stub.cpp is —
# never an in-source #if defined(ESP_PLATFORM) (the no-feature-macro doctrine). The
# `linux` (POSIX host) target keeps the portable pair: it has glibc's sendmsg, and it
# has no esp_http_server to host the native links.
#
# Consequence on a chip target: no register_ws_transport call is generated, so a
# `:children[]` SPEC carrying `kind=ws` and NO staged link resolves to
# SCHEMA_NOT_FOUND — the same answer any un-registered kind gets. A node serving
# WebSocket on silicon stages an IDF-native link with provide_link instead, which
# bypasses the factory catalog entirely (register_module never consults it).
if(IDF_TARGET STREQUAL "linux")
    set(LIBTRACER_PORTABLE_WS TRUE)
else()
    set(LIBTRACER_PORTABLE_WS FALSE)
endif()

set(LIBTRACER_BUILTIN_TRANSPORT_CALLS "")
if(CONFIG_LIBTRACER_TRANSPORT_UDP)
    list(APPEND LIBTRACER_SRCS
        "${LIBTRACER_ROOT}/core/src/builtin_transport_udp.cpp"
        "${LIBTRACER_ROOT}/core/src/transport_udp.cpp")
    string(APPEND LIBTRACER_BUILTIN_TRANSPORT_CALLS
        "    register_udp_transport(vertex, rx_backend);\n")
endif()
if(CONFIG_LIBTRACER_TRANSPORT_TCP)
    list(APPEND LIBTRACER_SRCS
        "${LIBTRACER_ROOT}/core/src/builtin_transport_tcp.cpp"
        "${LIBTRACER_ROOT}/core/src/transport_tcp.cpp")
    string(APPEND LIBTRACER_BUILTIN_TRANSPORT_CALLS
        "    register_tcp_transport(vertex, rx_backend);\n")
endif()
# The portable WS pair rides LIBTRACER_PORTABLE_WS on top of its module bool (see above):
# ON only where POSIX sockets are the right answer, i.e. the `linux` target.
if(CONFIG_LIBTRACER_TRANSPORT_WS AND LIBTRACER_PORTABLE_WS)
    list(APPEND LIBTRACER_SRCS
        "${LIBTRACER_ROOT}/core/src/builtin_transport_ws.cpp"
        "${LIBTRACER_ROOT}/core/src/transport_ws.cpp")
    string(APPEND LIBTRACER_BUILTIN_TRANSPORT_CALLS
        "    register_ws_transport(vertex, rx_backend);\n")
endif()

# posix_endpoint_t — the recv-thread/endpoint scaffold shared by every socket transport.
# WS counts toward it only where the PORTABLE (socket-owning) WS pair compiles; the
# IDF-native links never touch posix_endpoint_t.
if(CONFIG_LIBTRACER_TRANSPORT_UDP OR CONFIG_LIBTRACER_TRANSPORT_TCP
   OR (CONFIG_LIBTRACER_TRANSPORT_WS AND LIBTRACER_PORTABLE_WS))
    list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/posix_endpoint.cpp")
endif()

# register_builtin_transports dispatcher: the hand-written full-node form (udp+tcp+ws)
# when all three are on — identical to a stock node — else a CMake-generated variant
# from src/builtin_transports.cpp.in that calls ONLY the enabled register_*_transport, so
# a dropped transport leaves neither a glue TU nor a dangling call (same mechanism as
# core/CMakeLists.txt). A chip target takes the GENERATED branch even with all three
# module bools on, because its ws leg is absent (LIBTRACER_PORTABLE_WS): the hand-written
# form's register_ws_transport call is exactly the reference that would drag the portable
# WS objects back into the image.
if(CONFIG_LIBTRACER_TRANSPORT_UDP AND CONFIG_LIBTRACER_TRANSPORT_TCP
   AND CONFIG_LIBTRACER_TRANSPORT_WS AND LIBTRACER_PORTABLE_WS)
    list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/builtin_transports.cpp")
else()
    configure_file(
        "${LIBTRACER_ROOT}/core/src/builtin_transports.cpp.in"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_transports.cpp"
        @ONLY)
    list(APPEND LIBTRACER_SRCS "${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_transports.cpp")
endif()

# lwip is a PRIVATE requirement of the socket transports' .cpp files only
# (<sys/socket.h> etc. resolve into lwIP on chip targets); the public headers
# expose no lwIP types. On the `linux` target the same includes resolve into
# glibc, so no lwip component is needed (and IDF does not provide one there).
set(LIBTRACER_PRIV_REQUIRES pthread)
set(LIBTRACER_PRIV_INCLUDE_DIRS "")

# CAN transport (#393-gated): portable framing/reassembly (transport_can.cpp) + its
# can_link_t platform binding — the real SocketCAN link on the linux host (glibc ships
# <linux/can.h>), a SocketCAN stub + the TWAI-backed can_link_t (twai_link.cpp) on chip
# targets. Which link compiles is a TU choice, never an in-source #ifdef. Off => the
# whole CAN plane (and the TWAI driver glue) is dropped from the archive.
if(CONFIG_LIBTRACER_TRANSPORT_CAN)
    list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/transport_can.cpp")
    if(IDF_TARGET STREQUAL "linux")
        list(APPEND LIBTRACER_SRCS "${LIBTRACER_ROOT}/core/src/socketcan_link.cpp")
    else()
        list(APPEND LIBTRACER_SRCS
            "${LIBTRACER_ROOT}/core/src/socketcan_link_stub.cpp"
            "twai_link.cpp")
    endif()
endif()

if(NOT IDF_TARGET STREQUAL "linux")
    # Chip targets: the interrupt-disable critical-section pool policy (#770, ADR-0060 §2)
    # — the single-core-MCU synchronisation variant of mem::synchronized_pool_t. The
    # adapter is a header; this TU exists purely to instantiate it, so every chip build
    # type-checks the policy against the portMUX macros. Not compiled on `linux` (no
    # portMUX there); the host proves the SEAM instead (core/tests/mem_sync_policy_test).
    # Nothing links against it by default — it is opt-in construction, so an application
    # that never names critical_pool_t leaves the symbols for --gc-sections to drop.
    list(APPEND LIBTRACER_SRCS "critical_pool.cpp")
    # Chip targets: the esp_http_server-backed WS transport link (httpd_ws_link_t) — the
    # embedded-native REPLACEMENT for core's raw-socket transport_ws_server, a platform
    # link picked by which TU compiles (never an in-source #ifdef). Since #947 that is a
    # true replacement, not a coexistence: transport_ws.cpp is NOT compiled here at all
    # (LIBTRACER_PORTABLE_WS is FALSE on chips), so nothing relies on --gc-sections to
    # drop it — the registration in the built-in catalog kept it reachable and it never
    # got dropped. Gated on BOTH the WS module (#393) and CONFIG_HTTPD_WS_SUPPORT (the TU
    # rides esp_http_server's native WS API, which the component ships only when that
    # config is on) — so an application that wants the native WS SERVER must set
    # CONFIG_HTTPD_WS_SUPPORT=y; there is no portable fallback behind it any more.
    if(CONFIG_LIBTRACER_TRANSPORT_WS AND CONFIG_HTTPD_WS_SUPPORT)
        list(APPEND LIBTRACER_SRCS "httpd_ws_link.cpp")
    elseif(CONFIG_LIBTRACER_TRANSPORT_WS)
        # Reachable only when an existing sdkconfig pins CONFIG_HTTPD_WS_SUPPORT=n
        # explicitly — the Kconfig now SELECTs it, so a fresh configure cannot land here.
        # Say so rather than letting the build go green and fail at link with an
        # undefined httpd_ws_link_t constructor naming no cause (#963).
        message(WARNING
            "libtracer: CONFIG_LIBTRACER_TRANSPORT_WS=y but CONFIG_HTTPD_WS_SUPPORT=n — "
            "httpd_ws_link_t (the WebSocket SERVER link) is NOT being compiled. The client "
            "link is unaffected. Set CONFIG_HTTPD_WS_SUPPORT=y, or turn off "
            "CONFIG_LIBTRACER_TRANSPORT_WS if this node neither serves nor dials WebSocket.")
    endif()
    # Chip targets: the esp_transport_ws-backed WS *client* link (esp_ws_client_link_t) — the
    # embedded-native replacement for core's raw-socket transport_ws_client (which fails the
    # handshake on lwIP silicon), the dial mirror of httpd_ws_link_t. Uses tcp_transport (core
    # IDF), so gated on CONFIG_LIBTRACER_TRANSPORT_WS alone (no esp_http_server dependency).
    # It is the ONLY ws dialer on a chip target since #947.
    if(CONFIG_LIBTRACER_TRANSPORT_WS)
        list(APPEND LIBTRACER_SRCS "esp_ws_client_link.cpp")
    endif()
    # esp_driver_gpio (twai_link.cpp names its pins via <driver/gpio.h>), esp_driver_twai
    # (the TWAI peripheral) and esp_http_server (httpd_ws_link.cpp's native WS API) MUST be
    # UNCONDITIONAL requirements — NOT gated on the transport bools or CONFIG_HTTPD_WS_SUPPORT.
    # IDF resolves component requirements in an early-expansion pass where CONFIG_* symbols
    # are not yet defined, so a Kconfig-gated REQUIRES silently evaluates false there and
    # never propagates the component's include dir; the TU (correctly gated on the same
    # config in the second pass) then compiles WITHOUT its header and dies with "No such
    # file" on any node that enables it (the origin firmware's prod build hit exactly this).
    # These headers are base IDF components present on every chip target, so requiring them
    # always is safe and free when their TU is absent — nothing references the symbols, so
    # --gc-sections drops the link dep and libtracer.a is byte-identical (no footprint change
    # to a transport-trimmed build the sentinel measures). lwip stays unconditional for the
    # same reason (the socket transports' <sys/socket.h> resolves into it).
    # PUBLIC vs PRIVATE, and the split is decided by ONE test: does a header under
    # `include/libtracer_esp/` name that component's header? PRIV_REQUIRES does not
    # propagate include dirs, so a dependent that includes one of ours and does not
    # independently require the base component dies with "esp_http_server.h: No such file
    # or directory" and no hint that libtracer is the cause (#963).
    #
    # Three do, and they are REQUIRES:
    #   esp_http_server   <- include/libtracer_esp/httpd_ws_link.hpp:119
    #   tcp_transport     <- include/libtracer_esp/esp_ws_client_link.hpp:157
    #   esp_driver_twai   <- include/libtracer_esp/twai_link.hpp:36-37
    # Two do not, and stay private: lwip (the socket transports' <sys/socket.h> resolves
    # into it, from .cpp only) and esp_driver_gpio (twai_link.cpp names its pins).
    #
    # Unconditional for the reason above this line — a Kconfig-gated REQUIRES evaluates
    # false in the early expansion pass — and free when the TU is absent: nothing
    # references the symbols, so --gc-sections drops the link dep.
    list(APPEND LIBTRACER_REQUIRES esp_http_server tcp_transport esp_driver_twai)
    list(APPEND LIBTRACER_PRIV_REQUIRES lwip esp_driver_gpio)
    # compat/include: POSIX headers IDF's libc spells differently (<poll.h> ->
    # <sys/poll.h>) — private, so core sources stay pure POSIX and dependents
    # never see the shim.
    list(APPEND LIBTRACER_PRIV_INCLUDE_DIRS "compat/include")
endif()

# libtracer/config.hpp — per-target compile-time knobs delivered as plain C++
# (ADR-0068). Generated here from the Kconfig values and listed BEFORE core/include,
# so the generated file shadows the checked-in default for the component's own
# sources AND every dependent (`REQUIRES libtracer`) — one file per build means every
# TU agrees, which is what the old PUBLIC compile definition existed to guarantee.
# CONFIG_* is defined only in the SECOND (build) CMake pass; in the early
# requirement-expansion pass the fallback renders the stock default (16), which the
# second pass then overwrites with the real value before anything compiles.
if(CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES)
    set(LIBTRACER_VERTEX_LOCK_STRIPES ${CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES})
else()
    set(LIBTRACER_VERTEX_LOCK_STRIPES 16)
endif()
set(LIBTRACER_ACL_POLICY allow_only_policy_t)  # the MCU profile — no full-ACL Kconfig yet
set(LIBTRACER_LKV_SLOT sp_atomic_slot_t)  # write-dominated single core: keep the refcount slot
set(LIBTRACER_HAZARD_READER_SLOTS 64)  # unused here: the refcount slot never builds the domain
# Edge-pin announcements (#635) — one index per task that may PUBLISH concurrently. Unlike the
# hazard domain this one is ALWAYS emitted, so it is real static RAM: N * max(padding,
# sizeof(cell_t)) bytes. A cell is the announced pointer PLUS its claimed flag, so it is 8 B on
# rv32 at padding 0 (not 4) — i.e. 64 B at the 8 chosen here, matching the +256 B .bss the
# 32-slot host default costs. A node's publishers are its
# transport-receive tasks plus the app's own; eight covers that with room and stays a rounding
# error against the 896 B the stripe padding costs. Undersizing it is not a correctness problem
# — a task past the bound copies the edge array under the stripe mutex instead, which is what
# every task did before #635.
set(LIBTRACER_EDGE_PIN_SLOTS 8)
# False-sharing padding: DERIVED, not a Kconfig option. A unicore chip (C2/C3/C6/H2, or a
# dual-core built with CONFIG_FREERTOS_UNICORE) has no second core to false-share with, so
# the padding is pure static RAM loss — measured 896 B of .bss for 16 stripes on rv32 -Os.
# Asking the integrator would only invite the wrong answer: the chip already knows.
if(CONFIG_FREERTOS_UNICORE)
    set(LIBTRACER_CACHE_LINE_BYTES 0)
else()
    set(LIBTRACER_CACHE_LINE_BYTES 64)
endif()
# Spin-wait safety (#1158, #963.3) — DERIVED from the target, like the padding above, and for
# the same reason: the chip already knows and asking would only invite the wrong answer. FALSE
# on every chip target, because the hazard is priority-preemptive scheduling and not core
# count: a spinner that outranks the lock holder never yields the CPU the holder needs, so on
# an SMP chip whose spinner and holder happen to share a core the wedge is identical to a C6's.
# With this false, mem_pool.hpp refuses to instantiate synchronized_pool_t<spin_sync_t> (the
# `tr::mem::sync_pool_t` alias) and points at tr::esp::critical_pool_t instead — the mistake
# becomes a compile error rather than a task_wdt reset found under load. TRUE on the `linux`
# host target, which has a real preemptive multi-core scheduler. Read from IDF_TARGET, which
# unlike CONFIG_* is defined in BOTH CMake passes, so the early expansion pass renders the same
# value the build pass does.
if(IDF_TARGET STREQUAL "linux")
    set(LIBTRACER_SPIN_WAIT_SAFE true)
else()
    set(LIBTRACER_SPIN_WAIT_SAFE false)
endif()
configure_file(
    "${LIBTRACER_ROOT}/core/include/libtracer/config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/libtracer/config.hpp"
    @ONLY)

idf_component_register(
    SRCS ${LIBTRACER_SRCS}
    INCLUDE_DIRS
        "${CMAKE_CURRENT_BINARY_DIR}/generated/include"
        "${LIBTRACER_ROOT}/core/include"
        "include"
    PRIV_INCLUDE_DIRS ${LIBTRACER_PRIV_INCLUDE_DIRS}
    REQUIRES ${LIBTRACER_REQUIRES}
    PRIV_REQUIRES ${LIBTRACER_PRIV_REQUIRES}
)

# pthread is a PRIVATE requirement, not a public (REQUIRES) one. CORE's public
# headers expose only libstdc++ headers (<atomic>, <mutex>, <shared_mutex>,
# <thread>) — never ESP-IDF's <pthread.h> — so the component does not propagate a
# pthread dependency to its dependents. (Scoped to `core/` deliberately: the
# `libtracer_esp/` headers DO expose IDF types, which is why three components moved
# to REQUIRES above — #963. The pthread claim is unaffected by that.) The pthread symbols
# (pthread_create, etc.) are pulled in by libstdc++'s gthread threading, which
# the *.cpp objects need at link time; that's exactly what PRIV_REQUIRES models.
# (The old `REQUIRES pthread` over-declared it as a public dependency.)

# libtracer's core is C++23. ESP-IDF compiles component sources with the project's
# C++ standard; pin it here so the component is correct regardless of project default.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)

# -Werror=switch, mirroring core/CMakeLists.txt — the SAME core sources compile here, and the
# hand-written maps out of closed enums (status_t -> wire::err_t at the L4->wire cast point,
# status_t -> its wire name) have no `default:` label and no fall-through tail, so an unmapped
# enumerator must be a red build wherever those TUs are compiled (#876). Set explicitly rather
# than inherited: a project's warning level is the project's business, and IDF promotes
# warnings to errors only under CONFIG_COMPILER_WARN_WRITE_STRINGS-style opt-ins and its own CI
# flag — this guarantee cannot be left to the consumer's build settings. PRIVATE, so a
# dependent component's switches are untouched.
target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=switch)

# The vertex lock-stripe count (CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES, see Kconfig) now
# rides the generated config.hpp above — it reaches every TU as the ordinary constexpr
# `tr::graph::kVertexLockStripes` through ONE shared header, so the old PUBLIC compile
# definition (and the ODR essay that justified it) is gone (ADR-0068).

# Ship the component archive as `libtracer.a`, not the double-prefixed
# `liblibtracer.a` that ESP-IDF's `lib${COMPONENT_NAME}.a` convention produces for
# a component literally named `libtracer`. This only renames the output file; the
# component NAME stays `libtracer` (so `REQUIRES libtracer`, the idf_component.yml
# registry entry, and the docs are all unchanged) and IDF links by CMake target,
# not by archive file name, so nothing in the link graph is affected. The
# esp-idf-size footprint report matches the archive by the substring `libtracer`
# (tools/esp_size_gate.py --component-pattern), which still hits `libtracer.a`.
set_target_properties(${COMPONENT_LIB} PROPERTIES OUTPUT_NAME tracer)
