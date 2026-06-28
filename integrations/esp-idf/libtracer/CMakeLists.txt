# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# ESP-IDF component wrapper for the libtracer C++ reference core.
#
# The sources live in the repo's core/; this component pulls them in so an
# ESP-IDF project can depend on libtracer through the component manager. It
# packages the P0 (in-process) core — the wire codec, the L0/L1 substrate, and
# the L4 graph runtime — which is what an in-process mirror (e.g. strawberry-fw
# migration Phase 1) needs. Transport modules (transport_ws, #54; transport_can,
# #55) are added in later phases and bring their own REQUIRES (lwip, etc.).
#
# Requires the GCC 13 toolchain (C++23: std::expected, std::span) shipped with
# ESP-IDF >= 5.3.

# This component dir is integrations/esp-idf/libtracer/; the repo root (which
# holds core/) is three levels up. An ESP-IDF project consumes this component in
# place by pointing `EXTRA_COMPONENT_DIRS` at it (see the bundled examples'
# top-level CMakeLists.txt) — so this relative path resolves against the real
# checkout and core/ is reachable.
set(LIBTRACER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

idf_component_register(
    SRCS
        "${LIBTRACER_ROOT}/core/src/frame.cpp"
        "${LIBTRACER_ROOT}/core/src/mem_heap.cpp"
        "${LIBTRACER_ROOT}/core/src/mem_pool.cpp"
        "${LIBTRACER_ROOT}/core/src/rope.cpp"
        "${LIBTRACER_ROOT}/core/src/path.cpp"
        "${LIBTRACER_ROOT}/core/src/graph.cpp"
    INCLUDE_DIRS
        "${LIBTRACER_ROOT}/core/include"
    PRIV_REQUIRES
        pthread
)

# pthread is a PRIVATE requirement, not a public (REQUIRES) one. libtracer's
# *public* headers expose only libstdc++ headers (<atomic>, <mutex>,
# <shared_mutex>, <thread>) — never ESP-IDF's <pthread.h> — so the component
# does not propagate a pthread dependency to its dependents. The pthread symbols
# (pthread_create, etc.) are pulled in by libstdc++'s gthread threading, which
# the *.cpp objects need at link time; that's exactly what PRIV_REQUIRES models.
# (The old `REQUIRES pthread` over-declared it as a public dependency.)

# libtracer's core is C++23. ESP-IDF compiles component sources with the project's
# C++ standard; pin it here so the component is correct regardless of project default.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)
