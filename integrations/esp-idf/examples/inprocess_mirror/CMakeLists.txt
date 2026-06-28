# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# ESP-IDF project that builds the libtracer P0 (in-process) component for a
# real target (esp32c6 by default) and exercises the in-process mirror surface
# (register / write / read / await). This is the on-silicon build gate for the
# integrations/esp-idf component — see .github/workflows/esp-idf.yml.

cmake_minimum_required(VERSION 3.16)

# Pull in the in-tree libtracer component (integrations/esp-idf/libtracer/). A
# single-component directory is registered under its basename, so this makes the
# component resolvable as `libtracer` (what main/ does `REQUIRES libtracer` on).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../libtracer")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(inprocess_mirror)
