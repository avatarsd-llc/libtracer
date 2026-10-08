# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC

idf_component_register(SRCS "app_main.cpp" INCLUDE_DIRS "." REQUIRES libtracer)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_23)
