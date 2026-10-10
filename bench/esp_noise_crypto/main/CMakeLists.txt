# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "." "../.."
    # Wi-Fi's components are required by every build so the four images differ only in what
    # Kconfig turns on; nothing of Wi-Fi is linked unless NOISE_BENCH_WITH_WIFI calls it.
    PRIV_REQUIRES mbedtls esp_timer esp_wifi esp_netif esp_event nvs_flash
)
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_23)
if(CONFIG_NOISE_BENCH_BACKEND_PSA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIBTRACER_BENCH_HAVE_PSA=1)
endif()
