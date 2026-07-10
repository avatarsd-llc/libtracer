/**
 * @file
 * @brief `twai_link_t` implementation — see include/libtracer_esp/twai_link.hpp.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * Chip-target-only TU, selected by the component CMakeLists (never an
 * in-source #ifdef).
 */

#include "libtracer_esp/twai_link.hpp"

#include <cstddef>
#include <cstring>
#include <type_traits>
#include <utility>

#include "driver/gpio.h"

namespace tr::net {

namespace {

/**
 * @brief ms → RTOS ticks, floored to ONE tick for any non-zero timeout.
 *
 * pdMS_TO_TICKS truncates: at the default 100 Hz tick a 1–9 ms timeout rounds
 * to 0 ticks — a silent non-blocking take that would void the backpressure
 * window entirely.
 */
TickType_t tx_wait_ticks(std::uint32_t ms) {
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return (ticks == 0 && ms != 0) ? 1 : ticks;
}

}  // namespace

twai_link_t::twai_link_t(const twai_link_config_t& config)
    // Pool = the driver's maximum in-flight frame count: its queue plus the
    // hardware TX slot. A free pool slot therefore implies driver-side room,
    // so twai_node_transmit below never needs the driver's own (queue-full)
    // wait — the pool semaphore is the ONE backpressure point.
    : tx_pool_(config.tx_queue_depth + 1), tx_timeout_ms_(config.tx_timeout_ms) {
    // The ISR→dispatch handoff: fixed-size copies of the seam's frame record.
    rx_queue_ = xQueueCreate(config.rx_queue_depth, sizeof(can_frame_data_t));
    if (rx_queue_ == nullptr) return;

    // Free-slot gate for the TX pool, given back from the tx-done ISR.
    const auto slots = static_cast<UBaseType_t>(tx_pool_.capacity());
    tx_free_sem_ = xSemaphoreCreateCounting(slots, slots);
    if (tx_free_sem_ == nullptr) {
        vQueueDelete(rx_queue_);
        rx_queue_ = nullptr;
        return;
    }

    twai_onchip_node_config_t node_config = {};
    node_config.io_cfg.tx = static_cast<gpio_num_t>(config.tx_gpio);
    node_config.io_cfg.rx = static_cast<gpio_num_t>(config.rx_gpio);
    node_config.bit_timing.bitrate = config.bitrate;
    node_config.tx_queue_depth = config.tx_queue_depth;

    twai_node_handle_t node = nullptr;
    if (twai_new_node_onchip(&node_config, &node) != ESP_OK) {
        vSemaphoreDelete(tx_free_sem_);
        tx_free_sem_ = nullptr;
        vQueueDelete(rx_queue_);
        rx_queue_ = nullptr;
        return;
    }

    twai_event_callbacks_t cbs = {};
    cbs.on_rx_done = &twai_link_t::on_rx_done_isr;
    cbs.on_tx_done = &twai_link_t::on_tx_done_isr;
    if (twai_node_register_event_callbacks(node, &cbs, this) != ESP_OK ||
        twai_node_enable(node) != ESP_OK) {
        (void)twai_node_delete(node);
        vSemaphoreDelete(tx_free_sem_);
        tx_free_sem_ = nullptr;
        vQueueDelete(rx_queue_);
        rx_queue_ = nullptr;
        return;
    }

    node_ = node;
    thread_ = std::thread([this] { run(); });
}

twai_link_t::~twai_link_t() {
    stop_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
    // Reset the node under the write lock before teardown so an in-flight
    // write_raw never touches a disabled/deleted controller.
    twai_node_handle_t doomed = nullptr;
    {
        const std::lock_guard lock(write_m_);
        doomed = node_;
        node_ = nullptr;
    }
    if (doomed != nullptr) {
        // Bounded drain: let queued frames reach the wire (and their tx-done
        // releases fire) before the controller — and the pool slots the
        // driver still points into — go away.
        (void)twai_node_transmit_wait_all_done(doomed, /*timeout_ms=*/100);
        (void)twai_node_disable(doomed);
        (void)twai_node_delete(doomed);
    }
    if (tx_free_sem_ != nullptr) vSemaphoreDelete(tx_free_sem_);
    if (rx_queue_ != nullptr) vQueueDelete(rx_queue_);
}

void twai_link_t::on_receive(rx_fn_t rx) {
    const std::lock_guard lock(m_);
    rx_ = std::move(rx);
}

void twai_link_t::write_raw(const can_frame_data_t& frame) {
    // TWAI is classic CAN only: an FD frame (or an over-8-byte payload) is
    // dropped best-effort, mirroring the RX side's skip-and-continue policy.
    if (frame.fd || frame.len > 8) return;

    const std::lock_guard lock(write_m_);
    if (node_ == nullptr) return;

    // FULL policy (#383): bounded backpressure, then a COUNTED drop. A
    // continuation burst deeper than the driver queue parks here until the
    // tx-done ISR frees a slot — it no longer silently loses frames; only a
    // wait that outlives tx_timeout_ms_ drops, and that drop is observable
    // via tx_dropped().
    if (xSemaphoreTake(tx_free_sem_, tx_wait_ticks(tx_timeout_ms_)) != pdTRUE) {
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    tx_slot_t* slot = tx_pool_.try_acquire();
    if (slot == nullptr) {  // unreachable: the semaphore counts free slots
        (void)xSemaphoreGive(tx_free_sem_);
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // The driver holds the frame POINTER until the transmit completes (the
    // data field is formatted when the frame reaches hardware, possibly from
    // the tx-done ISR), so the descriptor and payload live in the link-owned
    // pool slot — never on this call's stack (#383). on_tx_done_isr releases
    // the slot.
    std::memset(slot->data, 0, sizeof(slot->data));
    std::memcpy(slot->data, frame.data.data(), frame.len);
    slot->frame = {};
    slot->frame.header.id = frame.id;  // 29-bit extended id (the CAN-ID IS the path, ADR-0022)
    slot->frame.header.ide = true;
    slot->frame.buffer = slot->data;
    slot->frame.buffer_len = frame.len;
    // A held pool slot implies driver-side room (pool = queue + hw slot), so
    // no driver-side wait; a rejection here is a driver/state error, not
    // queue-full — hand the slot straight back and count the drop.
    if (twai_node_transmit(node_, &slot->frame, /*timeout_ms=*/0) != ESP_OK) {
        tx_pool_.release(slot);
        (void)xSemaphoreGive(tx_free_sem_);
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
    }
}

bool twai_link_t::on_rx_done_isr(twai_node_handle_t node, const twai_rx_done_event_data_t* edata,
                                 void* user_ctx) {
    (void)edata;
    auto* self = static_cast<twai_link_t*>(user_ctx);

    // Copy the frame out of the driver inside the ISR window (required by the
    // node API), then queue it for the dispatch thread — no user code here.
    std::uint8_t buf[8] = {};
    twai_frame_t rx = {};
    rx.buffer = buf;
    rx.buffer_len = sizeof(buf);
    if (twai_node_receive_from_isr(node, &rx) != ESP_OK) return false;
    if (!rx.header.ide || rx.header.rtr) return false;  // 29-bit data frames only

    can_frame_data_t out;
    out.id = rx.header.id & 0x1FFFFFFFu;
    out.fd = false;
    out.len = static_cast<std::uint8_t>(twaifd_dlc2len(rx.header.dlc));
    if (out.len > 8) out.len = 8;
    std::memcpy(out.data.data(), buf, out.len);

    BaseType_t hp_task_woken = pdFALSE;
    // A full queue drops the frame (backpressure, never a block in ISR).
    (void)xQueueSendFromISR(self->rx_queue_, &out, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

bool twai_link_t::on_tx_done_isr(twai_node_handle_t node, const twai_tx_done_event_data_t* edata,
                                 void* user_ctx) {
    (void)node;
    // The cast below recovers the owning slot from the driver's frame pointer;
    // it is only valid while `frame` is the first member of a standard-layout
    // slot (pointer-interconvertible).
    static_assert(std::is_standard_layout_v<tx_slot_t>);
    static_assert(offsetof(tx_slot_t, frame) == 0);
    auto* self = static_cast<twai_link_t*>(user_ctx);

    // done_tx_frame IS &slot->frame of the pool slot write_raw submitted;
    // success or not, the driver is finished with it — return it and wake one
    // backpressured writer. No allocation, no locks: the pool release is a
    // single store-release, ISR-safe by construction.
    auto* slot = reinterpret_cast<tx_slot_t*>(const_cast<twai_frame_t*>(edata->done_tx_frame));
    self->tx_pool_.release(slot);

    BaseType_t hp_task_woken = pdFALSE;
    (void)xSemaphoreGiveFromISR(self->tx_free_sem_, &hp_task_woken);
    return hp_task_woken == pdTRUE;
}

void twai_link_t::run() {
    can_frame_data_t frame;
    while (!stop_.load(std::memory_order_relaxed)) {
        // A bounded wait keeps the stop_ poll responsive (the SO_RCVTIMEO idiom).
        if (xQueueReceive(rx_queue_, &frame, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        rx_fn_t rx;
        {
            const std::lock_guard lock(m_);
            rx = rx_;
        }
        if (rx) rx(frame);
    }
}

}  // namespace tr::net
