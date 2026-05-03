# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 Avatar LLC

cmake_minimum_required(VERSION 3.16)
project(libtracer CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(libtracer INTERFACE)
target_include_directories(libtracer INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(libtracer INTERFACE cxx_std_17)

if(PROJECT_IS_TOP_LEVEL AND BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
