/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The per-target platform seam of the full_node example. app_main.cpp is a
 * single portable TU; which implementation of these two functions links in is
 * a BUILD-SYSTEM decision (main/CMakeLists.txt picks platform_esp.cpp on chip
 * targets, platform_linux.cpp on the ESP-IDF `linux` host target) — never an
 * in-source #ifdef.
 *
 * This is example code, not core: plain comments, no Doxygen.
 */
#pragma once

// Bring the platform up (chips: NVS + netif + Wi-Fi station from menuconfig,
// skipped when no SSID is configured; linux host: a no-op). Returns false only
// on an unrecoverable bring-up error.
bool platform_bring_up();

// True on a real device (chip target): after the self-proof the node parks in
// the publish loop, serving peers forever. False on the linux host target:
// the app exits with the self-proof's status (the CI gate).
bool platform_is_device();
