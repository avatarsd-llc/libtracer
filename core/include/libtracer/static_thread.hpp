/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief `tr::static_thread_t` — one joinable thread whose stack and control block, where the
 *        platform would otherwise take them from the heap, are drawn from a
 *        `tr::mem::block_source_t` (#2288).
 *
 * The spawn policy is the build's platform, decided at compile time:
 *
 * | build | spawn | what the source pays |
 * | --- | --- | --- |
 * | POSIX host | `pthread_create`, the stack-size hint applied through its attr | a three-pointer
 * boot record, released by the thread as it begins; the stack is the platform's mapping, not a heap
 * block | | ESP-IDF (`ESP_PLATFORM`) | `xTaskCreateStatic` | ONE block: the control record, the
 * task's `StaticTask_t` and its stack |
 *
 * On ESP-IDF a `pthread_create` (and a `std::thread`, which is one) draws the task's stack and
 * its pthread record from the platform heap, which is what the zero-heap gate refuses in the
 * archive. A FreeRTOS task created static allocates nothing, so the block a link's own source
 * serves is the whole cost, and a starved source refuses the spawn with an error code instead
 * of an abort.
 *
 * The thread runs a plain function pointer with one argument. It is not a pthread on ESP-IDF:
 * `std::mutex`, `std::condition_variable` and `thread_local` all work from a FreeRTOS task
 * there, and a `thread_local` destructor runs when the task is deleted, which `static_thread_t::join` does
 * before it returns the block.
 */
#pragma once

#include <cstddef>

#if !defined(ESP_PLATFORM)
#include <pthread.h>
#endif

#include "libtracer/mem_source.hpp"

namespace tr {

/**
 * @brief One joinable thread, spawned from a block source where the platform needs memory for
 *        it (see the file comment for the per-platform policy).
 *
 * Not copyable or movable: the running thread holds this object's address on ESP-IDF only
 * through its own block, but a caller-visible move would still race a concurrent @ref join.
 * The destructor joins.
 */
class static_thread_t {
   public:
    /** @brief The thread body: a plain function taking the argument @ref start was given. */
    using entry_t = void (*)(void* arg);

    /** @brief No thread. */
    static_thread_t() noexcept = default;
    static_thread_t(const static_thread_t&) = delete;
    static_thread_t& operator=(const static_thread_t&) = delete;
    /** @brief Joins a still-running thread (a no-op when none was started or it was joined). */
    ~static_thread_t() { join(); }

    /**
     * @brief Spawn @p fn(@p arg) on a new thread.
     *
     * @param fn    The body; it runs once and the thread ends when it returns.
     * @param arg   Passed to @p fn unchanged.
     * @param stack Stack bytes, or 0 for the platform default (the pthread default stack on
     *              both platforms: `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT` on ESP-IDF). On
     *              POSIX a hint below the platform floor leaves the default in place.
     * @param name  Task name on ESP-IDF; must outlive the thread (a literal). Unused on POSIX.
     * @param src   Where the stack and control block come from on ESP-IDF, and the boot
     *              record on POSIX; it MUST outlive the thread (until @ref join returns, or,
     *              for a thread that may be detached, the process).
     * @retval false The spawn failed (the source refused, or the platform did); nothing runs
     *               and nothing is held. Also false when a thread is already running here.
     */
    [[nodiscard]] bool start(entry_t fn, void* arg, std::size_t stack, const char* name,
                             mem::block_source_t& src) noexcept;

    /** @brief Wait for the thread to end and release what it held (idempotent). On ESP-IDF this
     *         also reaps any detached thread that has finished (see @ref detach). */
    void join() noexcept;

    /**
     * @brief Let the thread run on without this object; it is no longer joinable here.
     *
     * POSIX: `pthread_detach`. ESP-IDF: a static task cannot return its own stack, so its
     * block is PARKED on a process-wide intrusive list (the link lives inside the block; a
     * static lock guards it; nothing is allocated). A parked block whose body has returned is
     * reaped — the task deleted and the block released to its source — by the next
     * @ref start, @ref join or @ref detach on any `static_thread_t` in the process, so it is
     * held at most until then. The block's source therefore MUST outlive the process: spawn a
     * thread that may be detached from `tr::mem::default_root()` or a source of the same
     * lifetime, never from a source that dies with its owner.
     */
    void detach() noexcept;

    /** @brief True from a successful @ref start until @ref join or @ref detach. */
    [[nodiscard]] bool joinable() const noexcept;

   private:
#if defined(ESP_PLATFORM)
    void* ctl_ = nullptr; /**< @brief The control record at the head of the drawn block. */
#else
    pthread_t t_{};        /**< @brief The thread, valid while `started_` is set. */
    bool started_ = false; /**< @brief Set by a successful @ref start; cleared by join/detach. */
#endif
};

}  // namespace tr
