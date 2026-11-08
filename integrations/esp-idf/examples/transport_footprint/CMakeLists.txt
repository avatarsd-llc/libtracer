# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Footprint-only ESP-IDF project (#2678). It links the two transports full_node never
# links, CAN (can_transport_t over the TWAI link) and WebSocket (the httpd server link and
# the esp_websocket client link), so tools/esp_size_gate.py can pin their flash rows. It
# exists for the linker: CI builds it and reads its map, and nothing runs it.

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../libtracer")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(transport_footprint)
