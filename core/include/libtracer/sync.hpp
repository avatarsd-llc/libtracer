/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * sync — the build's blocking primitives: mutex, shared mutex, condition variable.
 */
#pragma once

#include <condition_variable>
#include <mutex>
#include <shared_mutex>

#include "libtracer/config.hpp"

/**
 * @file
 * @brief `tr::mutex_t`, `tr::shared_mutex_t` and `tr::condvar_t`: the blocking primitives
 *        core sleeps on, bound at compile time by `config_t::sync_t` (#2454).
 *
 * Every lock core holds across work that can block, and every condition it waits on, is one
 * of these three; core spells no `std::mutex` of its own. The host binding is the standard
 * library's, unchanged. An ESP-IDF chip binds `tr::esp::static_sync_t`, whose primitives hold
 * their FreeRTOS semaphore inline: the standard ones there are pthread objects whose semaphore
 * the IDF pthread layer takes from the platform heap, which the no-heap census cannot see from
 * inside the archive and therefore refuses by name (`tools/check_no_heap.py`).
 *
 * This is a header of its own, apart from `%config.hpp`: a freestanding build compiles that one
 * without `<mutex>`, so it may only forward-declare the default (see `default_config_t::sync_t`).
 */

namespace tr {

/** @brief The host's blocking primitives: the standard library's, as core always used. */
struct std_sync_t {
    using mutex_t = std::mutex;                /**< @brief The blocking lock. */
    using shared_mutex_t = std::shared_mutex;  /**< @brief Shared readers, one writer. */
    using condvar_t = std::condition_variable; /**< @brief Waits on a `unique_lock<mutex_t>`. */
};

/** @brief This build's blocking lock (`config_t::sync_t::mutex_t`). */
using mutex_t = graph::config_t::sync_t::mutex_t;
/** @brief This build's reader/writer lock (`config_t::sync_t::shared_mutex_t`). */
using shared_mutex_t = graph::config_t::sync_t::shared_mutex_t;
/** @brief This build's condition variable, waited on with a `std::unique_lock<tr::mutex_t>`. */
using condvar_t = graph::config_t::sync_t::condvar_t;

}  // namespace tr
