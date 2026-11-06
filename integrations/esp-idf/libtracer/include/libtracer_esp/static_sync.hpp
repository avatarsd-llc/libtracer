/**
 * @file
 * @brief `tr::esp::static_sync_t` — the blocking primitives every ESP-IDF chip target binds as
 *        `config_t::sync_t` (#2454): a mutex, a shared mutex and a condition variable that hold
 *        their FreeRTOS state inline and never touch the heap.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * On ESP-IDF `std::mutex`, `std::condition_variable` and `std::shared_mutex` are pthread
 * objects. The IDF pthread layer gives each one a record drawn from the platform heap
 * (`pthread_mutex_init`, `pthread_cond_init`, `pthread_rwlock_init`), and a mutex its FreeRTOS
 * semaphore from the heap too, inside IDF's own code, so the no-heap census of `libtracer.a`
 * never saw a `malloc` for them. (A condition wait was never the heap cost: IDF v6.0's
 * `pthread_cond_timedwait` already makes its semaphore static, on the waiter's stack, as
 * @ref static_condvar_t does.) These types are the static spelling of the same three promises:
 *
 * - @ref static_mutex_t: one `StaticSemaphore_t`, a FreeRTOS mutex with priority inheritance;
 *   a waiter pays nothing.
 * - @ref static_condvar_t: a portMUX and a waiter list; a waiter pays one binary semaphore on
 *   its own stack.
 * - @ref static_shared_mutex_t: a portMUX, two counts and a @ref static_condvar_t; a waiter
 *   pays as the condvar's does.
 *
 * It includes only FreeRTOS and the standard library, never a libtracer header: the override
 * fragment is included FROM `libtracer/config.hpp`, so anything this header pulled in from
 * libtracer would be a cycle.
 *
 * None of these is ISR-callable, as no pthread object was.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>  // std::unique_lock

#include "freertos/FreeRTOS.h"  // IWYU pragma: keep — ordering: FreeRTOS.h before semphr.h
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace tr::esp {

/**
 * @brief A blocking mutex over a statically created FreeRTOS mutex.
 *
 * A FreeRTOS mutex, like the one IDF's `pthread_mutex_t` wraps, inherits the priority of a
 * higher-priority waiter, so a control-plane section held by a low-priority task cannot be
 * starved by a middle one. Non-recursive, and unlocked by the task that locked it, as
 * `std::mutex` requires.
 */
class static_mutex_t {
   public:
    /** @brief Create the mutex in this object's own storage; cannot fail. The handle a static
     *         create returns is the storage's own address (`xQueueGenericCreateStatic` casts
     *         it), so @ref handle recomputes it instead of a member keeping a copy. */
    static_mutex_t() noexcept { (void)xSemaphoreCreateMutexStatic(&buf_); }
    /** @brief Unregister the semaphore; the storage is this object's, so nothing is freed. */
    ~static_mutex_t() { vSemaphoreDelete(handle()); }
    static_mutex_t(const static_mutex_t&) = delete;
    static_mutex_t& operator=(const static_mutex_t&) = delete;

    /** @brief Block until the mutex is taken. */
    void lock() noexcept { (void)xSemaphoreTake(handle(), portMAX_DELAY); }
    /** @brief Take the mutex only if it is free. */
    [[nodiscard]] bool try_lock() noexcept { return xSemaphoreTake(handle(), 0) == pdTRUE; }
    /** @brief Give the mutex back. */
    void unlock() noexcept { (void)xSemaphoreGive(handle()); }

   private:
    /** @brief The semaphore created in @ref buf_. */
    SemaphoreHandle_t handle() noexcept { return reinterpret_cast<SemaphoreHandle_t>(&buf_); }

    StaticSemaphore_t buf_{}; /**< @brief The semaphore's storage, and its handle. */
};

/**
 * @brief A condition variable whose waiters each sleep on a binary semaphore of their own,
 *        created on their own stack.
 *
 * The object itself is a portMUX and a FIFO list of the waiters' records. A wait links its
 * record under the portMUX BEFORE it releases the caller's lock, so a notify that follows the
 * caller's state change always finds it: no wakeup is lost. A notify unlinks under the portMUX
 * and gives each semaphore after leaving it, reading the next link first, because a waiter's
 * record dies the moment its wait returns. A timed-out waiter that a notify already unlinked
 * takes the give it is owed before it returns, so the notifier never touches a dead record.
 *
 * The timed waits block in the kernel with a tick timeout: they read no clock of their own,
 * and the predicate form keeps one deadline across spurious wakes with FreeRTOS's own
 * `vTaskSetTimeOutState` / `xTaskCheckForTimeOut`.
 *
 * Waits on any lock with `unlock()` / `lock()`, a `std::unique_lock<static_mutex_t>` in core.
 */
class static_condvar_t {
   public:
    static_condvar_t() noexcept = default;
    static_condvar_t(const static_condvar_t&) = delete;
    static_condvar_t& operator=(const static_condvar_t&) = delete;

    /** @brief Wake the longest-waiting sleeper, if any. */
    void notify_one() noexcept { wake(false); }
    /** @brief Wake every sleeper. */
    void notify_all() noexcept { wake(true); }

    /** @brief Release @p lock, sleep until notified, take @p lock again. May wake spuriously. */
    template <class Lock>
    void wait(Lock& lock) noexcept {
        block(lock, portMAX_DELAY);
    }
    /** @brief Sleep until @p pred holds, re-checked under @p lock. */
    template <class Lock, class Pred>
    void wait(Lock& lock, Pred pred) {
        while (!pred()) wait(lock);
    }
    /**
     * @brief Sleep until @p pred holds or @p d has passed since the call.
     * @return @p pred's last value, as `std::condition_variable::wait_for` returns it.
     */
    template <class Lock, class Rep, class Period, class Pred>
    bool wait_for(Lock& lock, const std::chrono::duration<Rep, Period>& d, Pred pred) {
        TickType_t left = ticks(d);
        TimeOut_t since;
        vTaskSetTimeOutState(&since);
        while (!pred()) {
            if (xTaskCheckForTimeOut(&since, &left) == pdTRUE) return pred();
            block(lock, left);
        }
        return true;
    }

   private:
    /** @brief One sleeper's record, on its own stack for the length of its wait. */
    struct waiter_t {
        StaticSemaphore_t buf{};  /**< @brief @ref sem's storage. */
        SemaphoreHandle_t sem{};  /**< @brief Given once, by the notify that unlinks this. */
        waiter_t* next = nullptr; /**< @brief The list link, under @ref mux_. */
        bool linked = false;      /**< @brief Still on the list (no notify claimed it). */
    };

    /**
     * @brief @p d in ticks: rounded up, PLUS one, clamped below `portMAX_DELAY` (which means
     *        forever).
     *
     * A block of `n` ticks ends at the n-th tick interrupt after the call, and the first of
     * those may be microseconds away, so `n` rounded-up ticks can end almost a whole tick short
     * of @p d. The extra tick makes a timed wait never report a timeout before @p d has passed,
     * the `std::condition_variable::wait_for` contract; IDF's `pthread_cond_timedwait` adds the
     * same tick for the same reason.
     */
    template <class Rep, class Period>
    static TickType_t ticks(const std::chrono::duration<Rep, Period>& d) noexcept {
        const auto ms = std::chrono::ceil<std::chrono::milliseconds>(d).count();
        if (ms <= 0) return 0;
        constexpr std::uint64_t kMaxMs =
            (static_cast<std::uint64_t>(portMAX_DELAY) - 2) / configTICK_RATE_HZ * 1000;
        if (static_cast<std::uint64_t>(ms) >= kMaxMs) return portMAX_DELAY - 1;
        return static_cast<TickType_t>(
            (static_cast<std::uint64_t>(ms) * configTICK_RATE_HZ + 999) / 1000 + 1);
    }

    /** @brief Link a record, release @p lock, sleep until notified or @p t ticks pass, retake
     *         @p lock. The caller re-checks its predicate either way. */
    template <class Lock>
    void block(Lock& lock, TickType_t t) noexcept {
        waiter_t w;
        // Under static_shared_mutex_t the caller's lock is a portMUX section, so this create
        // runs with interrupts masked. IDF advises against FreeRTOS calls there, but a static
        // create neither blocks nor allocates: it initializes the caller's own storage.
        w.sem = xSemaphoreCreateBinaryStatic(&w.buf);
        portENTER_CRITICAL(&mux_);
        w.linked = true;
        *tail_ = &w;
        tail_ = &w.next;
        portEXIT_CRITICAL(&mux_);
        lock.unlock();
        if (xSemaphoreTake(w.sem, t) != pdTRUE) {
            portENTER_CRITICAL(&mux_);
            const bool mine = w.linked;
            if (mine) unlink(&w);
            portEXIT_CRITICAL(&mux_);
            // A notify claimed the record first and owes it a give: wait for that, so it never
            // gives a semaphore on a stack this frame has left.
            if (!mine) (void)xSemaphoreTake(w.sem, portMAX_DELAY);
        }
        vSemaphoreDelete(w.sem);
        lock.lock();
    }

    /** @brief Remove @p w from the list (under @ref mux_). */
    void unlink(waiter_t* w) noexcept {
        waiter_t** at = &head_;
        while (*at != w) at = &(*at)->next;
        *at = w->next;
        if (tail_ == &w->next) tail_ = at;
        w->linked = false;
    }

    /** @brief Claim the head (or every) record under @ref mux_, then give each outside it. */
    void wake(bool all) noexcept {
        portENTER_CRITICAL(&mux_);
        waiter_t* claimed = head_;
        if (claimed != nullptr && !all) {
            head_ = claimed->next;
            if (head_ == nullptr) tail_ = &head_;
            claimed->next = nullptr;
        } else {
            head_ = nullptr;
            tail_ = &head_;
        }
        for (waiter_t* w = claimed; w != nullptr; w = w->next) w->linked = false;
        portEXIT_CRITICAL(&mux_);
        while (claimed != nullptr) {
            waiter_t* const w = claimed;
            claimed = w->next;  // read before the give: the record dies once its wait returns
            (void)xSemaphoreGive(w->sem);
        }
    }

    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED; /**< @brief Guards the list. */
    waiter_t* head_ = nullptr;                        /**< @brief Longest-waiting sleeper. */
    waiter_t** tail_ = &head_;                        /**< @brief Where the next one links. */
};

/**
 * @brief Shared readers or one writer, as `std::shared_mutex`, over a portMUX-guarded count.
 *
 * Readers are preferred, as glibc's default rwlock prefers them: a reader enters whenever no
 * writer holds the lock, even while a writer waits, so a reader that nests under another
 * reader's hold on a second lock cannot be wedged by a queued writer. The state edits are a few
 * instructions inside the portMUX; every sleep is on @ref cv_, after the portMUX is released.
 * No priority inheritance, as IDF's `pthread_rwlock_t` has none.
 */
class static_shared_mutex_t {
   public:
    static_shared_mutex_t() noexcept = default;
    static_shared_mutex_t(const static_shared_mutex_t&) = delete;
    static_shared_mutex_t& operator=(const static_shared_mutex_t&) = delete;

    /** @brief Block until no reader or writer holds the lock, then hold it alone. */
    void lock() noexcept {
        std::unique_lock<section_t> l(s_);
        while (writer_ || readers_ != 0) cv_.wait(l);
        writer_ = true;
    }
    /** @brief Give the exclusive hold back and wake every sleeper. */
    void unlock() noexcept {
        {
            const std::lock_guard<section_t> l(s_);
            writer_ = false;
        }
        cv_.notify_all();
    }
    /** @brief Block until no writer holds the lock, then hold it shared. */
    void lock_shared() noexcept {
        std::unique_lock<section_t> l(s_);
        while (writer_) cv_.wait(l);
        ++readers_;
    }
    /** @brief Give a shared hold back; the last reader out wakes a waiting writer. */
    void unlock_shared() noexcept {
        bool last = false;
        {
            const std::lock_guard<section_t> l(s_);
            last = --readers_ == 0;
        }
        if (last) cv_.notify_all();
    }

   private:
    /** @brief The portMUX critical section the counts are edited in. */
    struct section_t {
        portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;    /**< @brief The section's token. */
        void lock() noexcept { portENTER_CRITICAL(&mux); }  /**< @brief Enter. */
        void unlock() noexcept { portEXIT_CRITICAL(&mux); } /**< @brief Leave. */
    };

    section_t s_;               /**< @brief Guards @ref readers_ and @ref writer_. */
    std::uint32_t readers_ = 0; /**< @brief Shared holds outstanding. */
    bool writer_ = false;       /**< @brief Whether one task holds the lock exclusively. */
    static_condvar_t cv_;       /**< @brief Where a blocked reader or writer sleeps. */
};

/** @brief The ESP-IDF chip binding of `config_t::sync_t`: the three types above. */
struct static_sync_t {
    using mutex_t = static_mutex_t;               /**< @brief The blocking lock. */
    using shared_mutex_t = static_shared_mutex_t; /**< @brief Shared readers, one writer. */
    using condvar_t = static_condvar_t;           /**< @brief Waits on a `unique_lock<mutex_t>`. */
};

}  // namespace tr::esp
