# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

cmake_minimum_required(VERSION 3.16)

# Removed configure options (#1722, ADR-0068 §Erratum 1). Each duplicated a trait of the one
# configuration type, which is set in a libtracer/config_override.hpp fragment as plain C++.
# A value that would have changed the build is refused, naming the trait; a value equal to the
# old default changed nothing, so a build directory that merely cached it is told once and the
# entry is dropped. Checked before project(), so a refused configure costs no compiler probe.
if(DEFINED LIBTRACER_ACL_FULL)
    if(LIBTRACER_ACL_FULL)
        message(FATAL_ERROR "LIBTRACER_ACL_FULL was removed (#1722). Bind the ACL policy in "
                            "your libtracer/config_override.hpp instead: "
                            "`using acl_policy_t = full_acl_policy_t;` in a struct that "
                            "inherits tr::graph::default_config_t (docs/modules/config.md).")
    endif()
    message(WARNING "LIBTRACER_ACL_FULL was removed (#1722) and its default value did nothing; "
                    "dropping it from the cache. The ACL policy is config_t::acl_policy_t.")
    unset(LIBTRACER_ACL_FULL CACHE)
endif()
if(DEFINED LIBTRACER_LKV_SLOT)
    if(NOT LIBTRACER_LKV_SLOT STREQUAL "single_writer_slot_t")
        message(FATAL_ERROR "LIBTRACER_LKV_SLOT was removed (#1722). Bind the LKV slot in your "
                            "libtracer/config_override.hpp instead: "
                            "`using lkv_slot_t = ${LIBTRACER_LKV_SLOT};` in a struct that "
                            "inherits tr::graph::default_config_t (docs/modules/config.md).")
    endif()
    message(WARNING "LIBTRACER_LKV_SLOT was removed (#1722) and its default value did nothing; "
                    "dropping it from the cache. The LKV slot is config_t::lkv_slot_t.")
    unset(LIBTRACER_LKV_SLOT CACHE)
endif()

# Version — one source of truth, no hardcoded literal to drift. Resolution order:
#   1. the release git tag (`vMAJOR.MINOR.PATCH`), when building at a tag, so a
#      tagged release reports its exact version;
#   2. otherwise the repo-root `VERSION` file — the single hand-edited version for
#      the core release axis, mirrored into the package manifests by
#      `tools/sync-version.py` (CI-gated by version-consistency.yml);
#   3. a last-resort literal, reached only by a vendored `core/` copied away from
#      the repo root (no `VERSION` file present).
# The protocol/wire version is a separate axis, pinned by docs/spec/v1.md.
set(_libtracer_version "")
find_program(GIT_EXECUTABLE git)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0 --match "v[0-9]*"
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE _libtracer_git_tag
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _libtracer_git_result)
    if(_libtracer_git_result EQUAL 0 AND _libtracer_git_tag)
        # Strip the leading `v`; project() wants a bare MAJOR.MINOR.PATCH.
        string(REGEX REPLACE "^v" "" _libtracer_version "${_libtracer_git_tag}")
    endif()
endif()
if(NOT _libtracer_version AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" _libtracer_version_file)
    string(STRIP "${_libtracer_version_file}" _libtracer_version)
endif()
if(NOT _libtracer_version)
    # Last resort: a `core/` tree vendored away from the repo-root VERSION file.
    set(_libtracer_version 0.3.0)
endif()

project(libtracer VERSION ${_libtracer_version} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Per-module build options — the project's own "a node is a chosen set of modules"
# design. Each option toggles whether its module's translation unit(s) are COMPILED
# into libtracer; it is NOT a compile-definition, so a dropped module leaves neither
# code nor a symbol behind. Selection is by which TUs compile (the no-feature-macro
# doctrine, cf. socketcan_link.cpp vs. its stub, picked by CMAKE_SYSTEM_NAME). The
# transports and the net plane default ON. The two LINK MODULES — CAN (a bus by
# construction) and the S5 liveness engine — follow the lean-default ruling (#1670,
# v0.17.0): OFF for a consumer, ON for this project's own test build, which exercises
# them and binds the matching config members through its preset fragment
# (tests/instrumented).
#
#   * The required core (below) ALWAYS compiles: the L2/L3 wire codec (frame +
#     tlv_arena), the L0/L1 substrate (backends + view/rope), path, and the L4 graph
#     runtime. A pure in-process node needs nothing more.
#   * LIBTRACER_NET_PLANE adds the FWD routing plane (op_resolve, route_handle,
#     fwd_router, transport_vertex) — a node that forwards to other nodes.
#   * LIBTRACER_TRANSPORT_{TCP,UDP,WS,CAN} each add their transport's TU(s), plus
#     shared deps (posix_endpoint when any socket transport is on; socketcan_link when
#     CAN is on). udp/tcp/ws are the net-plane builtins auto-registered on the
#     transport vertex; can/quic/webtransport are opt-in external factories.
option(LIBTRACER_NET_PLANE
       "Build the FWD net/routing plane (op_resolve, route_handle, fwd_router, transport_vertex)"
       ON)
option(LIBTRACER_TRANSPORT_TCP "Build the TCP stream transport (tcp_transport_t)" ON)
option(LIBTRACER_TRANSPORT_UDP "Build the UDP datagram transport (udp_transport_t)" ON)
option(LIBTRACER_TRANSPORT_WS "Build the WebSocket transport (transport_ws_*)" ON)
# The link-module default (#1670): ON only for this project's top-level test build. A CAN link
# is peer-named by construction, so it needs `kBusLinks = true` — and `transport_can.cpp`
# refuses to compile without it — which the lean default no longer binds. The test build's
# preset fragment binds it, so the default follows the same condition that installs that
# fragment (below, at add_subdirectory(tests)).
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    set(_libtracer_link_modules_default ON)
else()
    set(_libtracer_link_modules_default OFF)
endif()
option(LIBTRACER_TRANSPORT_CAN
       "Build the CAN transport (transport_can + socketcan_link); needs kBusLinks = true"
       ${_libtracer_link_modules_default})
# The RFC-0014 §4 S5 link-liveness engine (self_heal_link_t) — #1470. Its own TU, so its
# own switch. OFF by default since v0.17.0 (#1670, the lean default), which keeps the 4,336 B
# of reachable `self_heal_link_t` symbols the reporter measured on an ESP32-C6 image out of a
# node that does not ask for them (`nm` on the linked ELF; ±0 B of .bss — the engine is code,
# not a static table). ON for this project's own test build, like CAN above.
#
# PAIR IT WITH THE CONFIG MEMBER. This switch decides which TUs compile; whether the engine
# is REACHABLE is `tr::graph::default_config_t::kSelfHealLinks`, bound by an override
# fragment (ADR-0068), exactly as `kBusLinks` governs the ADR-0044 bus tier. To opt in, turn
# this ON AND bind `kSelfHealLinks = true`. Binding the member without the TU is a LINK error
# naming `self_heal_link_t` — deliberately, because the mint site would be compiled in with
# no implementation behind it. The converse mistake is caught earlier and more clearly:
# `self_heal_link.cpp` opens with a `static_assert(kSelfHealLinks, …)`.
option(LIBTRACER_SELF_HEAL_LINKS
       "Build the RFC-0014 S5 link-liveness engine (self_heal_link_t); needs kSelfHealLinks = true"
       ${_libtracer_link_modules_default})

# The core source list lives in ONE file (#1702), shared with the ESP-IDF component and
# checked against the PlatformIO deny-list by tools/check_source_list.py. This file only
# SELECTS groups from it; it never names a core/src source itself.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/libtracer_sources.cmake)

# The required core (always compiled): L2/L3 wire codec + L0/L1 substrate + path + L4
# graph runtime, plus the in-process loopback transport (dev/test, no dependencies; a
# tracer.hpp umbrella member).
add_library(libtracer STATIC ${LIBTRACER_SOURCES_REQUIRED})

# The net/routing plane (LIBTRACER_NET_PLANE). The graph runtime above works without it
# for a pure in-process node.
if(LIBTRACER_NET_PLANE)
    target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_NET_PLANE})
    # The S5 liveness engine (#1470) — see the option's own comment above.
    if(LIBTRACER_SELF_HEAL_LINKS)
        target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_SELF_HEAL_LINKS})
    endif()
    # The built-in transport-factory registrations transport_vertex_t wires up. One glue
    # TU per enabled builtin (udp/tcp/ws), plus a dispatcher (register_builtin_transports)
    # that calls exactly the enabled ones: the hand-written src/builtin_transports.cpp for
    # a FULL node (also what PlatformIO and the ESP-IDF component compile), else a
    # CMake-generated variant naming only the enabled register_*_transport. No preprocessor
    # macros — a disabled transport contributes neither its glue TU nor a call, so nothing
    # references a dropped transport (the decoupling that lets a transport TU be omitted).
    set(_libtracer_builtin_calls "")
    if(LIBTRACER_TRANSPORT_UDP)
        target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_BUILTIN_UDP})
        string(APPEND _libtracer_builtin_calls "    register_udp_transport(vertex, rx_backend, egress_src);\n")
    endif()
    if(LIBTRACER_TRANSPORT_TCP)
        target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_BUILTIN_TCP})
        string(APPEND _libtracer_builtin_calls "    register_tcp_transport(vertex, rx_backend, egress_src);\n")
    endif()
    if(LIBTRACER_TRANSPORT_WS)
        target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_BUILTIN_WS})
        string(APPEND _libtracer_builtin_calls "    register_ws_transport(vertex, rx_backend, egress_src);\n")
    endif()
    if(LIBTRACER_TRANSPORT_UDP AND LIBTRACER_TRANSPORT_TCP AND LIBTRACER_TRANSPORT_WS)
        target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_BUILTIN_DISPATCHER})
    else()
        set(LIBTRACER_BUILTIN_TRANSPORT_CALLS "${_libtracer_builtin_calls}")
        configure_file(
            ${LIBTRACER_BUILTIN_DISPATCHER_TEMPLATE}
            ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_transports.cpp
            @ONLY)
        target_sources(libtracer PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}/generated/builtin_transports.cpp)
    endif()
endif()

# Socket transports — tcp/udp/ws share posix_endpoint_t (the recv-thread/endpoint
# scaffold), compiled when ANY of them is on.
if(LIBTRACER_TRANSPORT_TCP OR LIBTRACER_TRANSPORT_UDP OR LIBTRACER_TRANSPORT_WS)
    target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_POSIX_ENDPOINT})
endif()
if(LIBTRACER_TRANSPORT_TCP)
    target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_TRANSPORT_TCP})
endif()
if(LIBTRACER_TRANSPORT_UDP)
    target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_TRANSPORT_UDP})
endif()
if(LIBTRACER_TRANSPORT_WS)
    target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_TRANSPORT_WS})
endif()

# CAN transport (portable framing/reassembly) + its can_link_t platform binding.
# socketcan_link_t — platform TU selection (no in-source #ifdefs): the Linux PF_CAN
# implementation on Linux, the always-off stub everywhere else. Platform ports (e.g.
# the ESP-IDF component's twai_link_t) implement the same can_link_t seam in their tree.
if(LIBTRACER_TRANSPORT_CAN)
    target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_TRANSPORT_CAN})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_SOCKETCAN_LINUX})
    else()
        target_sources(libtracer PRIVATE ${LIBTRACER_SOURCES_SOCKETCAN_STUB})
    endif()
endif()
# Namespaced alias so a build-tree consumer (add_subdirectory / FetchContent) links
# the SAME `libtracer::libtracer` name that `find_package(libtracer)` provides from
# the installed package — one target spelling regardless of how libtracer was pulled in.
add_library(libtracer::libtracer ALIAS libtracer)
find_package(Threads REQUIRED)
target_link_libraries(libtracer PUBLIC Threads::Threads)
target_include_directories(libtracer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(libtracer PUBLIC cxx_std_23)
# The CMake target is `libtracer`, but the artifact ships as `libtracer.a` (not the
# double-prefixed `liblibtracer.a`) so a non-CMake downstream links it with the
# conventional `-ltracer`. CMake consumers use the imported `libtracer::libtracer`
# target and are unaffected by the file name. (The ESP-IDF component archive stays
# `liblibtracer.a` — that name is fixed by the IDF component build, a separate tree.)
set_target_properties(libtracer PROPERTIES OUTPUT_NAME tracer)
# -Werror=switch (MSVC C4062) is a HARD error, not a taste warning: the library carries
# hand-written maps out of closed enums — status_t -> wire::err_t at the L4->wire cast point
# (op_resolve_walk.hpp), status_t -> its wire name (status.hpp) — and those switches are the
# only thing keeping a new enumerator from being reported as some OTHER member. None of them
# carries a `default:` label or a fall-through tail, so the enumerator that nobody mapped is a
# red build here rather than a mislabelled error code on the wire at a peer (#876). Promoted
# per switch, not blanket -Werror: a warning that has no wire consequence still must not fail
# a build on a toolchain nobody has tried yet.
if(MSVC)
    target_compile_options(libtracer PRIVATE /W4 /we4062)
else()
    target_compile_options(libtracer PRIVATE -Wall -Wextra -Werror=switch)
endif()

# ---- libtracer/config.hpp — per-target build configuration as plain C++ (ADR-0068) ----
# The defaults are C++ literals in include/libtracer/config.hpp and the build system does not
# spell them at all. A target with non-default knobs supplies a `libtracer/config_override.hpp`
# fragment earlier on the include path; config.hpp picks it up and uses the `config_t` it binds.
# One file per build ⇒ every TU agrees ⇒ no per-TU -D ODR hazard, which is the property the
# older generated-header arrangement existed to provide. Deleting the template deleted with it
# the checked-in second copy of the defaults and the configure-time drift gate that guarded the
# two against each other — a class of rot that cannot occur when there is one copy (§Erratum 1).
#
# The ACL policy and the LKV slot are traits like any other: `using acl_policy_t = ...;` and
# `using lkv_slot_t = ...;` in that fragment. Their one-release CMake options
# (-DLIBTRACER_ACL_FULL, -DLIBTRACER_LKV_SLOT) were removed in #1722 and are refused above.

# The GPU device backend used to be an option HERE, compiled into libtracer itself with a
# PUBLIC `-DLIBTRACER_WITH_CUDA` every consumer TU then carried. It is now a tier module —
# `backends/cuda/`, its own CMake project that consumes this one (#1381, docs/adr/0024
# Amendment 1) — and it plugs in through `tr::mem::register_device_backend` exactly as an
# out-of-tree transport plugs in through `register_transport_type`. Nothing in core names it,
# which is why there is no option to find here.

# libtracer_quic — the QUIC transport MODULE (docs/adr/0043 Phase A), a SEPARATE
# library target: a host that talks QUIC links libtracer_quic; a host that
# doesn't simply never compiles these sources — no feature #ifdefs, no compile
# definitions, nothing in libtracer itself references msquic. The module extends
# the transport catalog through the existing register_transport_type seam
# (quic_transport_factory), so transport_vertex stays closed for modification.
# ADR-0043 Phase B adds webtransport_transport_t to the SAME target (one msquic
# investment serves both — QUIC is the substrate WebTransport requires): the
# WebTransport-over-HTTP/3 endpoint with its module-private minimal H3/QPACK
# layer (src/wt_h3.hpp — never installed, never referenced by core).
# LIBTRACER_WITH_QUIC only controls whether the target is CONFIGURED (msquic
# must be installed to build it; the dedicated quic CI job builds msquic from
# source). Lookup is find-first: the msquic CMake package config if installed,
# else a plain header+library search (point CMAKE_PREFIX_PATH at an msquic
# install prefix).
option(LIBTRACER_WITH_QUIC "Configure the libtracer_quic transport module (needs msquic)" OFF)
if(LIBTRACER_WITH_QUIC)
    find_package(msquic CONFIG QUIET)
    if(NOT TARGET msquic)
        find_library(MSQUIC_LIBRARY NAMES msquic)
        if(NOT MSQUIC_LIBRARY)
            message(FATAL_ERROR "LIBTRACER_WITH_QUIC=ON but msquic was not found. "
                "Install msquic (https://github.com/microsoft/msquic) and/or set "
                "CMAKE_PREFIX_PATH to its install prefix.")
        endif()
        add_library(msquic SHARED IMPORTED)
        set_target_properties(msquic PROPERTIES IMPORTED_LOCATION "${MSQUIC_LIBRARY}")
    endif()
    # msquic's exported target (as of v2.4) carries no include-dir usage
    # requirement — locate msquic.h ourselves and attach it (PUBLIC: consumers
    # like quic_test include msquic.h directly for their raw test client).
    find_path(MSQUIC_INCLUDE_DIR msquic.h)
    if(NOT MSQUIC_INCLUDE_DIR)
        message(FATAL_ERROR "msquic.h not found — set CMAKE_PREFIX_PATH to the msquic install prefix.")
    endif()
    add_library(libtracer_quic STATIC ${LIBTRACER_SOURCES_QUIC})
    target_include_directories(libtracer_quic PUBLIC "${MSQUIC_INCLUDE_DIR}")
    target_link_libraries(libtracer_quic PUBLIC libtracer msquic)
    if(MSVC)
        target_compile_options(libtracer_quic PRIVATE /W4 /we4062)
    else()
        target_compile_options(libtracer_quic PRIVATE -Wall -Wextra -Werror=switch)
    endif()
endif()

# The security_noise module's crypto backend (RFC-0033, #2072): one compile-time choice, no
# runtime switch. `none` (the default) configures nothing, so a build without the Noise link
# compiles none of it and links no crypto library. Any other value makes the header-only
# INTERFACE target `libtracer_noise`, which carries the backend library and the
# `LIBTRACER_NOISE_CRYPTO_<BACKEND>` define that `security_noise.hpp` aliases as
# `tr::net::noise::default_crypto_t`. The core library itself stays crypto-free (ADR-0045).
#   openssl  libcrypto 3 (find_package(OpenSSL))
#   sodium   libsodium (sodium.h + libsodium)
#   psa      mbedTLS through PSA: an existing MbedTLS::tfpsacrypto / MbedTLS::mbedcrypto
#            target, else find_package(MbedTLS), else psa/crypto.h + libtfpsacrypto or
#            libmbedcrypto on the search path. Build it with MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS
#            (see security_noise_psa.hpp).
set(LIBTRACER_NOISE_CRYPTO "none" CACHE STRING
    "Crypto backend of the Noise link module (none, openssl, sodium, psa)")
set_property(CACHE LIBTRACER_NOISE_CRYPTO PROPERTY STRINGS none openssl sodium psa)
if(NOT LIBTRACER_NOISE_CRYPTO STREQUAL "none")
    add_library(libtracer_noise INTERFACE)
    target_include_directories(libtracer_noise INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
    target_compile_features(libtracer_noise INTERFACE cxx_std_23)
    if(LIBTRACER_NOISE_CRYPTO STREQUAL "openssl")
        find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
        target_link_libraries(libtracer_noise INTERFACE OpenSSL::Crypto)
        target_compile_definitions(libtracer_noise INTERFACE LIBTRACER_NOISE_CRYPTO_OPENSSL=1)
    elseif(LIBTRACER_NOISE_CRYPTO STREQUAL "sodium")
        find_path(LIBTRACER_SODIUM_INCLUDE_DIR sodium.h)
        find_library(LIBTRACER_SODIUM_LIBRARY NAMES sodium libsodium)
        if(NOT LIBTRACER_SODIUM_INCLUDE_DIR OR NOT LIBTRACER_SODIUM_LIBRARY)
            message(FATAL_ERROR "LIBTRACER_NOISE_CRYPTO=sodium but libsodium was not found "
                "(install libsodium-dev, or set CMAKE_PREFIX_PATH).")
        endif()
        target_include_directories(libtracer_noise SYSTEM INTERFACE "${LIBTRACER_SODIUM_INCLUDE_DIR}")
        target_link_libraries(libtracer_noise INTERFACE "${LIBTRACER_SODIUM_LIBRARY}")
        target_compile_definitions(libtracer_noise INTERFACE LIBTRACER_NOISE_CRYPTO_SODIUM=1)
    elseif(LIBTRACER_NOISE_CRYPTO STREQUAL "psa")
        if(NOT TARGET MbedTLS::tfpsacrypto AND NOT TARGET MbedTLS::mbedcrypto)
            find_package(MbedTLS CONFIG QUIET)
        endif()
        if(TARGET MbedTLS::tfpsacrypto)
            target_link_libraries(libtracer_noise INTERFACE MbedTLS::tfpsacrypto)
        elseif(TARGET MbedTLS::mbedcrypto)
            target_link_libraries(libtracer_noise INTERFACE MbedTLS::mbedcrypto)
        else()
            find_path(LIBTRACER_PSA_INCLUDE_DIR psa/crypto.h)
            find_library(LIBTRACER_PSA_LIBRARY NAMES tfpsacrypto mbedcrypto)
            if(NOT LIBTRACER_PSA_INCLUDE_DIR OR NOT LIBTRACER_PSA_LIBRARY)
                message(FATAL_ERROR "LIBTRACER_NOISE_CRYPTO=psa but no PSA Crypto library was "
                    "found (an MbedTLS CMake package, or psa/crypto.h and libmbedcrypto; set "
                    "CMAKE_PREFIX_PATH).")
            endif()
            target_include_directories(libtracer_noise SYSTEM INTERFACE "${LIBTRACER_PSA_INCLUDE_DIR}")
            target_link_libraries(libtracer_noise INTERFACE "${LIBTRACER_PSA_LIBRARY}")
        endif()
        target_compile_definitions(libtracer_noise INTERFACE LIBTRACER_NOISE_CRYPTO_PSA=1)
    else()
        message(FATAL_ERROR "LIBTRACER_NOISE_CRYPTO='${LIBTRACER_NOISE_CRYPTO}': expected one of "
            "none, openssl, sodium, psa.")
    endif()
    message(STATUS "libtracer: Noise link crypto backend = ${LIBTRACER_NOISE_CRYPTO}")
endif()

# Install / export: make libtracer consumable with `find_package(libtracer)` and
# `target_link_libraries(app PRIVATE libtracer::libtracer)` — the release-hardening
# packaging seam (before this, a downstream could only source-vendor the tree).
# Scope: the default-build core static library only. The optional libtracer_quic
# module is intentionally NOT part of the installed package yet — it links an
# imported msquic target whose provenance (CMake package vs. bare library search)
# can't be faithfully reconstructed in an exported config; packaging it cleanly is
# its own follow-up. Gated on LIBTRACER_INSTALL (default: only when top-level), so
# an embedding parent (bench/, esp-idf) that add_subdirectory()s core never
# inherits install rules it didn't ask for.
option(LIBTRACER_INSTALL "Generate install/export rules for libtracer" ${PROJECT_IS_TOP_LEVEL})
if(LIBTRACER_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    install(TARGETS libtracer
        EXPORT libtracerTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(DIRECTORY include/libtracer
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    # config.hpp itself is the checked-in default the directory copy above brought along. The
    # build no longer generates an override fragment (#1722), so an embedder that configures a
    # non-default package ships its own libtracer/config_override.hpp beside it (ADR-0068).

    set(LIBTRACER_CMAKE_CONFIG_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/libtracer)

    install(EXPORT libtracerTargets
        FILE libtracerTargets.cmake
        NAMESPACE libtracer::
        DESTINATION ${LIBTRACER_CMAKE_CONFIG_DIR})

    # Dependencies the exported config must re-find. Threads is unconditional, and it is now
    # the only one: with the GPU backend moved to the `backends/` tier (#1381) core's export
    # carries no vendor usage requirement to reconstruct — the tier links its own toolkit.
    set(LIBTRACER_PACKAGE_DEPENDENCIES "find_dependency(Threads)")

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/libtracerConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/libtracerConfig.cmake
        INSTALL_DESTINATION ${LIBTRACER_CMAKE_CONFIG_DIR})

    # Pre-1.0: a 0.x minor bump may break the C++ API (per CHANGELOG), so match on
    # minor, not major — a consumer that asks for 0.3 won't silently accept 0.4.
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/libtracerConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMinorVersion)

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/libtracerConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/libtracerConfigVersion.cmake
        DESTINATION ${LIBTRACER_CMAKE_CONFIG_DIR})
endif()

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    enable_testing()
    # The test preset (#1664, #1670): the test build binds config_t::kInstrumentCounters so the
    # ancestor-walk / canonical-fallback ablations can read their counters, and opts in to the two
    # link modules (kBusLinks, kSelfHealLinks) whose TUs the options above compile. Listed FIRST, so it
    # also sits ahead of a fragment the build supplies — and yields to that one by
    # `#include_next` (see the fragment), never shadowing the configuration a CI leg tests.
    target_include_directories(libtracer BEFORE PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tests/instrumented>)
    add_subdirectory(tests)
endif()

# M3: the in-process pub/sub example (the P0 node end to end). Built by default
# when libtracer is the top-level project; runs as a smoke test under CTest.
option(LIBTRACER_BUILD_EXAMPLES "Build the core examples" ${PROJECT_IS_TOP_LEVEL})
if(LIBTRACER_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()
