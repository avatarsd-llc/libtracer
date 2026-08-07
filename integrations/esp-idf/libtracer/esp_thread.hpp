/**
 * @file
 * @brief `tr::esp::spawn_thread` — start a `std::thread` on a RIGHT-SIZED FreeRTOS task
 *        stack. The one recipe every chip-native link in this component needs.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * `std::thread` on ESP-IDF is a pthread, and a pthread's stack comes from the GLOBAL
 * `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` — there is no per-thread argument on the
 * `std::thread` constructor to override it. The only seam IDF offers is
 * `esp_pthread_set_cfg`, which arms the NEXT `pthread_create` performed on the CALLING
 * thread. So sizing one thread means save / set / spawn / restore, and skipping the
 * restore would leak this link's stack size onto every later thread spawned from the
 * same thread (typically app_main, which spawns them all).
 *
 * That four-step sequence is not something each link should re-derive: `twai_link_t`
 * carried the only correct copy of it (#486) while `esp_ws_client_link_t` accepted a
 * `recv_stack` and dropped it on the floor — a caller who sized the stack for its
 * delivery path silently got the default and a stack-overflow reboot on the deep
 * in-call graph delivery (#900). One helper, used by both, is the fix.
 *
 * Private to the component (it names an IDF header), so no dependent inherits an
 * `esp_pthread.h` include; sources in this directory reach it as `"esp_thread.hpp"`.
 */
#pragma once

#include <cstddef>
#include <thread>
#include <utility>

#include "esp_pthread.h"

namespace tr::esp {

/**
 * @brief Spawn @p body on a thread of @p stack bytes, named @p name.
 *
 * @param stack Stack size in bytes; **0 means the platform default** — the call then
 *              reduces to a plain `std::thread`, touching no pthread config at all, so
 *              a caller that asks for nothing gets exactly the historical behaviour.
 * @param name  FreeRTOS task name; must outlive the spawn (a literal). Ignored when
 *              @p stack is 0, since no config is applied.
 *
 * @note Callable from any thread. The surrounding `esp_pthread` config of the CALLING
 *       thread is saved and restored around the spawn, so this never leaks @p stack
 *       onto a later `std::thread` created by the same caller.
 *
 * @note The had-a-config distinction is load-bearing, not defensive coding.
 *       `esp_pthread_get_cfg` does **not** leave its out-param untouched when the
 *       calling thread has no config: on `ESP_ERR_NOT_FOUND` it `memset`s the struct to
 *       ZERO (IDF `components/pthread/pthread.c`). Seeding from
 *       `esp_pthread_get_default_config()` and calling straight through therefore
 *       *destroys* the seed and leaves `stack_size == 0`, which `esp_pthread_set_cfg`
 *       rejects with `ESP_ERR_INVALID_ARG` (it requires `>= PTHREAD_STACK_MIN`). The
 *       restore then silently does nothing and this link's `stack_size` / `thread_name`
 *       stay armed on the caller — leaking onto every later `std::thread` it spawns.
 *       That is the common case, not a corner: a thread has no config until someone
 *       sets one, and `app_main` typically spawns every link without ever doing so.
 *       So the return code is CHECKED, and the no-config case restores the platform
 *       default explicitly. IDF exposes no way to return a thread to "unset", but the
 *       default config carries exactly the `stack_size` / `prio` / `pin_to_core` an
 *       unset thread would have used, so the observable behaviour is the same.
 */
template <typename body_t>
[[nodiscard]] std::thread spawn_thread(std::size_t stack, const char* name, body_t&& body) {
    if (stack == 0) return std::thread(std::forward<body_t>(body));
    esp_pthread_cfg_t saved{};
    const bool had_cfg = esp_pthread_get_cfg(&saved) == ESP_OK;
    esp_pthread_cfg_t cfg = had_cfg ? saved : esp_pthread_get_default_config();
    cfg.stack_size = stack;
    cfg.thread_name = name;
    (void)esp_pthread_set_cfg(&cfg);
    std::thread spawned(std::forward<body_t>(body));
    const esp_pthread_cfg_t restore = had_cfg ? saved : esp_pthread_get_default_config();
    (void)esp_pthread_set_cfg(&restore);
    return spawned;
}

}  // namespace tr::esp
