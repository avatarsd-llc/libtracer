# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

add_executable(conformance_runner conformance_runner.cpp)
target_link_libraries(conformance_runner PRIVATE libtracer)
if(MSVC)
    target_compile_options(conformance_runner PRIVATE /W4)
else()
    target_compile_options(conformance_runner PRIVATE -Wall -Wextra)
endif()
target_compile_definitions(conformance_runner PRIVATE
    LIBTRACER_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/conformance/vectors/v1")

add_test(NAME conformance COMMAND conformance_runner)
