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

#include <cstring>
#include <utility>

#include "driver/gpio.h"

namespace tr::net {

twai_link_t::twai_link_t(const twai_link_config_t& config) {
    // The ISR→dispatch handoff: fixed-size copies of the seam's frame record.
    rx_queue_ = xQueueCreate(config.rx_queue_depth, sizeof(can_frame_data_t));
    if (rx_queue_ == nullptr) return;

    twai_onchip_node_config_t node_config = {};
    node_config.io_cfg.tx = static_cast<gpio_num_t>(config.tx_gpio);
    node_config.io_cfg.rx = static_cast<gpio_num_t>(config.rx_gpio);
    node_config.bit_timing.bitrate = config.bitrate;
    node_config.tx_queue_depth = config.tx_queue_depth;

    twai_node_handle_t node = nullptr;
    if (twai_new_node_onchip(&node_config, &node) != ESP_OK) {
        vQueueDelete(rx_queue_);
        rx_queue_ = nullptr;
        return;
    }

    twai_event_callbacks_t cbs = {};
    cbs.on_rx_done = &twai_link_t::on_rx_done_isr;
    if (twai_node_register_event_callbacks(node, &cbs, this) != ESP_OK ||
        twai_node_enable(node) != ESP_OK) {
        (void)twai_node_delete(node);
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
        (void)twai_node_disable(doomed);
        (void)twai_node_delete(doomed);
    }
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

    std::uint8_t data[8] = {};
    std::memcpy(data, frame.data.data(), frame.len);

    twai_frame_t tx = {};
    tx.header.id = frame.id;  // 29-bit extended id (the CAN-ID IS the path, ADR-0022)
    tx.header.ide = true;
    tx.buffer = data;
    tx.buffer_len = frame.len;
    // Bounded block: a saturated TX queue drops the frame (best-effort per the
    // seam contract), it never wedges the caller.
    (void)twai_node_transmit(node_, &tx, /*timeout_ms=*/20);
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
