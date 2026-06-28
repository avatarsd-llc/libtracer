# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES libtracer
)

# app_main.cpp includes the C++23 core headers (std::expected, std::span). The
# libtracer component already requests cxx_std_23 PUBLICly, but pin it here too
# so this app is correct regardless of the project's default C++ standard.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)
