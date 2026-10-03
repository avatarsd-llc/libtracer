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
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/core/cmake/libtracer_sources.cmake")
    set(LIBTRACER_ROOT "${CMAKE_CURRENT_LIST_DIR}")
else()
    set(LIBTRACER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")
endif()

# The core source list is the ONE shared file core/CMakeLists.txt also includes (#1702):
# this component selects groups from it and never names a core/src source itself, so
# adding or removing a core source cannot leave the chip build behind.
include("${LIBTRACER_ROOT}/core/cmake/libtracer_sources.cmake")

set(LIBTRACER_SRCS
    # L2/L3 wire codec + L0/L1 substrate + L4 graph runtime + loopback (the P0 profile).
    # It includes device_backend.cpp, the DEVICE-space backend registry backend_set.cpp's
    # multi-member transfer dispatches through (#1381): an ESP node registers nothing, so
    # the table stays empty, but the multi-member fold is what this component builds.
    ${LIBTRACER_SOURCES_REQUIRED}
    # net plane (ADR-0038/0040): FWD routing + transport/connection vertices
    ${LIBTRACER_SOURCES_NET_PLANE}
)

# The RFC-0014 S5 link-liveness engine (#1470) — its own TU, so its own Kconfig bool, and the
# TU list and the config member below are driven by the SAME symbol, so the two can never
# disagree on this component the way a hand-paired core build can. OFF by default since
# v0.17.0 (#1670): a node that does not opt in keeps 4,336 B of reachable self_heal_link_t
# symbols out of its image (measured on a C6 image; 0 B of .dram0.bss), and its config-created
# udp/tcp/ws DIALs dial eagerly. A node staging its links with provide_link never reaches the
# engine either way — that path bypasses the factory catalog.
if(CONFIG_LIBTRACER_SELF_HEAL_LINKS)
    list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_SELF_HEAL_LINKS})
endif()

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
# ESP-IDF WebSocket never uses POSIX sockets. core's ws_server_transport_t /
# ws_client_transport_t are the HOST implementation: they open their own ::socket, run
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
    list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_BUILTIN_UDP} ${LIBTRACER_SOURCES_TRANSPORT_UDP})
    string(APPEND LIBTRACER_BUILTIN_TRANSPORT_CALLS
        "    register_udp_transport(vertex, rx_backend);\n")
endif()
if(CONFIG_LIBTRACER_TRANSPORT_TCP)
    list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_BUILTIN_TCP} ${LIBTRACER_SOURCES_TRANSPORT_TCP})
    string(APPEND LIBTRACER_BUILTIN_TRANSPORT_CALLS
        "    register_tcp_transport(vertex, rx_backend);\n")
endif()
# The portable WS pair rides LIBTRACER_PORTABLE_WS on top of its module bool (see above):
# ON only where POSIX sockets are the right answer, i.e. the `linux` target.
if(CONFIG_LIBTRACER_TRANSPORT_WS AND LIBTRACER_PORTABLE_WS)
    list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_BUILTIN_WS} ${LIBTRACER_SOURCES_TRANSPORT_WS})
    string(APPEND LIBTRACER_BUILTIN_TRANSPORT_CALLS
        "    register_ws_transport(vertex, rx_backend);\n")
endif()

# posix_endpoint_t — the recv-thread/endpoint scaffold shared by every socket transport.
# WS counts toward it only where the PORTABLE (socket-owning) WS pair compiles; the
# IDF-native links never touch posix_endpoint_t.
if(CONFIG_LIBTRACER_TRANSPORT_UDP OR CONFIG_LIBTRACER_TRANSPORT_TCP
   OR (CONFIG_LIBTRACER_TRANSPORT_WS AND LIBTRACER_PORTABLE_WS))
    list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_POSIX_ENDPOINT})
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
    list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_BUILTIN_DISPATCHER})
else()
    configure_file(
        "${LIBTRACER_BUILTIN_DISPATCHER_TEMPLATE}"
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
    list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_TRANSPORT_CAN})
    if(IDF_TARGET STREQUAL "linux")
        list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_SOCKETCAN_LINUX})
    else()
        list(APPEND LIBTRACER_SRCS ${LIBTRACER_SOURCES_SOCKETCAN_STUB} "twai_link.cpp")
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
    # embedded-native REPLACEMENT for core's raw-socket ws_server_transport_t, a platform
    # link picked by which TU compiles (never an in-source #ifdef). Since #947 that is a
    # true replacement, not a coexistence: transport_ws.cpp is NOT compiled here at all
    # (LIBTRACER_PORTABLE_WS is FALSE on chips), so nothing relies on --gc-sections to
    # drop it — the registration in the built-in catalog kept it reachable and it never
    # got dropped. Gated on CONFIG_LIBTRACER_WS_SERVER (#1670), which depends on the WS
    # module (#393) and SELECTS both CONFIG_HTTPD_WS_SUPPORT (the TU rides esp_http_server's
    # native WS API) and CONFIG_LIBTRACER_BUS_LINKS (the link is a bus by construction, and
    # the TU static_asserts kBusLinks) — so an image that serves WebSocket always carries
    # the bus tier, and one that does not keeps the lean default.
    if(CONFIG_LIBTRACER_WS_SERVER)
        list(APPEND LIBTRACER_SRCS "httpd_ws_link.cpp")
    elseif(CONFIG_LIBTRACER_TRANSPORT_WS AND CONFIG_HTTPD_WS_SUPPORT)
        # The pre-v0.17.0 shape: TRANSPORT_WS=y used to select HTTPD_WS_SUPPORT and compile
        # the server with it, so an sdkconfig carried over from then has both and no
        # WS_SERVER. Say so at configure time rather than leaving a bare undefined reference
        # to httpd_ws_link_t at link time naming no cause (the #963 lesson, re-applied).
        message(WARNING
            "libtracer: CONFIG_LIBTRACER_TRANSPORT_WS=y and CONFIG_HTTPD_WS_SUPPORT=y, but "
            "CONFIG_LIBTRACER_WS_SERVER is off, so httpd_ws_link_t (the WebSocket SERVER link) "
            "is NOT compiled. Since v0.17.0 (#1670) the server is its own option: set "
            "CONFIG_LIBTRACER_WS_SERVER=y (it selects CONFIG_LIBTRACER_BUS_LINKS) if this node "
            "serves WebSocket. Ignore this if CONFIG_HTTPD_WS_SUPPORT is on for another reason.")
    endif()
    # Chip targets: the esp_transport_ws-backed WS *client* link (esp_ws_client_link_t) — the
    # embedded-native replacement for core's raw-socket ws_client_transport_t (which fails the
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

# libtracer/config_override.hpp — this component's per-target compile-time knobs, delivered
# as plain C++ (ADR-0068 §Erratum 1). The DEFAULTS are not spelled here at all: they are C++
# literals in the checked-in core/include/libtracer/config.hpp, which discovers this fragment
# with __has_include and uses the `tr::graph::config_t` it binds. The fragment INHERITS
# `tr::graph::default_config_t` and states only the knobs an ESP target changes, so a knob
# added to config.hpp later reaches this build at its new default — which is why no drift gate
# has to hold the two files in step.
# The generated directory is listed BEFORE core/include in INCLUDE_DIRS below, so the fragment
# is found from the component's own sources AND from every dependent (`REQUIRES libtracer`) —
# one file per build means every TU agrees, which is what the old PUBLIC compile definition
# existed to guarantee.
# The copy-or-share threshold (RFC-0028 §5.3) is the NARROW per-build trait §11 ruling 2 leaves
# to the target: copy always (SIZE_MAX). A shared value holds its whole RX pool slot for as long
# as it is a vertex's value, and a small fixed pool cannot fund that borrow — RFC-0022
# Amendment 2 measured a 29-slot C6 pool collapsing once the live shared set crossed the slot
# count. It is exactly the posture this component shipped before the knob (the retired pin
# ratio's never-pin sentinel); an app that wants zero-copy on a vertex declares it per vertex
# with `vertex_policy_t::share_threshold_bytes`.
# What this component deliberately does NOT state, because core's default is already the right
# answer here: the ACL profile (`allow_only_policy_t`, the MCU profile — no full-ACL Kconfig
# yet) and the hazard reader slots (unused on a chip, which binds the single-writer slot below).
# CONFIG_* is defined only in the SECOND (build) CMake pass; in the early
# requirement-expansion pass the fallback writes the stock default (16), which the
# second pass then overwrites with the real value before anything compiles.
if(CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES)
    set(LIBTRACER_VERTEX_LOCK_STRIPES ${CONFIG_LIBTRACER_VERTEX_LOCK_STRIPES})
else()
    set(LIBTRACER_VERTEX_LOCK_STRIPES 16)
endif()
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
# With this false, mem_pool.hpp refuses to instantiate a synchronized_pool_t over any guard
# declaring `may_spin` and points at tr::esp::critical_pool_t instead — the mistake
# becomes a compile error rather than a task_wdt reset found under load. TRUE on the `linux`
# host target, which has a real preemptive multi-core scheduler. Read from IDF_TARGET, which
# unlike CONFIG_* is defined in BOTH CMake passes, so the early expansion pass renders the same
# value the build pass does.
if(IDF_TARGET STREQUAL "linux")
    set(LIBTRACER_SPIN_WAIT_SAFE true)
else()
    set(LIBTRACER_SPIN_WAIT_SAFE false)
endif()
# The LKV slot (#1618, RFC 0028 §5.5) — DERIVED with the spin-wait fact above, because it is
# the same fact. core refuses any slot that can spin where kSpinWaitSafe is false, so every chip
# target binds `single_writer_slot_t`, whose one wait is an interrupt-masked critical section
# (`tr::esp::critical_guard_t`). The single-writer trait it used to state was removed (#1718).
# The `linux` host target keeps core's default (the same slot over the host mutex guard). The
# guard header includes only FreeRTOS, so including it from the fragment is not a cycle.
if(LIBTRACER_SPIN_WAIT_SAFE)
    set(LIBTRACER_SLOT_INCLUDE "")
    set(LIBTRACER_SLOT_BINDING "")
else()
    set(LIBTRACER_SLOT_INCLUDE "#include \"libtracer_esp/critical_guard.hpp\"\n")
    set(LIBTRACER_SLOT_BINDING
"    using guard_t = tr::esp::critical_guard_t;
    using lkv_slot_t = single_writer_slot_t;
")
endif()
# Inline fan-out width (#1708) — DERIVED from the target, like the two facts above. Every
# publish reserves `kInlineFanout * sizeof(edge_view_t)` bytes of the publishing task's stack
# for its subscriber snapshot (28 B a view on rv32), whatever the vertex's fan-out. A chip node's
# vertices carry one or two subscribers, so 2 keeps every such publish off the allocator and
# gives the other six slots back: graph_t::fan_out's frame measures 304 B at 8 and 128 B at 2
# (rv32 -Os, GCC 15.2, -fstack-usage) on every publishing task; a wider fan-out
# still delivers to every subscriber through the overflow vector. The `linux` host target keeps
# core's default (8), as it does the slot.
if(IDF_TARGET STREQUAL "linux")
    set(LIBTRACER_INLINE_FANOUT 8)
else()
    set(LIBTRACER_INLINE_FANOUT 2)
endif()
# The two link modules (#1470, #1670). CONFIG_* is undefined in the early requirement-expansion
# pass, so each fallback is its Kconfig DEFAULT — `n` for both since v0.17.0 — and the second
# pass then writes the real value before anything compiles. The TU lists that pair with them
# (self_heal_link.cpp; transport_can.cpp, which `depends on` the bus bool in Kconfig) are
# CONFIG_-gated and read only in that second pass too, so the fragment and the TU list agree
# whenever anything is compiled; `self_heal_link.cpp` and `transport_can.cpp` each
# static_assert their member if they ever did not.
if(CONFIG_LIBTRACER_SELF_HEAL_LINKS)
    set(LIBTRACER_SELF_HEAL_LINKS true)
else()
    set(LIBTRACER_SELF_HEAL_LINKS false)
endif()
if(CONFIG_LIBTRACER_BUS_LINKS)
    set(LIBTRACER_BUS_LINKS true)
else()
    set(LIBTRACER_BUS_LINKS false)
endif()
# Worker stack bytes; 0 = the IDF pthread default. Same early-pass fallback reasoning.
if(CONFIG_LIBTRACER_SELF_HEAL_WORKER_STACK)
    set(LIBTRACER_SELF_HEAL_WORKER_STACK ${CONFIG_LIBTRACER_SELF_HEAL_WORKER_STACK})
else()
    set(LIBTRACER_SELF_HEAL_WORKER_STACK 0)
endif()
# Ingress drain budget (ADR-0085). DEFINED, not truthy: 0 is a legal value (no budget) and
# must not fall back to the default. Absent — the early pass, or a build without the WS
# server — binds the core defaults, which only httpd_ws_link_t reads.
if(DEFINED CONFIG_LIBTRACER_WS_SERVER_RX_DRAIN_FRAMES)
    set(LIBTRACER_RX_DRAIN_FRAMES ${CONFIG_LIBTRACER_WS_SERVER_RX_DRAIN_FRAMES})
else()
    set(LIBTRACER_RX_DRAIN_FRAMES 32)
endif()
if(DEFINED CONFIG_LIBTRACER_WS_SERVER_RX_DRAIN_BYTES)
    set(LIBTRACER_RX_DRAIN_BYTES ${CONFIG_LIBTRACER_WS_SERVER_RX_DRAIN_BYTES})
else()
    set(LIBTRACER_RX_DRAIN_BYTES 32768)
endif()
# A non-zero budget waits for the idle task, so it must not be bound in a build that does not
# watch the idle task: there a never-blocking low-priority task may keep it from ever running,
# and the link would stop for good after one budget (ADR-0085). Kconfig defaults both to 0 in
# that case; this names an explicit override that undid it.
if(CONFIG_LIBTRACER_WS_SERVER AND NOT CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
   AND (LIBTRACER_RX_DRAIN_FRAMES OR LIBTRACER_RX_DRAIN_BYTES))
    message(WARNING "libtracer: CONFIG_LIBTRACER_WS_SERVER_RX_DRAIN_FRAMES/_BYTES are non-zero but "
                    "CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 is off. The drain waits for the idle "
                    "task; if a low-priority task never blocks, httpd_ws_link_t stops reading for "
                    "good after one budget. Set both to 0 unless the idle task is known to run.")
endif()
set(LIBTRACER_CONFIG_OVERRIDE_PATH
    "${CMAKE_CURRENT_BINARY_DIR}/generated/include/libtracer/config_override.hpp")
set(LIBTRACER_CONFIG_OVERRIDE_TEXT
"/**
 * @file
 * @brief GENERATED — this ESP-IDF build's configuration as an override fragment (ADR-0068).
 *
 * Written by integrations/esp-idf/libtracer/CMakeLists.txt from the component's Kconfig
 * values and the chip facts above it. Do not edit; do not add a knob here that is not also
 * a tr::graph::default_config_t member in core/include/libtracer/config.hpp.
 */
#pragma once

#include <cstddef>
${LIBTRACER_SLOT_INCLUDE}
namespace tr::graph {

/** @brief Core's defaults with this component's Kconfig and chip-derived knobs applied. */
struct esp_config_t : default_config_t {
    static constexpr std::size_t kVertexLockStripes = ${LIBTRACER_VERTEX_LOCK_STRIPES};
    static constexpr std::size_t kCacheLineBytes = ${LIBTRACER_CACHE_LINE_BYTES};
    static constexpr std::size_t kEdgePinSlots = ${LIBTRACER_EDGE_PIN_SLOTS};
    static constexpr std::size_t kInlineFanout = ${LIBTRACER_INLINE_FANOUT};
    static constexpr bool kSpinWaitSafe = ${LIBTRACER_SPIN_WAIT_SAFE};
    static constexpr bool kBusLinks = ${LIBTRACER_BUS_LINKS};
    static constexpr bool kSelfHealLinks = ${LIBTRACER_SELF_HEAL_LINKS};
    static constexpr std::size_t kSelfHealWorkerStackBytes = ${LIBTRACER_SELF_HEAL_WORKER_STACK};
    static constexpr std::size_t kRxDrainFrames = ${LIBTRACER_RX_DRAIN_FRAMES};
    static constexpr std::size_t kRxDrainBytes = ${LIBTRACER_RX_DRAIN_BYTES};
    static constexpr std::size_t kShareThresholdBytes = ~std::size_t{0};
    static constexpr std::size_t kSizeClasses[] = {~std::size_t{0}};
${LIBTRACER_SLOT_BINDING}};

using config_t = esp_config_t;

}  // namespace tr::graph
")
# Write only when the bytes actually change. `configure_file` gave that for free and this
# component reconfigures often (menuconfig, and the two-pass component expansion runs this
# block twice per configure); an unconditional file(WRITE) would touch the header every time
# and rebuild every TU that includes it, which is all of them.
set(LIBTRACER_CONFIG_OVERRIDE_PREV "")
if(EXISTS "${LIBTRACER_CONFIG_OVERRIDE_PATH}")
    file(READ "${LIBTRACER_CONFIG_OVERRIDE_PATH}" LIBTRACER_CONFIG_OVERRIDE_PREV)
endif()
if(NOT LIBTRACER_CONFIG_OVERRIDE_PREV STREQUAL LIBTRACER_CONFIG_OVERRIDE_TEXT)
    file(WRITE "${LIBTRACER_CONFIG_OVERRIDE_PATH}" "${LIBTRACER_CONFIG_OVERRIDE_TEXT}")
endif()

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
