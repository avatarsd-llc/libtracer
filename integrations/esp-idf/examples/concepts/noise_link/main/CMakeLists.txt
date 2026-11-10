# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

# A chip target starts lwIP through esp_netif; the `linux` target has host sockets and no netif.
# mbedtls is named here for the `linux` target, where the component does not pull it in.
set(requires libtracer mbedtls)
if(NOT IDF_TARGET STREQUAL "linux")
    list(APPEND requires esp_netif)
endif()
idf_component_register(SRCS "app_main.cpp" INCLUDE_DIRS "." REQUIRES ${requires})
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)
