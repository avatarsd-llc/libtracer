/**
 * @file
 * @brief Host stand-in for ESP-IDF's `port/include/mbedtls/ecp.h` (#2065).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * ESP-IDF's mbedTLS 4.x fork includes this public header, which its port directory supplies
 * (adding the hardware-ECC hooks). Building that tree on the host for bench_noise_crypto needs
 * only the private declarations it forwards to.
 */
#pragma once
#include "mbedtls/private/ecp.h"
