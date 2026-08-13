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
 *
 * Since #1184 it also models the PERIODIC timer object, because the WS link arms one to
 * sweep sessions that have not authenticated. What is modelled is the object's lifetime —
 * create / start / stop / delete, and whether a callback is armed — and NOT the passage of
 * time: a host test fires the sweep itself with @ref fake_esp_timer_fire, which makes the
 * deadline a decision the test controls rather than a wall-clock race it has to sleep for.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
#include <chrono>
#include <cstddef>
#include <mutex>
#include <vector>

/** @brief Microseconds on a monotonic clock — the host stand-in for IDF's timer read. */
inline int64_t esp_timer_get_time(void) {
    return static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

/** @brief IDF's `esp_timer_cb_t` — the callback a timer fires, with its registered arg. */
using esp_timer_cb_t = void (*)(void*);

/** @brief One fake timer; IDF keeps the type opaque and so does every consumer here. */
struct esp_timer_fake_t {
    esp_timer_cb_t callback = nullptr; /**< @brief What @ref fake_esp_timer_fire invokes. */
    void* arg = nullptr;               /**< @brief The registered opaque argument. */
    uint64_t period_us = 0;            /**< @brief From `esp_timer_start_periodic`. */
    bool running = false;              /**< @brief True between start and stop. */
};

/** @brief IDF's opaque handle — a pointer to one of the above. */
using esp_timer_handle_t = esp_timer_fake_t*;

/** @brief IDF's `esp_timer_create_args_t`, trimmed to the fields the links set. */
struct esp_timer_create_args_t {
    esp_timer_cb_t callback = nullptr;  /**< @brief Fired on each expiry. */
    void* arg = nullptr;                /**< @brief Passed through to @p callback. */
    int dispatch_method = 0;            /**< @brief `ESP_TIMER_TASK` on target; unused here. */
    const char* name = nullptr;         /**< @brief Debug name. */
    bool skip_unhandled_events = false; /**< @brief Unused here. */
};

/** @brief `ESP_TIMER_TASK` — the only dispatch method the links ask for. */
#define ESP_TIMER_TASK 0

/**
 * @brief The fake's timer table, plus the one-shot create-failure latch.
 *
 * Header-inline (a function-local static) rather than a translation unit of its own, so that
 * adding the timer to the fake needs no change to the eight test targets that already link
 * the chip TU — the same shape `esp_timer_get_time` above has always had.
 */
struct fake_esp_timer_state_t {
    std::mutex m;                          /**< @brief Guards both members. */
    std::vector<esp_timer_fake_t*> timers; /**< @brief Live timers, in creation order. */
    bool fail_next_create = false;         /**< @brief Consumed by the next create. */
};

/** @brief The one timer table. */
inline fake_esp_timer_state_t& fake_esp_timer_state() {
    static fake_esp_timer_state_t state;
    return state;
}

/** @brief Allocate a timer and record its callback (`ESP_ERR_INVALID_ARG` on a null out,
 *         `ESP_FAIL` when @ref fake_esp_timer_fail_next_create armed a refusal). */
inline esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out) {
    if (args == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;
    fake_esp_timer_state_t& state = fake_esp_timer_state();
    const std::lock_guard<std::mutex> lock(state.m);
    if (state.fail_next_create) {
        state.fail_next_create = false;
        return ESP_FAIL;
    }
    auto* const timer = new esp_timer_fake_t{args->callback, args->arg, 0, false};
    state.timers.push_back(timer);
    *out = timer;
    return ESP_OK;
}

/** @brief Arm @p timer at @p period_us — recorded, never fired on its own; the test drives
 *         expiry through @ref fake_esp_timer_fire. */
inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    const std::lock_guard<std::mutex> lock(fake_esp_timer_state().m);
    timer->period_us = period_us;
    timer->running = true;
    return ESP_OK;
}

/** @brief Disarm @p timer; firing a stopped timer is a no-op, as on the target. */
inline esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    const std::lock_guard<std::mutex> lock(fake_esp_timer_state().m);
    timer->running = false;
    return ESP_OK;
}

/** @brief Release @p timer's slot. */
inline esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
    if (timer == nullptr) return ESP_ERR_INVALID_ARG;
    fake_esp_timer_state_t& state = fake_esp_timer_state();
    const std::lock_guard<std::mutex> lock(state.m);
    for (std::size_t i = 0; i < state.timers.size(); ++i)
        if (state.timers[i] == timer) {
            state.timers.erase(state.timers.begin() + static_cast<std::ptrdiff_t>(i));
            delete timer;
            return ESP_OK;
        }
    return ESP_ERR_INVALID_ARG;
}

/**
 * @brief TEST LEVER: invoke every RUNNING timer's callback once, synchronously, on the
 *        CALLING thread; returns how many ran.
 *
 * The stand-in for the esp_timer task, and the reason a deadline test needs no sleeping: the
 * test decides when a tick happens. Calling it from the test thread — never from inside the
 * fake httpd task's drain — reproduces the split the link is written against, where the
 * callback runs somewhere other than the httpd task and must marshal its work over.
 *
 * The callback list is COPIED before the calls so a callback that deletes a timer (or
 * creates one) cannot invalidate the iteration.
 */
inline std::size_t fake_esp_timer_fire() {
    std::vector<esp_timer_fake_t> armed;
    {
        fake_esp_timer_state_t& state = fake_esp_timer_state();
        const std::lock_guard<std::mutex> lock(state.m);
        for (esp_timer_fake_t* const t : state.timers)
            if (t->running && t->callback != nullptr) armed.push_back(*t);
    }
    for (const esp_timer_fake_t& t : armed) t.callback(t.arg);
    return armed.size();
}

/** @brief How many timers are currently allocated — the leak check a teardown test makes. */
inline std::size_t fake_esp_timer_live() {
    fake_esp_timer_state_t& state = fake_esp_timer_state();
    const std::lock_guard<std::mutex> lock(state.m);
    return state.timers.size();
}

/** @brief Make the next @ref esp_timer_create fail, once — the unarmable-sweep path. */
inline void fake_esp_timer_fail_next_create() {
    fake_esp_timer_state_t& state = fake_esp_timer_state();
    const std::lock_guard<std::mutex> lock(state.m);
    state.fail_next_create = true;
}
#endif
