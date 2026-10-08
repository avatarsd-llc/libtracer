# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# One-concept ESP-IDF example (#1786): see docs/examples/esp-idf-one-link.md.

cmake_minimum_required(VERSION 3.16)

# The in-tree libtracer component, registered as `libtracer` (main/ REQUIRES it).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../libtracer")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(one_link)
