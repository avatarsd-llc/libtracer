# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

set(LIBTRACER_VECTORS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/conformance/vectors/v1")

function(libtracer_test_warnings tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W4)
    else()
        target_compile_options(${tgt} PRIVATE -Wall -Wextra)
    endif()
endfunction()

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

# M3 — L4 graph-runtime harness (single-threaded checks + multithreaded stress).
add_executable(graph_test graph_test.cpp)
target_link_libraries(graph_test PRIVATE libtracer)
find_package(Threads REQUIRED)
target_link_libraries(graph_test PRIVATE Threads::Threads)
libtracer_test_warnings(graph_test)
target_compile_definitions(graph_test PRIVATE
    LIBTRACER_VECTORS_DIR="${LIBTRACER_VECTORS_DIR}")
add_test(NAME graph COMMAND graph_test)

# M2 — substrate harness in single-threaded mode. LIBTRACER_NO_ATOMIC changes the
# Segment refcount layout, so this variant compiles the library sources directly
# (rather than linking the atomic-built libtracer.a) to keep one consistent ABI.
add_executable(substrate_test_no_atomic
    substrate_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/frame.cpp
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
