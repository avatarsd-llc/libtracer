# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# ESP-IDF project that builds the libtracer FULL-NODE profile (#183) — graph +
# fwd_router + transport_vertex + config-created UDP sockets — in the
# origin-firmware shape (the originating production firmware's
# libtracer-migration plan Phase 2): a sensor vertex, a
# `/net` listener created via a `:children[]` SPEC, and a remote subscriber
# fanning out to a peer node over REAL datagrams.
#
# Builds for the chip targets (esp32c6 / esp32c3 — the CI on-silicon gate) and
# for the ESP-IDF `linux` (POSIX host) target, where CI also RUNS it: the app
# then brings up a second, in-process "host peer" node that dials the device
# node over 127.0.0.1 and drives read / subscribe / await / fan-out end to end.

cmake_minimum_required(VERSION 3.16)

# Pull in the in-tree libtracer component (integrations/esp-idf/libtracer/). A
# single-component directory is registered under its basename, so this makes the
# component resolvable as `libtracer` (what main/ does `REQUIRES libtracer` on).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../libtracer")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(full_node)
