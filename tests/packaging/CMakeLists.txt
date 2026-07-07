# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Downstream-consumer fixture for the install/export regression guard (core-ci
# `install-consume` job). This is NOT part of the core build — it is configured
# SEPARATELY against an installed libtracer prefix (CMAKE_PREFIX_PATH), so it
# proves the real thing a downstream does: find_package + link the imported
# target + include a public header + call a compiled symbol. If the install
# rules, package config, or exported target ever break, this fails to configure
# or link.
cmake_minimum_required(VERSION 3.16)
project(libtracer_packaging_consumer LANGUAGES CXX)

find_package(libtracer 0.3 REQUIRED)

add_executable(app main.cpp)
target_link_libraries(app PRIVATE libtracer::libtracer)
