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
# Each object's static frames and call edges (*.su, *.ci), for tools/esp_stack_gate.py's
# WebSocket client rx stack rows (#2871). Neither option changes the generated code, so the
# size rows read from this image's map stay what they were.
idf_build_set_property(COMPILE_OPTIONS "-fstack-usage;-fcallgraph-info=su" APPEND)
project(transport_footprint)
