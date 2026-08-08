/**
 * @file
 * @brief The model behind fake_twai.hpp — the TWAI node, the FreeRTOS queue and
 *        counting semaphore, and the `esp_pthread` stack-sizing seam.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Host threads stand in for FreeRTOS tasks, so a tick is wall time: a
 * `std::condition_variable` bounded wait is the faithful shape of a blocked task
 * that a give (from a "task" or from the "ISR") releases early.
 */

#include "fake_twai.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "driver/gpio.h"
#include "esp_pthread.h"
#include "esp_twai_onchip.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace {

/** @brief One scheduler tick in wall time, at the fake's configTICK_RATE_HZ. */
constexpr std::chrono::milliseconds kTickPeriod{1000 / configTICK_RATE_HZ};

/** @brief Ticks to a wall-clock duration. */
std::chrono::milliseconds ticks_to_wall(TickType_t ticks) { return kTickPeriod * ticks; }

/** @brief Peak simultaneous `xSemaphoreTake` waiters since the last reset. */
std::atomic<unsigned> g_peak_waiters{0};
/** @brief The tick budget the most recent `xSemaphoreTake` was given. */
std::atomic<TickType_t> g_last_take_ticks{0};
/** @brief Frames `twai_node_transmit` accepted since the last reset, ever. */
std::atomic<std::size_t> g_transmits_accepted{0};

/** @brief Raise @ref g_peak_waiters to @p seen if it is higher. */
void observe_waiters(unsigned seen) {
    unsigned peak = g_peak_waiters.load(std::memory_order_relaxed);
    while (seen > peak &&
           !g_peak_waiters.compare_exchange_weak(peak, seen, std::memory_order_relaxed)) {
    }
}

}  // namespace

/** @brief A FreeRTOS counting semaphore: a count, a bound, and its parked callers. */
struct fake_semaphore_t {
    std::mutex m;               /**< @brief Guards the count and the waiter tally. */
    std::condition_variable cv; /**< @brief Signalled by every give. */
    unsigned count = 0;         /**< @brief Counts currently available. */
    unsigned max_count = 0;     /**< @brief The ceiling a give will not pass. */
    unsigned waiters = 0;       /**< @brief Callers inside xSemaphoreTake right now. */
};

/** @brief A FreeRTOS queue of fixed-size items with a bounded depth. */
struct fake_queue_t {
    std::mutex m;                                 /**< @brief Guards the ring. */
    std::condition_variable cv;                   /**< @brief Signalled by every send. */
    std::size_t item_size = 0;                    /**< @brief Bytes per item. */
    std::size_t capacity = 0;                     /**< @brief Maximum items held. */
    std::deque<std::vector<unsigned char>> items; /**< @brief The ring itself. */
};

/** @brief A TWAI node: its callbacks and the frame POINTERS the driver is holding. */
struct twai_node_base {
    std::mutex m;                           /**< @brief Guards the queues and callbacks. */
    bool enabled = false;                   /**< @brief Set by twai_node_enable. */
    twai_event_callbacks_t cbs{};           /**< @brief What the link registered. */
    void* ctx = nullptr;                    /**< @brief Its user context (the link). */
    std::deque<const twai_frame_t*> queued; /**< @brief Frames not yet completed. */
    std::deque<fake_twai::scripted_rx_frame_t>
        inbound; /**< @brief Scripted frames not yet reported. */
};

namespace {

/** @brief The one node this fake hands out, or nullptr when none is allocated. */
twai_node_base* g_node = nullptr;
/** @brief Guards @ref g_node's identity (not its contents). */
std::mutex g_node_m;

}  // namespace

// --- FreeRTOS: counting semaphore -------------------------------------------------

SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max_count, UBaseType_t initial_count) {
    auto* s = new fake_semaphore_t();
    s->max_count = static_cast<unsigned>(max_count);
    s->count = static_cast<unsigned>(initial_count);
    return s;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore) { delete semaphore; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks_to_wait) {
    g_last_take_ticks.store(ticks_to_wait, std::memory_order_relaxed);
    std::unique_lock lock(semaphore->m);
    ++semaphore->waiters;
    observe_waiters(semaphore->waiters);
    const bool got = semaphore->cv.wait_for(lock, ticks_to_wall(ticks_to_wait),
                                            [semaphore] { return semaphore->count > 0; });
    --semaphore->waiters;
    if (!got) return pdFALSE;
    --semaphore->count;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    {
        const std::lock_guard lock(semaphore->m);
        if (semaphore->count >= semaphore->max_count) return pdFALSE;
        ++semaphore->count;
    }
    semaphore->cv.notify_one();
    return pdTRUE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t semaphore,
                                 BaseType_t* higher_priority_task_woken) {
    if (higher_priority_task_woken != nullptr) *higher_priority_task_woken = pdFALSE;
    return xSemaphoreGive(semaphore);
}

// --- FreeRTOS: queue --------------------------------------------------------------

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    auto* q = new fake_queue_t();
    q->capacity = static_cast<std::size_t>(length);
    q->item_size = static_cast<std::size_t>(item_size);
    return q;
}

void vQueueDelete(QueueHandle_t queue) { delete queue; }

BaseType_t xQueueReceive(QueueHandle_t queue, void* dst, TickType_t ticks_to_wait) {
    std::unique_lock lock(queue->m);
    const bool got = queue->cv.wait_for(lock, ticks_to_wall(ticks_to_wait),
                                        [queue] { return !queue->items.empty(); });
    if (!got) return pdFALSE;
    std::memcpy(dst, queue->items.front().data(), queue->item_size);
    queue->items.pop_front();
    return pdTRUE;
}

BaseType_t xQueueSendFromISR(QueueHandle_t queue, const void* item,
                             BaseType_t* higher_priority_task_woken) {
    if (higher_priority_task_woken != nullptr) *higher_priority_task_woken = pdFALSE;
    {
        const std::lock_guard lock(queue->m);
        if (queue->items.size() >= queue->capacity) return pdFALSE;
        const auto* bytes = static_cast<const unsigned char*>(item);
        queue->items.emplace_back(bytes, bytes + queue->item_size);
    }
    queue->cv.notify_one();
    return pdTRUE;
}

// --- FreeRTOS: task ---------------------------------------------------------------

void vTaskDelay(TickType_t ticks_to_delay) {
    std::this_thread::sleep_for(ticks_to_wall(ticks_to_delay));
}

// --- esp_pthread: the stack-sizing seam -------------------------------------------
//
// The TWAI link reaches it through esp_thread.hpp. Nothing here resizes a host
// thread (glibc sizes through pthread_attr_t, not a thread-local config); these
// exist so a link constructed with a non-zero stack_size links and behaves. The
// WS client suite proves the ARMING contract on its own fake, which records it.

namespace {
/** @brief The config armed on this thread, if any. */
thread_local esp_pthread_cfg_t g_pthread_cfg{};
/** @brief Whether @ref g_pthread_cfg has ever been set on this thread. */
thread_local bool g_pthread_cfg_set = false;
}  // namespace

esp_pthread_cfg_t esp_pthread_get_default_config(void) {
    esp_pthread_cfg_t cfg{};
    cfg.stack_size = 4096;
    return cfg;
}

esp_err_t esp_pthread_set_cfg(const esp_pthread_cfg_t* cfg) {
    if (cfg == nullptr || cfg->stack_size == 0) return ESP_ERR_INVALID_ARG;
    g_pthread_cfg = *cfg;
    g_pthread_cfg_set = true;
    return ESP_OK;
}

esp_err_t esp_pthread_get_cfg(esp_pthread_cfg_t* p) {
    if (p == nullptr) return ESP_ERR_INVALID_ARG;
    if (!g_pthread_cfg_set) {
        // IDF's own behaviour, and the reason esp_thread.hpp checks the return
        // code: a thread with no config gets its out-param ZEROED, not left alone.
        *p = esp_pthread_cfg_t{};
        return ESP_ERR_NOT_FOUND;
    }
    *p = g_pthread_cfg;
    return ESP_OK;
}

// --- esp_driver_twai: the node API ------------------------------------------------

esp_err_t twai_new_node_onchip(const twai_onchip_node_config_t* node_config,
                               twai_node_handle_t* node_ret) {
    if (node_config == nullptr || node_ret == nullptr) return ESP_ERR_INVALID_ARG;
    const std::lock_guard lock(g_node_m);
    if (g_node != nullptr) return ESP_ERR_INVALID_STATE;
    g_node = new twai_node_base();
    *node_ret = g_node;
    return ESP_OK;
}

esp_err_t twai_node_register_event_callbacks(twai_node_handle_t node,
                                             const twai_event_callbacks_t* cbs, void* user_data) {
    if (node == nullptr || cbs == nullptr) return ESP_ERR_INVALID_ARG;
    const std::lock_guard lock(node->m);
    node->cbs = *cbs;
    node->ctx = user_data;
    return ESP_OK;
}

esp_err_t twai_node_enable(twai_node_handle_t node) {
    if (node == nullptr) return ESP_ERR_INVALID_ARG;
    const std::lock_guard lock(node->m);
    node->enabled = true;
    return ESP_OK;
}

esp_err_t twai_node_disable(twai_node_handle_t node) {
    if (node == nullptr) return ESP_ERR_INVALID_ARG;
    const std::lock_guard lock(node->m);
    node->enabled = false;
    return ESP_OK;
}

esp_err_t twai_node_delete(twai_node_handle_t node) {
    if (node == nullptr) return ESP_ERR_INVALID_ARG;
    const std::lock_guard lock(g_node_m);
    if (g_node == node) g_node = nullptr;
    delete node;
    return ESP_OK;
}

esp_err_t twai_node_transmit(twai_node_handle_t node, const twai_frame_t* frame, int timeout_ms) {
    (void)timeout_ms;  // the link always passes 0; a held pool slot implies room
    if (node == nullptr || frame == nullptr) return ESP_ERR_INVALID_ARG;
    const std::lock_guard lock(node->m);
    if (!node->enabled) return ESP_ERR_INVALID_STATE;
    node->queued.push_back(frame);
    g_transmits_accepted.fetch_add(1, std::memory_order_relaxed);
    return ESP_OK;
}

esp_err_t twai_node_transmit_wait_all_done(twai_node_handle_t node, int timeout_ms) {
    (void)timeout_ms;
    if (node == nullptr) return ESP_ERR_INVALID_ARG;
    // A stalled controller completes nothing in the window: the bounded drain
    // returns having drained nothing, which is the case the suites stage.
    const std::lock_guard lock(node->m);
    return node->queued.empty() ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t twai_node_receive_from_isr(twai_node_handle_t node, twai_frame_t* rx_frame) {
    if (node == nullptr || rx_frame == nullptr) return ESP_ERR_INVALID_ARG;

    fake_twai::scripted_rx_frame_t got;
    {
        const std::lock_guard lock(node->m);
        // The failure IDF gives when the callback fetches with nothing pending.
        if (node->inbound.empty()) return ESP_FAIL;
        got = std::move(node->inbound.front());
        node->inbound.pop_front();
    }

    // The header the HAL parsed, exactly as it parsed it: the DLC is the RAW code
    // off the wire, not a byte count. Checked in ESP-IDF's esp32/ and esp32c3/
    // hal/twai_ll.h: twai_ll_parse_frame_header does `header->dlc = rx_frame->dlc`,
    // the untouched 4-bit field, and only the separate DATA copy is clamped to 8.
    // A bus frame coding 9..15 therefore does reach the link as a code
    // twaifd_dlc2len expands past the classic width — the input the link's own
    // clamp is there for. Only the fields the TU reads are carried; the rest zeroed.
    rx_frame->header = {};
    rx_frame->header.id = got.id;
    rx_frame->header.dlc = got.dlc;
    rx_frame->header.ide = got.extended ? 1U : 0U;
    rx_frame->header.rtr = got.remote ? 1U : 0U;

    // twai_hal_parse_frame copies NOTHING for a remote frame — an RTR carries a DLC
    // but no data field — and otherwise fills the caller's buffer under its own
    // buffer_len. The v2 HAL's extra MIN against the mode's width is subsumed here:
    // the link hands in an 8-byte buffer, which is the classic width already.
    if (!got.remote && rx_frame->buffer != nullptr) {
        std::size_t n = twaifd_dlc2len(got.dlc);
        if (n > rx_frame->buffer_len) n = rx_frame->buffer_len;
        if (n > got.data.size()) n = got.data.size();
        if (n != 0) std::memcpy(rx_frame->buffer, got.data.data(), n);
    }
    return ESP_OK;
}

// --- The levers ------------------------------------------------------------------

namespace fake_twai {

void reset() {
    const std::lock_guard lock(g_node_m);
    delete g_node;
    g_node = nullptr;
    g_peak_waiters.store(0, std::memory_order_relaxed);
    g_last_take_ticks.store(0, std::memory_order_relaxed);
    g_transmits_accepted.store(0, std::memory_order_relaxed);
}

bool node_alive() {
    const std::lock_guard lock(g_node_m);
    return g_node != nullptr;
}

std::size_t frames_in_flight() {
    const std::lock_guard lock(g_node_m);
    if (g_node == nullptr) return 0;
    const std::lock_guard node_lock(g_node->m);
    return g_node->queued.size();
}

std::size_t transmits_accepted() { return g_transmits_accepted.load(std::memory_order_relaxed); }

bool complete_one_tx() {
    twai_node_base* node = nullptr;
    {
        const std::lock_guard lock(g_node_m);
        node = g_node;
    }
    if (node == nullptr) return false;

    const twai_frame_t* done = nullptr;
    twai_event_callbacks_t cbs{};
    void* ctx = nullptr;
    {
        const std::lock_guard lock(node->m);
        if (node->queued.empty() || node->cbs.on_tx_done == nullptr) return false;
        done = node->queued.front();
        node->queued.pop_front();
        cbs = node->cbs;
        ctx = node->ctx;
    }
    twai_tx_done_event_data_t edata{};
    edata.is_tx_success = true;
    edata.done_tx_frame = done;
    // Outside the node lock, exactly as the driver's ISR is outside it: the
    // callback releases a pool slot and gives the link's semaphore.
    (void)cbs.on_tx_done(node, &edata, ctx);
    return true;
}

unsigned peak_tx_waiters() { return g_peak_waiters.load(std::memory_order_relaxed); }

TickType_t last_take_ticks() { return g_last_take_ticks.load(std::memory_order_relaxed); }

void script_rx(const scripted_rx_frame_t& frame) {
    const std::lock_guard lock(g_node_m);
    if (g_node == nullptr) return;
    const std::lock_guard node_lock(g_node->m);
    g_node->inbound.push_back(frame);
}

std::size_t rx_scripted() {
    const std::lock_guard lock(g_node_m);
    if (g_node == nullptr) return 0;
    const std::lock_guard node_lock(g_node->m);
    return g_node->inbound.size();
}

bool deliver_one_rx() {
    twai_node_base* node = nullptr;
    {
        const std::lock_guard lock(g_node_m);
        node = g_node;
    }
    if (node == nullptr) return false;

    twai_event_callbacks_t cbs{};
    void* ctx = nullptr;
    {
        const std::lock_guard lock(node->m);
        if (node->inbound.empty() || node->cbs.on_rx_done == nullptr) return false;
        cbs = node->cbs;
        ctx = node->ctx;
    }
    twai_rx_done_event_data_t edata{};
    // Outside the node lock, exactly as the driver's ISR is: the callback reaches
    // straight back in through twai_node_receive_from_isr to take the frame, and
    // then hands it to the link's ISR->dispatch queue.
    (void)cbs.on_rx_done(node, &edata, ctx);
    return true;
}

}  // namespace fake_twai
