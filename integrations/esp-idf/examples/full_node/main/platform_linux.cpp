/**
 * @file
 * @brief Platform seam, ESP-IDF `linux` (POSIX host) target: nothing to bring
 *        up — the host kernel's loopback carries the datagrams.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Selected by the build system (main/CMakeLists.txt); chip targets link
 * platform_esp.cpp instead.
 */

#include "platform.hpp"

bool platform_bring_up() { return true; }

bool platform_is_device() { return false; }
