/**
 * @file
 * @brief A process-wide `malloc`-level heap counter for the Noise link harness (#2065): it sees
 *        what C libraries allocate, which an `operator new` counter (`heap_probe.hpp`) cannot.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `malloc_probe.cpp` defines `malloc`, `calloc`, `realloc`, `free` and the aligned forms in the
 * executable, forwarding to glibc's `__libc_*` entry points. A definition in the executable
 * interposes for every shared library too, so libsodium's, OpenSSL's and mbedTLS's heap traffic
 * is counted along with the C++ runtime's (`operator new` calls `malloc`). Counting is armed per
 * window and is process-global, so a caller arms it around single-threaded work, or reads it
 * knowing every thread is included. glibc only; on another C library the interposer is not
 * built and every reading is zero (`kAvailable` says which).
 */
#pragma once

#include <cstddef>

namespace bench::malloc_probe {

/** @brief One window's reading. */
struct reading_t {
    long long live = 0;   /**< @brief Usable bytes allocated minus freed while armed. */
    long long peak = 0;   /**< @brief High-water of @ref live within the window. */
    long long allocs = 0; /**< @brief Allocation calls while armed. */
    long long frees = 0;  /**< @brief Free calls while armed. */
};

/** @brief True when the interposer is compiled in (glibc). */
extern const bool kAvailable;

/** @brief Zero the counters and start counting. */
void arm();

/** @brief Stop counting and return the window's reading. */
reading_t disarm();

}  // namespace bench::malloc_probe
