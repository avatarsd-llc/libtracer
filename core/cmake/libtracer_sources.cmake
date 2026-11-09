# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# THE core source list (#1702) — the one place a `core/src/*.cpp` is named.
#
# Every build that compiles core takes its sources from the groups below:
#   * core/CMakeLists.txt (the CMake build, host + every preset);
#   * integrations/esp-idf/libtracer/CMakeLists.txt (the ESP-IDF component), which
#     `include()`s this file from the repo, or from the copy the release job vendors
#     into the component next to core/src;
#   * PlatformIO compiles `core/src` by FILTER (`+<*>` minus a deny-list in
#     integrations/platformio/pio_esp32_can.py), so it cannot include a CMake file.
#     tools/check_source_list.py holds that deny-list to these groups instead.
#
# So adding or removing a core source is ONE edit here; neither CMake consumer keeps a
# copy, and the check above fails if a file on disk is in no group, a group names a file
# that is gone, a consumer names a core source directly, or the PlatformIO deny-list
# disagrees with the groups it is derived from.
#
# The groups are MODULES, not builds: each consumer decides which groups it compiles
# (CMake options, Kconfig bools, the PlatformIO opt-ins), and that selection logic stays
# with the consumer. Paths are absolute, so `include()` from anywhere yields the same list.
#
# tools/check_source_list.py parses this file: keep each group a plain
# `set(LIBTRACER_SOURCES_<GROUP> "${_libtracer_src}/<file>.cpp" ...)`.

get_filename_component(_libtracer_src "${CMAKE_CURRENT_LIST_DIR}/../src" ABSOLUTE)

# Required core, always compiled: the L2/L3 wire codec (frame + tlv_arena), the L0/L1
# substrate (backends + view/rope), path, the L4 graph runtime, and the in-process
# loopback transport (dev/test, no dependencies).
set(LIBTRACER_SOURCES_REQUIRED
    "${_libtracer_src}/frame.cpp"
    "${_libtracer_src}/tlv_arena.cpp"
    "${_libtracer_src}/backend_set.cpp"
    "${_libtracer_src}/device_backend.cpp"
    "${_libtracer_src}/mem_heap.cpp"
    "${_libtracer_src}/mem_source.cpp"
    "${_libtracer_src}/mem_source_backend.cpp"
    "${_libtracer_src}/mem_pool.cpp"
    "${_libtracer_src}/lkv_node_pool.cpp"
    "${_libtracer_src}/rope.cpp"
    "${_libtracer_src}/rope_decode.cpp"
    "${_libtracer_src}/tlv_view.cpp"
    "${_libtracer_src}/path.cpp"
    "${_libtracer_src}/graph.cpp"
    "${_libtracer_src}/graph_fields.cpp"
    "${_libtracer_src}/path_object.cpp"
    "${_libtracer_src}/link_index.cpp"
    "${_libtracer_src}/loopback.cpp"
    "${_libtracer_src}/static_thread.cpp"
)

# The FWD net/routing plane: op_resolve, fwd_router, transport_vertex.
set(LIBTRACER_SOURCES_NET_PLANE
    "${_libtracer_src}/fwd_reply.cpp"
    "${_libtracer_src}/op_resolve.cpp"
    "${_libtracer_src}/op_resolve_view.cpp"
    "${_libtracer_src}/fwd_router.cpp"
    "${_libtracer_src}/fwd_originate.cpp"
    "${_libtracer_src}/transport_vertex.cpp"
)

# The RFC-0014 S5 link-liveness engine (self_heal_link_t), a net-plane link module.
set(LIBTRACER_SOURCES_SELF_HEAL_LINKS
    "${_libtracer_src}/self_heal_link.cpp"
)

# The register_builtin_transports dispatcher has ONE source, this template (#2439). Every
# build generates its copy from it: the two CMake consumers through the function below,
# PlatformIO through integrations/platformio/libtracer_builtin_transports.py.
set(LIBTRACER_BUILTIN_DISPATCHER_TEMPLATE "${_libtracer_src}/builtin_transports.cpp.in")

# libtracer_generate_builtin_transports(<out_file> [<kind>...])
#
# Write the dispatcher for the built-in transports <kind>... (udp, tcp, ws: the glue TUs
# the caller compiles) to <out_file>. Each template call carries a `// builtin: <kind>`
# marker: the call of a listed kind is kept with its marker dropped, every other marked
# line is deleted. A kind the template does not know is a configure error. The file is
# rewritten only when its content changes, so a reconfigure recompiles nothing.
# tools/check_builtin_transports.py holds this and the PlatformIO generator byte-identical.
function(libtracer_generate_builtin_transports out_file)
    file(READ "${LIBTRACER_BUILTIN_DISPATCHER_TEMPLATE}" _text)
    string(REGEX MATCHALL "  // builtin: [a-z0-9_]+\n" _markers "${_text}")
    set(_known "")
    foreach(_marker IN LISTS _markers)
        string(REGEX REPLACE "^  // builtin: ([a-z0-9_]+)\n$" "\\1" _kind "${_marker}")
        list(APPEND _known "${_kind}")
    endforeach()
    foreach(_kind IN LISTS ARGN)
        if(NOT _kind IN_LIST _known)
            message(FATAL_ERROR "libtracer: no built-in transport '${_kind}' in "
                                "${LIBTRACER_BUILTIN_DISPATCHER_TEMPLATE} (known: ${_known})")
        endif()
    endforeach()
    foreach(_kind IN LISTS _known)
        if(_kind IN_LIST ARGN)
            string(REPLACE "  // builtin: ${_kind}\n" "\n" _text "${_text}")
        else()
            string(REGEX REPLACE "\n[^\n]*  // builtin: ${_kind}\n" "\n" _text "${_text}")
        endif()
    endforeach()
    set(_old "")
    if(EXISTS "${out_file}")
        file(READ "${out_file}" _old)
    endif()
    if(NOT _old STREQUAL _text)
        file(WRITE "${out_file}" "${_text}")
    endif()
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                     "${LIBTRACER_BUILTIN_DISPATCHER_TEMPLATE}")
    endif()
endfunction()

# The builtin socket transports: each transport's own TU, and separately its factory glue
# TU (register_<kind>_transport), which only a net-plane build compiles.
set(LIBTRACER_SOURCES_TRANSPORT_UDP
    "${_libtracer_src}/transport_udp.cpp"
)
set(LIBTRACER_SOURCES_TRANSPORT_TCP
    "${_libtracer_src}/transport_tcp.cpp"
)
set(LIBTRACER_SOURCES_TRANSPORT_WS
    "${_libtracer_src}/transport_ws.cpp"
)
set(LIBTRACER_SOURCES_BUILTIN_UDP
    "${_libtracer_src}/builtin_transport_udp.cpp"
)
set(LIBTRACER_SOURCES_BUILTIN_TCP
    "${_libtracer_src}/builtin_transport_tcp.cpp"
)
set(LIBTRACER_SOURCES_BUILTIN_WS
    "${_libtracer_src}/builtin_transport_ws.cpp"
)

# posix_endpoint_t, the recv-thread/endpoint scaffold every socket-owning transport shares.
set(LIBTRACER_SOURCES_POSIX_ENDPOINT
    "${_libtracer_src}/posix_endpoint.cpp"
)

# The wait module's two adapters (ADR-0087 §3, #2147), one per target, picked by which TU
# compiles: one epoll set on a Linux host, poll(2) over lwIP sockets everywhere else.
set(LIBTRACER_SOURCES_WAIT_EPOLL
    "${_libtracer_src}/wait_epoll.cpp"
)
set(LIBTRACER_SOURCES_WAIT_POLL
    "${_libtracer_src}/wait_poll.cpp"
)

# The CAN transport (portable framing/reassembly), plus its two can_link_t platform
# bindings: the real SocketCAN link (Linux only) and the always-off stub for elsewhere.
set(LIBTRACER_SOURCES_TRANSPORT_CAN
    "${_libtracer_src}/transport_can.cpp"
)
set(LIBTRACER_SOURCES_SOCKETCAN_LINUX
    "${_libtracer_src}/socketcan_link.cpp"
)
set(LIBTRACER_SOURCES_SOCKETCAN_STUB
    "${_libtracer_src}/socketcan_link_stub.cpp"
)

# The QUIC transport module (QUIC + WebTransport over msquic): the separate libtracer_quic
# target, never part of libtracer itself and never in an embedded build.
set(LIBTRACER_SOURCES_QUIC
    "${_libtracer_src}/transport_quic.cpp"
    "${_libtracer_src}/transport_webtransport.cpp"
)

unset(_libtracer_src)
