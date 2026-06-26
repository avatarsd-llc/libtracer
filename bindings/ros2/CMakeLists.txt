# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# ament package for rmw_tracer — a ROS 2 middleware backed by libtracer.
# Build with colcon (needs a ROS 2 distro on the AMENT_PREFIX_PATH); NOT built by
# core/'s CMake. See README.md and ADR-0023.
cmake_minimum_required(VERSION 3.16)
project(rmw_tracer C CXX)

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 23)
endif()

find_package(ament_cmake REQUIRED)
find_package(rmw REQUIRED)
find_package(rcutils REQUIRED)
find_package(rcpputils REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_introspection_c REQUIRED)
find_package(rosidl_typesupport_introspection_cpp REQUIRED)

# The libtracer reference core: built from ../../core (or found if installed).
# Kept optional at configure time so `cmake -n`/linting works without it; the
# build target requires it.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../core ${CMAKE_BINARY_DIR}/libtracer EXCLUDE_FROM_ALL)

# The RMW translation units (to be written against the distro's rmw headers, one
# TU per concern — init/node, publisher, subscription, wait, qos, graph, service).
file(GLOB RMW_TRACER_SRC CONFIGURE_DEPENDS src/rmw_tracer/*.c src/rmw_tracer/*.cpp)

add_library(rmw_tracer ${RMW_TRACER_SRC})
target_include_directories(rmw_tracer PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")
# Plain signature (no PUBLIC keyword) to match ament_target_dependencies, which
# uses the plain target_link_libraries internally — CMake forbids mixing the two.
target_link_libraries(rmw_tracer libtracer)
ament_target_dependencies(rmw_tracer
  rmw rcutils rcpputils rosidl_runtime_c
  rosidl_typesupport_introspection_c rosidl_typesupport_introspection_cpp)

# Register as an RMW implementation so RMW_IMPLEMENTATION=rmw_tracer resolves it.
ament_export_libraries(rmw_tracer)
ament_export_dependencies(rmw rcutils rosidl_runtime_c)

install(TARGETS rmw_tracer
  ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)

ament_package()
