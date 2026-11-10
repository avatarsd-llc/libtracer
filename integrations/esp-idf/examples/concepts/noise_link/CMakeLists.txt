# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# One-concept ESP-IDF example (#2076): see docs/examples/esp-idf-noise-link.md.

cmake_minimum_required(VERSION 3.16)

# The in-tree libtracer component, registered as `libtracer` (main/ REQUIRES it).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../../libtracer")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# mbedTLS's PSA key-slot table is sized by the application, since every PSA user in the image
# shares it (CONFIG_LIBTRACER_NOISE_CRYPTO_PSA's help). The sizes live in main/psa_slots.h.
idf_build_set_property(COMPILE_DEFINITIONS
    "MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_LIST_DIR}/main/psa_slots.h\"" APPEND)

project(noise_link)
