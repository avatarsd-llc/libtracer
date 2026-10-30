/**
 * @file
 * @brief #2416 — `tr::static_thread_t`'s ESP-IDF branch, on a host.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * An ESP-IDF build compiles the FreeRTOS branch of `core/src/static_thread.cpp`: the task's
 * control record, `StaticTask_t` and stack are ONE block drawn from the caller's source, and
 * the task is created with `xTaskCreateStaticPinnedToCore`. Every other host build compiles the
 * POSIX branch, so before this suite that branch ran only on silicon. This target compiles the
 * real TU with `ESP_PLATFORM` set, against fake_idf's FreeRTOS declarations and an empty
 * sdkconfig.h (fake_sdkconfig/), and defines the fake task and semaphore below: a host thread
 * stands in for a task, and the fake records every creation it is asked for.
 *
 * It links no `libtracer`: with `ESP_PLATFORM` set, `static_thread_t` has its FreeRTOS layout,
 * which the library's own POSIX objects do not share. The TU needs only `mem_source.hpp`'s
 * interface, which is header-only.
 *
 * What it pins (the issue's three criteria, plus the two refusal arms):
 *   1. a spawn creates its task with the static creator, on a stack inside the block the
 *      source handed out, at the requested depth. Only the static creator is declared by
 *      fake_idf's task.h, so a heap-allocating creator would not compile here at all;
 *   2. a source that refuses the block makes `start` return false, and no task is created;
 *   3. a platform that refuses the task makes `start` return false and the block go back;
 *   4. `join` (and the destructor) returns the block to its source, at the size it was drawn;
 *   5. a detached thread's block is returned by the next `join` once its body has ended.
 */

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "libtracer/mem_source.hpp"
#include "libtracer/static_thread.hpp"

#if !defined(LIBTRACER_STATIC_THREAD_FREERTOS)
#error "this suite must compile static_thread_t's FreeRTOS branch (ESP_PLATFORM, no IDF linux)"
#endif

// --- The fake FreeRTOS task and binary semaphore -------------------------------------------

struct fake_semaphore_t {
    std::mutex m;
    std::condition_variable cv;
    bool given = false;
};

namespace {

/** @brief One fake task: a host thread, and the suspend/delete handshake. */
struct fake_task_t {
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool suspended = false;
    bool deleted = false;
};

/** @brief What the fake was asked for; reset by each case. */
struct fake_log_t {
    std::mutex m;
    int created = 0;             ///< Successful static creations.
    int deleted = 0;             ///< vTaskDelete calls.
    int suspended = 0;           ///< Tasks that suspended themselves.
    bool refuse_create = false;  ///< The next creations return null.
    StackType_t* last_stack = nullptr;
    std::uint32_t last_depth = 0;
    StaticTask_t* last_tcb = nullptr;
};

fake_log_t g_log;
thread_local fake_task_t* t_self = nullptr;
// The semaphores live for the process: a static semaphore is never deleted by its user.
std::mutex g_sems_m;
std::vector<std::unique_ptr<fake_semaphore_t>> g_sems;

}  // namespace

SemaphoreHandle_t xSemaphoreCreateBinaryStatic(StaticSemaphore_t* /*storage*/) {
    const std::lock_guard lock(g_sems_m);
    g_sems.push_back(std::make_unique<fake_semaphore_t>());
    return g_sems.back().get();
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    std::unique_lock lock(s->m);
    if (ticks == portMAX_DELAY) {
        s->cv.wait(lock, [s] { return s->given; });
    } else if (!s->given) {
        return pdFALSE;
    }
    s->given = false;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s) {
    {
        const std::lock_guard lock(s->m);
        if (s->given) return pdFALSE;
        s->given = true;
    }
    s->cv.notify_all();
    return pdTRUE;
}

void vTaskDelay(TickType_t /*ticks*/) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char* /*name*/, uint32_t depth,
                                           void* arg, UBaseType_t /*priority*/, StackType_t* stack,
                                           StaticTask_t* tcb, BaseType_t /*core*/) {
    {
        const std::lock_guard lock(g_log.m);
        if (g_log.refuse_create) return nullptr;
        ++g_log.created;
        g_log.last_stack = stack;
        g_log.last_depth = depth;
        g_log.last_tcb = tcb;
    }
    auto* const t = new fake_task_t();
    t->th = std::thread([t, fn, arg] {
        t_self = t;
        fn(arg);
    });
    return t;
}

void vTaskSuspend(TaskHandle_t task) {
    // static_thread_t suspends only itself, as its body's last act; the thread then waits here
    // until it is deleted, as a suspended task would sit off the CPU.
    fake_task_t* const t = task != nullptr ? static_cast<fake_task_t*>(task) : t_self;
    {
        const std::lock_guard log_lock(g_log.m);
        ++g_log.suspended;
    }
    std::unique_lock lock(t->m);
    t->suspended = true;
    t->cv.notify_all();
    t->cv.wait(lock, [t] { return t->deleted; });
}

eTaskState eTaskGetState(TaskHandle_t task) {
    auto* const t = static_cast<fake_task_t*>(task);
    const std::lock_guard lock(t->m);
    return t->suspended ? eSuspended : eRunning;
}

void vTaskDelete(TaskHandle_t task) {
    auto* const t = static_cast<fake_task_t*>(task);
    {
        const std::lock_guard lock(t->m);
        t->deleted = true;
    }
    t->cv.notify_all();
    t->th.join();
    delete t;
    const std::lock_guard lock(g_log.m);
    ++g_log.deleted;
}

// --- The suite --------------------------------------------------------------------------------

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
    if (!ok) ++g_failures;
}

/** @brief A source that records its one outstanding block and can be told to refuse. */
class counting_source_t final : public tr::mem::block_source_t {
   public:
    counting_source_t() noexcept : block_source_t("counting") {}

    void* try_alloc(std::size_t bytes, std::size_t align) noexcept override {
        if (refuse) return nullptr;
        void* const p = std::aligned_alloc(align, (bytes + align - 1) / align * align);
        if (p != nullptr) {
            ++allocs;
            block = p;
            block_bytes = bytes;
        }
        return p;
    }

    void release(void* p, std::size_t bytes, std::size_t /*align*/) noexcept override {
        ++releases;
        released_matching = released_matching || (p == block && bytes == block_bytes);
        std::free(p);
    }

    bool refuse = false;
    int allocs = 0;
    int releases = 0;
    void* block = nullptr;
    std::size_t block_bytes = 0;
    bool released_matching = false;
};

void reset_log() {
    const std::lock_guard lock(g_log.m);
    g_log.created = 0;
    g_log.deleted = 0;
    g_log.suspended = 0;
    g_log.refuse_create = false;
    g_log.last_stack = nullptr;
    g_log.last_depth = 0;
    g_log.last_tcb = nullptr;
}

std::atomic<int> g_ran{0};
void body(void* arg) { static_cast<std::atomic<int>*>(arg)->fetch_add(1); }

void spawn_is_static_on_the_drawn_block() {
    std::printf("a spawn creates a static task on a stack inside the drawn block\n");
    reset_log();
    counting_source_t src;
    g_ran = 0;
    constexpr std::size_t kStack = 4096;
    {
        tr::static_thread_t t;
        check(t.start(&body, &g_ran, kStack, "t", src), "start succeeds");
        check(t.joinable(), "the thread is joinable");
        check(src.allocs == 1, "exactly one block is drawn from the source");
        auto* const lo = static_cast<std::byte*>(src.block);
        auto* const hi = lo + src.block_bytes;
        auto* const stack = reinterpret_cast<std::byte*>(g_log.last_stack);
        auto* const tcb = reinterpret_cast<std::byte*>(g_log.last_tcb);
        check(g_log.created == 1, "the task is created with the static creator");
        check(stack >= lo && stack + g_log.last_depth <= hi, "the stack lies inside that block");
        check(tcb >= lo && tcb + sizeof(StaticTask_t) <= stack,
              "the task control block lies inside that block, ahead of the stack");
        check(g_log.last_depth == kStack, "the stack depth is the one requested");
        t.join();
        check(g_ran.load() == 1, "the body ran once");
        check(!t.joinable(), "join ends the thread here");
        check(src.releases == 1 && src.released_matching,
              "join returns the block to its source at the size it was drawn");
        check(g_log.deleted == 1, "join deletes the task");
    }
    check(src.releases == 1, "the destructor of a joined thread releases nothing more");
}

void destructor_returns_the_block() {
    std::printf("destroying an unjoined thread joins it and returns the block\n");
    reset_log();
    counting_source_t src;
    g_ran = 0;
    {
        tr::static_thread_t t;
        check(t.start(&body, &g_ran, 0, "t", src), "start succeeds with the default stack");
    }
    check(g_ran.load() == 1, "the body ran");
    check(src.releases == 1 && src.released_matching, "the block went back to its source");
    check(g_log.deleted == 1, "the task was deleted");
}

void refused_block_is_a_failed_spawn() {
    std::printf("a source that refuses the block makes the spawn fail, with no task\n");
    reset_log();
    counting_source_t src;
    src.refuse = true;
    tr::static_thread_t t;
    check(!t.start(&body, &g_ran, 4096, "t", src), "start reports the refusal");
    check(!t.joinable(), "nothing is joinable");
    check(g_log.created == 0, "no task was created");
    check(src.allocs == 0 && src.releases == 0, "nothing was drawn, nothing is held");
}

void refused_task_returns_the_block() {
    std::printf("a platform that refuses the task makes the spawn fail and returns the block\n");
    reset_log();
    {
        const std::lock_guard lock(g_log.m);
        g_log.refuse_create = true;
    }
    counting_source_t src;
    tr::static_thread_t t;
    check(!t.start(&body, &g_ran, 4096, "t", src), "start reports the refusal");
    check(!t.joinable(), "nothing is joinable");
    check(src.allocs == 1 && src.releases == 1 && src.released_matching,
          "the drawn block went straight back to its source");
    reset_log();
}

/** @brief How many fake tasks have suspended themselves (their body done, `done` given). */
int suspended_count() {
    const std::lock_guard lock(g_log.m);
    return g_log.suspended;
}

void detached_block_is_reaped() {
    std::printf("a detached thread's block is returned by the next join once its body ended\n");
    reset_log();
    counting_source_t src;
    tr::static_thread_t t;
    check(t.start(&body, &g_ran, 4096, "d", src), "start succeeds");
    t.detach();
    check(!t.joinable(), "detach ends the thread here");
    // Wait for the task to SUSPEND, not just for its body to return: the task gives its done
    // semaphore between the two, and only a given semaphore makes the block reapable. Once
    // it has suspended, the next join on ANY static_thread_t reaps the parked block.
    while (suspended_count() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    tr::static_thread_t other;
    other.join();
    check(src.releases == 1 && src.released_matching, "the parked block went back to its source");
    check(g_log.deleted == 1, "the parked task was deleted");
}

}  // namespace

int main() {
    // Line-buffered: LeakSanitizer ends the process from its exit hook, before stdio would
    // flush a piped stdout, and ctest pipes it.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    spawn_is_static_on_the_drawn_block();
    destructor_returns_the_block();
    refused_block_is_a_failed_spawn();
    refused_task_returns_the_block();
    detached_block_is_reaped();
    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
