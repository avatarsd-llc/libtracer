# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
#
# Platform TU selection lives HERE (no in-source #ifdefs): app_main.cpp is the
# shared, portable node; the platform bring-up (Wi-Fi station on chips, a no-op
# on the linux host target) is a per-target translation unit behind the
# platform.hpp seam.

if(IDF_TARGET STREQUAL "linux")
    set(FULL_NODE_PLATFORM_SRC "platform_linux.cpp")
    set(FULL_NODE_PRIV_REQUIRES "")
else()
    set(FULL_NODE_PLATFORM_SRC "platform_esp.cpp")
    set(FULL_NODE_PRIV_REQUIRES nvs_flash esp_wifi esp_netif esp_event)
endif()

idf_component_register(
    SRCS "app_main.cpp" "${FULL_NODE_PLATFORM_SRC}"
    INCLUDE_DIRS "."
    REQUIRES libtracer
    PRIV_REQUIRES ${FULL_NODE_PRIV_REQUIRES}
)

# app_main.cpp includes the C++23 core headers (std::expected, std::span). The
# libtracer component already requests cxx_std_23 PUBLICly, but pin it here too
# so this app is correct regardless of the project's default C++ standard.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)
