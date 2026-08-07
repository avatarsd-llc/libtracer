/**
 * @file
 * @brief Host stand-in for ESP-IDF's `driver/gpio.h` — the pin type only.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `twai_link.cpp` names this header for exactly one reason: to spell its TX/RX
 * pins as `gpio_num_t` when filling the node config. IDF declares that as an
 * enum of every pad on the target; an int typedef takes the same `static_cast`
 * the TU already writes and needs no per-chip pad table. Nothing on the host
 * configures a pad, so there is no behaviour to reproduce.
 *
 * The C spellings are IDF's, per the sibling fake_idf headers.
 */
#pragma once

/** @brief A GPIO pad number (IDF: an enum; here the integer it is). */
typedef int gpio_num_t;
