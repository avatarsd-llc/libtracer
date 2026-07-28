/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * mem_source_sync — the HOSTED synchronization policy for tr::mem::pool_source_t.
 */
#pragma once

#include <mutex>

/**
 * @file
 * @brief A `std::mutex`-backed synchronization policy for @ref tr::mem::pool_source_t.
 *
 * Deliberately a separate header. `mem_source.hpp` is compiled into the freestanding
 * footprint sentinel, where `<mutex>` is not available; putting the only threading
 * facility this seam offers behind its own include keeps that build honest instead of
 * making every consumer of the L0 seam pay for a hosted dependency.
 */

namespace tr::mem {

/**
 * @brief The hosted synchronization policy — a plain `std::mutex`.
 *
 * For a source shared across threads at **wiring frequency**: a graph's control source,
 * where registration runs once per connection and an uncontended mutex is unmeasurable.
 *
 * @warning Do NOT reach for this to make a per-frame source thread-safe. ADR-0060
 *          erratum 1 measured a shared free-list pool collapsing to ~1/15 of its
 *          single-thread rate on a 12-core host while the platform heap scaled; guarding
 *          the shared list is the problem, not the guard's flavour. Give each receiver its
 *          own @ref pool_source_t with the default @ref sync_none_t instead.
 * @note Also not for a single-core FreeRTOS target's per-frame path: a blocking mutex
 *       there invites the priority inversion ADR-0063 erratum 1 records. Such a target
 *       supplies an interrupt-disable policy of its own; the seam only asks for
 *       `lock()`/`unlock()`.
 */
class sync_mutex_t {
   public:
    /** @brief Acquire. `noexcept` by policy: a mutex that cannot lock is a programming error. */
    void lock() noexcept { m_.lock(); }
    /** @brief Release. */
    void unlock() noexcept { m_.unlock(); }

   private:
    std::mutex m_; /**< @brief The lock itself; uncontended at wiring frequency. */
};

}  // namespace tr::mem
