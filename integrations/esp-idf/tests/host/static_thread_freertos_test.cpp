/**
 * @file
 * @brief #2416, #2352 — `tr::static_thread_t`'s ESP-IDF branch, on a host.
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
 * The fake FreeRTOS calls are `xTaskCreateStaticPinnedToCore`, `vTaskSuspend`, `vTaskDelete`,
 * `eTaskGetState`, `vTaskDelay`, and the static binary semaphore (`xSemaphoreCreateBinaryStatic`,
 * `xSemaphoreTake`, `xSemaphoreGive`). The fake also checks the two orderings the branch exists
 * for: no task is deleted before it has suspended itself (off the CPU), and no block goes back
 * to its source while a live task's control block still lies inside it.
 *
 * What it pins (#2416's three criteria, plus the two refusal arms):
 *   1. a spawn creates its task with the static creator, on a stack inside the block the
 *      source handed out, at the requested depth. Only the static creator is declared by
 *      fake_idf's task.h, so a heap-allocating creator would not compile here at all;
 *   2. a source that refuses the block makes `start` return false, and no task is created;
 *   3. a platform that refuses the task makes `start` return false and the block go back;
 *   4. `join` (and the destructor) returns the block to its source, at the size it was drawn;
 *   5. a detached thread's block is returned by the next `join` once its body has ended.
 * and #2352's run of the join and detach paths against a task that has not finished:
 *   6. a `join` entered while the body still runs waits for it, and only then deletes the task
 *      and returns the block;
 *   7. a `join` that finds the task between its done-give and its suspend waits it off the
 *      CPU (`eTaskGetState`, then `vTaskDelay`) before it deletes it;
 *   8. a detached thread whose body still runs stays parked through a reap, and is reaped
 *      once it ends;
 *   9. a reap over several parked blocks releases the finished ones and keeps the running one.
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
#include <set>
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
    StaticTask_t* tcb = nullptr;  ///< The control block it was created on.
    bool suspended = false;
    bool deleted = false;
    bool polled = false;  ///< Its state was queried while it was still on the CPU.
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
    bool hold_suspend = false;     ///< A suspending task waits until its state is polled first.
    int delays = 0;                ///< vTaskDelay calls.
    int deleted_unsuspended = 0;   ///< vTaskDelete of a task not yet off the CPU: a bug.
    std::set<StaticTask_t*> live;  ///< Control blocks of tasks not yet deleted.
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

void vTaskDelay(TickType_t /*ticks*/) {
    {
        const std::lock_guard lock(g_log.m);
        ++g_log.delays;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

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
        g_log.live.insert(tcb);
    }
    auto* const t = new fake_task_t();
    t->tcb = tcb;
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
    bool hold = false;
    {
        const std::lock_guard log_lock(g_log.m);
        ++g_log.suspended;
        hold = g_log.hold_suspend;
    }
    std::unique_lock lock(t->m);
    // Held: the task stays on the CPU, past its done-give, until a joiner has seen it there
    // (or deletes it early, which the delete counts as the bug it is).
    if (hold) t->cv.wait(lock, [t] { return t->polled || t->deleted; });
    t->suspended = true;
    t->cv.notify_all();
    t->cv.wait(lock, [t] { return t->deleted; });
}

eTaskState eTaskGetState(TaskHandle_t task) {
    auto* const t = static_cast<fake_task_t*>(task);
    {
        const std::lock_guard lock(t->m);
        if (t->suspended) return eSuspended;
        t->polled = true;
    }
    t->cv.notify_all();
    return eRunning;
}

void vTaskDelete(TaskHandle_t task) {
    auto* const t = static_cast<fake_task_t*>(task);
    bool was_suspended = false;
    {
        const std::lock_guard lock(t->m);
        was_suspended = t->suspended;
        t->deleted = true;
    }
    t->cv.notify_all();
    t->th.join();
    StaticTask_t* const tcb = t->tcb;
    delete t;
    const std::lock_guard lock(g_log.m);
    ++g_log.deleted;
    if (!was_suspended) ++g_log.deleted_unsuspended;
    g_log.live.erase(tcb);
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
        {
            const std::lock_guard lock(g_log.m);
            auto* const lo = static_cast<std::byte*>(p);
            for (StaticTask_t* tcb : g_log.live) {
                auto* const at = reinterpret_cast<std::byte*>(tcb);
                if (at >= lo && at < lo + bytes) ++released_live;
            }
        }
        std::free(p);
    }

    bool refuse = false;
    int allocs = 0;
    int releases = 0;
    void* block = nullptr;
    std::size_t block_bytes = 0;
    bool released_matching = false;
    int released_live = 0;  ///< Releases of a block whose task was not yet deleted: a bug.
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
    g_log.hold_suspend = false;
    g_log.delays = 0;
    g_log.deleted_unsuspended = 0;
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

/** @brief A body that holds its task on the CPU until the test opens the gate. */
struct gate_t {
    std::mutex m;
    std::condition_variable cv;
    bool entered = false;
    bool open = false;
};

void gated_body(void* arg) {
    auto* const g = static_cast<gate_t*>(arg);
    std::unique_lock lock(g->m);
    g->entered = true;
    g->cv.notify_all();
    g->cv.wait(lock, [g] { return g->open; });
}

void wait_entered(gate_t& g) {
    std::unique_lock lock(g.m);
    g.cv.wait(lock, [&g] { return g.entered; });
}

void open_gate(gate_t& g) {
    {
        const std::lock_guard lock(g.m);
        g.open = true;
    }
    g.cv.notify_all();
}

/** @brief A snapshot of the fake's counters, taken under its lock. */
struct counts_t {
    int created, deleted, suspended, delays, deleted_unsuspended;
    std::size_t live;
};

counts_t counts() {
    const std::lock_guard lock(g_log.m);
    return {
        g_log.created,    g_log.deleted, g_log.suspended, g_log.delays, g_log.deleted_unsuspended,
        g_log.live.size()};
}

void join_waits_for_a_running_body() {
    std::printf("a join entered while the body runs waits for it, then deletes and releases\n");
    reset_log();
    counting_source_t src;
    gate_t gate;
    tr::static_thread_t t;
    check(t.start(&gated_body, &gate, 4096, "j", src), "start succeeds");
    wait_entered(gate);
    std::atomic<bool> joining{false};
    std::atomic<bool> held_while_running{false};
    // The opener runs only while the main thread is inside join: it sees the task still
    // alive and the block still held, and only then lets the body return.
    std::thread opener([&] {
        while (!joining.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const counts_t c = counts();
        held_while_running = c.deleted == 0 && c.live == 1 && c.suspended == 0;
        open_gate(gate);
    });
    joining = true;
    t.join();
    opener.join();
    const counts_t c = counts();
    check(held_while_running.load(), "join did not delete the task while its body ran");
    check(!t.joinable(), "join ends the thread here");
    check(c.deleted == 1 && c.live == 0, "join deleted the task once its body returned");
    check(c.deleted_unsuspended == 0, "the task was off the CPU when it was deleted");
    check(src.releases == 1 && src.released_matching && src.released_live == 0,
          "the block went back only after its task was gone");
}

void join_waits_the_task_off_the_cpu() {
    std::printf("a join that finds the task between its give and its suspend waits it off\n");
    reset_log();
    {
        const std::lock_guard lock(g_log.m);
        g_log.hold_suspend = true;
    }
    counting_source_t src;
    g_ran = 0;
    tr::static_thread_t t;
    check(t.start(&body, &g_ran, 4096, "w", src), "start succeeds");
    t.join();
    const counts_t c = counts();
    check(g_ran.load() == 1, "the body ran once");
    check(c.delays >= 1, "join yielded (vTaskDelay) while the task was still on the CPU");
    check(c.deleted == 1 && c.deleted_unsuspended == 0,
          "the task was deleted only once it had suspended");
    check(src.releases == 1 && src.released_live == 0,
          "the block went back only after its task was gone");
    reset_log();
}

void running_detached_thread_stays_parked() {
    std::printf("a detached thread whose body runs survives a reap, and is reaped once done\n");
    reset_log();
    counting_source_t src;
    gate_t gate;
    tr::static_thread_t t;
    check(t.start(&gated_body, &gate, 4096, "p", src), "start succeeds");
    wait_entered(gate);
    t.detach();
    check(!t.joinable(), "detach ends the thread here");
    {
        tr::static_thread_t other;
        other.join();  // a reap over a parked block whose body has not returned
    }
    counts_t c = counts();
    check(src.releases == 0 && c.deleted == 0 && c.live == 1,
          "the running task was kept parked: not deleted, its block still held");
    open_gate(gate);
    while (suspended_count() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
        tr::static_thread_t other;
        other.join();
    }
    c = counts();
    check(src.releases == 1 && src.released_matching && src.released_live == 0,
          "once its body returned, the next reap returned the block after the task was gone");
    check(c.deleted == 1 && c.deleted_unsuspended == 0, "the parked task was deleted off-CPU");
}

void reap_keeps_the_running_among_the_parked() {
    std::printf("a reap over several parked blocks releases the finished, keeps the running\n");
    reset_log();
    counting_source_t src;
    gate_t gate;
    g_ran = 0;
    tr::static_thread_t a;
    tr::static_thread_t b;
    tr::static_thread_t c;
    check(a.start(&body, &g_ran, 4096, "a", src), "the first start succeeds");
    check(b.start(&gated_body, &gate, 4096, "b", src), "the second start succeeds");
    check(c.start(&body, &g_ran, 4096, "c", src), "the third start succeeds");
    wait_entered(gate);
    a.detach();
    b.detach();
    c.detach();
    while (suspended_count() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
        tr::static_thread_t other;
        other.join();
    }
    counts_t n = counts();
    check(src.releases == 2 && n.deleted == 2 && n.live == 1,
          "the two finished blocks were reaped, the running one kept");
    open_gate(gate);
    while (suspended_count() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
        tr::static_thread_t other;
        other.join();
    }
    n = counts();
    check(src.releases == 3 && n.deleted == 3 && n.live == 0, "the last block was reaped");
    check(n.deleted_unsuspended == 0 && src.released_live == 0,
          "every block went back only after its task was gone");
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
    join_waits_for_a_running_body();
    join_waits_the_task_off_the_cpu();
    running_detached_thread_stays_parked();
    reap_keeps_the_running_among_the_parked();
    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
