/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 */
/**
 * @file
 * @brief `tr::static_thread_t`'s two spawn policies (#2288): POSIX `pthread_create`, and the
 *        ESP-IDF static FreeRTOS task whose control record, `StaticTask_t` and stack are one
 *        block from the caller's source.
 */

#include "libtracer/static_thread.hpp"

#include <cstddef>
#include <new>

#if defined(LIBTRACER_STATIC_THREAD_FREERTOS)
#include <mutex>

#include "freertos/FreeRTOS.h"  // IWYU pragma: keep — ordering: FreeRTOS.h before task.h
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#endif

namespace tr {

#if defined(LIBTRACER_STATIC_THREAD_FREERTOS)

namespace {

#if defined(CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT)
constexpr std::size_t kDefaultStack = CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT;
#else
constexpr std::size_t kDefaultStack = 3072;
#endif
#if defined(CONFIG_PTHREAD_TASK_PRIO_DEFAULT)
constexpr UBaseType_t kPriority = CONFIG_PTHREAD_TASK_PRIO_DEFAULT;
#else
constexpr UBaseType_t kPriority = 5;
#endif
#if defined(CONFIG_PTHREAD_TASK_CORE_DEFAULT) && CONFIG_PTHREAD_TASK_CORE_DEFAULT >= 0
constexpr BaseType_t kCore = CONFIG_PTHREAD_TASK_CORE_DEFAULT;
#else
constexpr BaseType_t kCore = tskNO_AFFINITY;
#endif

/** @brief The block's alignment: the stack after the record is 16-byte aligned. */
constexpr std::size_t kAlign = 16;

/** @brief The head of a thread's block; the stack follows it. */
struct ctl_t {
    StaticTask_t tcb;                      /**< @brief The task's control block. */
    StaticSemaphore_t done_buf;            /**< @brief Storage of @ref done. */
    SemaphoreHandle_t done = nullptr;      /**< @brief Given once, when the body returns. */
    TaskHandle_t task = nullptr;           /**< @brief The task. */
    static_thread_t::entry_t fn = nullptr; /**< @brief The body. */
    void* arg = nullptr;                   /**< @brief Its argument. */
    mem::block_source_t* src = nullptr;    /**< @brief Where this block came from. */
    std::size_t bytes = 0;                 /**< @brief The size it was drawn at. */
    ctl_t* next = nullptr;                 /**< @brief The parked list's link (detach). */
};

constexpr std::size_t kCtlBytes = (sizeof(ctl_t) + kAlign - 1) / kAlign * kAlign;
// The ESP component sizes its thread-stack region at 640 B of task block per stack
// (integrations/esp-idf/libtracer/CMakeLists.txt, CONFIG_LIBTRACER_THREAD_STACKS).
static_assert(kCtlBytes + 64 <= 640,
              "the thread-stack region's per-stack budget no longer "
              "holds a task block; raise it in the ESP CMakeLists.txt");

/** @brief Detached blocks not yet reaped, guarded by @ref g_parked_m. */
ctl_t* g_parked = nullptr;
std::mutex g_parked_m;

void task_entry(void* p) {
    auto* const c = static_cast<ctl_t*>(p);
    c->fn(c->arg);
    (void)xSemaphoreGive(c->done);
    // Suspended, never self-deleted: a static task cannot hand its own stack back, so the
    // joiner deletes it once it is off the CPU and then releases the block.
    vTaskSuspend(nullptr);
}

/**
 * @brief Delete a task whose body has returned and release its block. The give precedes the
 *        suspend, so the task may still be on its way there: wait it off the CPU first.
 *
 * `eSuspended` means off every core. IDF's `eTaskGetState` (FreeRTOS-Kernel/tasks.c, v5.x and
 * v6.0) tests `taskIS_CURRENTLY_RUNNING(pxTCB)`, which compares the TCB against
 * `pxCurrentTCBs[0]` AND `pxCurrentTCBs[1]` on a dual-core build, under the kernel lock, before
 * it looks at any state list. A task that has put itself on the suspended list but is still
 * current on the other core therefore reads `eRunning`, and only reads `eSuspended` once that
 * core has switched away. So the delete below always finds the task off-CPU and deletes it
 * here, rather than deferring it to the idle task while its stack is still live.
 */
void finish(ctl_t* c) noexcept {
    while (eTaskGetState(c->task) != eSuspended) vTaskDelay(1);
    vTaskDelete(c->task);  // a suspended task is deleted here, running its TLS destructors
    mem::block_source_t* const src = c->src;
    const std::size_t bytes = c->bytes;
    c->~ctl_t();
    src->release(c, bytes, kAlign);
}

/** @brief Reap every parked block whose body has returned. */
void reap_parked() noexcept {
    ctl_t* done = nullptr;
    {
        const std::lock_guard lock(g_parked_m);
        for (ctl_t** at = &g_parked; *at != nullptr;) {
            ctl_t* const c = *at;
            if (xSemaphoreTake(c->done, 0) == pdTRUE) {
                *at = c->next;
                c->next = done;
                done = c;
            } else {
                at = &c->next;
            }
        }
    }
    while (done != nullptr) {
        ctl_t* const c = done;
        done = c->next;
        finish(c);
    }
}

}  // namespace

bool static_thread_t::start(entry_t fn, void* arg, std::size_t stack, const char* name,
                            mem::block_source_t& src) noexcept {
    reap_parked();
    if (ctl_ != nullptr) return false;
    const std::size_t stack_bytes = stack != 0 ? stack : kDefaultStack;
    const std::size_t bytes = kCtlBytes + (stack_bytes + kAlign - 1) / kAlign * kAlign;
    void* const block = src.try_alloc(bytes, kAlign);
    if (block == nullptr) return false;
    auto* const c = ::new (block) ctl_t{};
    c->fn = fn;
    c->arg = arg;
    c->src = &src;
    c->bytes = bytes;
    c->done = xSemaphoreCreateBinaryStatic(&c->done_buf);
    auto* const stack_base =
        reinterpret_cast<StackType_t*>(static_cast<std::byte*>(block) + kCtlBytes);
    c->task = xTaskCreateStaticPinnedToCore(&task_entry, name, stack_bytes, c, kPriority,
                                            stack_base, &c->tcb, kCore);
    if (c->task == nullptr) {
        c->~ctl_t();
        src.release(block, bytes, kAlign);
        return false;
    }
    ctl_ = c;
    return true;
}

void static_thread_t::join() noexcept {
    if (ctl_ != nullptr) {
        auto* const c = static_cast<ctl_t*>(ctl_);
        ctl_ = nullptr;
        (void)xSemaphoreTake(c->done, portMAX_DELAY);
        finish(c);
    }
    reap_parked();
}

void static_thread_t::detach() noexcept {
    if (ctl_ != nullptr) {
        auto* const c = static_cast<ctl_t*>(ctl_);
        ctl_ = nullptr;
        const std::lock_guard lock(g_parked_m);
        c->next = g_parked;
        g_parked = c;
    }
    reap_parked();
}

bool static_thread_t::joinable() const noexcept { return ctl_ != nullptr; }

#else  // POSIX

namespace {

/** @brief What the new thread needs before it can run: drawn at start, released by the thread
 *         as its first act, so a detached thread never reads its (possibly gone) owner. */
struct boot_t {
    static_thread_t::entry_t fn;
    void* arg;
    mem::block_source_t* src;
};

void* thread_entry(void* p) {
    auto* const b = static_cast<boot_t*>(p);
    const boot_t copy = *b;
    copy.src->release(b, sizeof(boot_t), alignof(boot_t));
    copy.fn(copy.arg);
    return nullptr;
}

}  // namespace

bool static_thread_t::start(entry_t fn, void* arg, std::size_t stack, const char* /*name*/,
                            mem::block_source_t& src) noexcept {
    if (started_) return false;
    void* const p = src.try_alloc(sizeof(boot_t), alignof(boot_t));
    if (p == nullptr) return false;
    auto* const b = ::new (p) boot_t{fn, arg, &src};
    pthread_attr_t attr;
    ::pthread_attr_init(&attr);
    // A hint below the platform floor makes setstacksize answer EINVAL and leaves the attr's
    // default in place: the spawn still happens, on the default stack.
    if (stack != 0) (void)::pthread_attr_setstacksize(&attr, stack);
    // An error code, never a throw: a `std::thread` spawn failure is a `std::abort()` under
    // -fno-exceptions.
    started_ = ::pthread_create(&t_, &attr, &thread_entry, b) == 0;
    ::pthread_attr_destroy(&attr);
    if (!started_) src.release(b, sizeof(boot_t), alignof(boot_t));
    return started_;
}

void static_thread_t::join() noexcept {
    if (!started_) return;
    started_ = false;
    (void)::pthread_join(t_, nullptr);
}

void static_thread_t::detach() noexcept {
    if (!started_) return;
    started_ = false;
    (void)::pthread_detach(t_);
}

bool static_thread_t::joinable() const noexcept { return started_; }

#endif

}  // namespace tr
