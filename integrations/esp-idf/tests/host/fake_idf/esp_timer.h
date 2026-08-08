/**
 * @file
 * @brief Host stand-in for ESP-IDF's `esp_timer.h` — the monotonic microsecond clock.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The links use exactly one thing from this header: `esp_timer_get_time()`, the
 * microseconds-since-boot stamp that dates a connection's `connected_at_us` and its
 * `last_rx_us`. On the target that is a hardware timer read; here it is
 * `steady_clock`, which shares the two properties the counters actually depend on —
 * monotonic, and an ORIGIN that is not the wall clock (so a host with a stepping
 * clock cannot make a duration go backwards).
 *
 * The origin is deliberately NOT the process start: nothing in the links interprets
 * the absolute value, only differences and the -1 "never" sentinel, and pinning the
 * origin would invite a test to assert on an absolute stamp that means nothing on
 * the chip.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <chrono>

/** @brief Microseconds on a monotonic clock — the host stand-in for IDF's timer read. */
inline int64_t esp_timer_get_time(void) {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}
#endif
