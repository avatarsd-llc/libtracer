/**
 * @file
 * @brief `tr::detail::thread_id_t` — the current execution context's identity, on every
 *        supported target INCLUDING a bare RTOS task
 * ([#1532](https://github.com/avatarsd-llc/libtracer/issues/1532)).
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Two ownership self-checks in this tree stamp "which thread is inside this critical section"
 * and compare it later: `transport_vertex_t`'s RFC-0014 S6 control-plane seam, and
 * `self_heal_link_t`'s worker re-entrancy guard. Both need an identity that **exists** wherever
 * the code can run, and `std::this_thread::get_id()` is not that identity.
 *
 * @section why Why not `std::this_thread::get_id()`
 *
 * It lowers to `__gthread_self()` → `pthread_self()`. ESP-IDF's `pthread_self`
 * (`components/pthread/pthread.c`) looks the calling task up in its `esp_pthread_t` registry
 * and **asserts** when it is absent:
 *
 * ```c
 * pthread_t pthread_self(void) {
 *     esp_pthread_t *pthread = pthread_find(xTaskGetCurrentTaskHandle());
 *     if (!pthread) { assert(false && "Failed to find current thread ID!"); }
 *     ...
 * }
 * ```
 *
 * A task created with `xTaskCreate` — which includes the IDF **main task** running `app_main`,
 * and every task an application spawns directly — has no such registration. Under the IDF
 * default `CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y` that assert is a **silent
 * `abort()`**: a bare reset loop with no panic message, which on an OTA node is a rollback.
 * With IDF asserts compiled out it is worse than unavailable — `pthread_self()` returns `NULL`,
 * so the stamp equals the unowned sentinel and every native task reports itself as the owner of
 * an **unowned** mutex. The identity is not merely missing there; it is inverted.
 *
 * @section how What this answers instead
 *
 * `xTaskGetCurrentTaskHandle()` on ESP-IDF — always valid, never allocates, never asserts, and
 * correct for pthread-created and native tasks alike, since an `esp_pthread_t` is a wrapper
 * around exactly that handle. `std::this_thread::get_id()` everywhere else.
 *
 * **The sentinel and every live value are distinguishable, by construction.** That is the
 * property the ESP-IDF failure mode above turned on, so it is stated as an invariant rather
 * than inherited from the platform: `this_thread_id()` never returns `unowned_thread_id()`.
 * On the FreeRTOS arm a null handle — which `xTaskGetCurrentTaskHandle()` answers only before
 * the scheduler starts, i.e. during static initialization — is mapped to a distinct private
 * marker address rather than passed through as the unowned value.
 */
#pragma once

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"  // IWYU pragma: keep — ordering: FreeRTOS.h before task.h
#include "freertos/task.h"
#else
#include <thread>
#endif

namespace tr::detail {

#if defined(ESP_PLATFORM)

/** @brief The FreeRTOS task handle of the running task, as an opaque identity. */
using thread_id_t = void*;

namespace thread_id_impl {
/** @brief Address-taken marker standing for "running before the scheduler started".
 *
 * `xTaskGetCurrentTaskHandle()` answers `nullptr` only during static initialization. Passing
 * that through would make it compare equal to @ref unowned_thread_id, which is the exact
 * inversion #1532 is about, so it is mapped to this object's address instead — distinct from
 * `nullptr` and from every task handle. */
inline char pre_scheduler = 0;
}  // namespace thread_id_impl

/** @brief The running context's identity. Never equal to @ref unowned_thread_id. */
[[nodiscard]] inline thread_id_t this_thread_id() noexcept {
    void* const handle = static_cast<void*>(xTaskGetCurrentTaskHandle());
    return handle != nullptr ? handle : static_cast<void*>(&thread_id_impl::pre_scheduler);
}

#else

/** @brief The hosted identity: `std::thread::id`. */
using thread_id_t = std::thread::id;

/** @brief The running context's identity. Never equal to @ref unowned_thread_id — a running
 *         thread's `std::thread::id` is never the default-constructed one. */
[[nodiscard]] inline thread_id_t this_thread_id() noexcept { return std::this_thread::get_id(); }

#endif

/** @brief The "nobody owns it" value a stamp is cleared to. Distinct from every value
 *         @ref this_thread_id can answer. */
[[nodiscard]] inline thread_id_t unowned_thread_id() noexcept { return thread_id_t{}; }

}  // namespace tr::detail
