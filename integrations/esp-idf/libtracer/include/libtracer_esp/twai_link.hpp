/*
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * twai_link_t — the ESP-IDF implementation of libtracer's `can_link_t` seam
 * over the on-chip TWAI controller (ESP-IDF's CAN 2.0 peripheral, driven by the
 * `esp_driver_twai` node API). The seam is what makes `tr::net::transport_can`
 * portable: the framing / advertise / reassembly layers above it are pure and
 * host-tested (core/tests/transport_can_test.cpp over a fake link); this class
 * only moves raw classic-CAN frames. It lives in the ESP-IDF component tree —
 * NOT in core/ — because it needs IDF headers; the build system selects it for
 * chip targets (integrations/esp-idf/libtracer/CMakeLists.txt), never a macro.
 *
 * TWAI is CLASSIC CAN only (no CAN-FD): pair it with a `transport_can` whose
 * `transport_can_config_t::mode` is `can_frame_mode_t::CLASSIC`. Frames flagged
 * `fd` are dropped on write (mirroring the link contract's best-effort drop).
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "libtracer/transport_can.hpp"

namespace tr::net {

/**
 * @brief Static configuration of a @ref twai_link_t — pins, bitrate, depths.
 */
struct twai_link_config_t {
    int tx_gpio = -1;                  /**< @brief TWAI TX pin (to the transceiver). */
    int rx_gpio = -1;                  /**< @brief TWAI RX pin (from the transceiver). */
    std::uint32_t bitrate = 500000;    /**< @brief Bus bitrate in bit/s (classic CAN). */
    std::uint32_t tx_queue_depth = 8;  /**< @brief Driver-side TX queue depth. */
    std::uint32_t rx_queue_depth = 16; /**< @brief ISR→dispatch RX queue depth (frames). */
};

/**
 * @brief The ESP TWAI `can_link_t` — classic CAN 2.0, 29-bit extended IDs.
 *
 * Mirrors `socketcan_link_t`'s contract: single-owner, best-effort per-frame
 * writes serialized under a lock, inbound frames delivered through the
 * registered @ref can_link_t::rx_fn_t on an internal dispatch thread. The
 * driver's RX-done callback runs in ISR context, so it only copies the frame
 * into a FreeRTOS queue; the dispatch thread (a plain `std::thread`, i.e. a
 * pthread task on FreeRTOS) pops and invokes the callback — user code never
 * runs in the ISR. @ref ok is false if the controller failed to come up.
 */
class twai_link_t : public can_link_t {
   public:
    /**
     * @brief Bring the on-chip TWAI controller up on @p config's pins/bitrate.
     * @param config Pins, bitrate, and queue depths.
     */
    explicit twai_link_t(const twai_link_config_t& config);

    /** @brief Stop the dispatch thread, disable and delete the TWAI node. */
    ~twai_link_t() override;

    twai_link_t(const twai_link_t&) = delete;
    twai_link_t& operator=(const twai_link_t&) = delete;

    /**
     * @brief Write one classic CAN frame to the bus (best-effort, serialized).
     *
     * CAN-FD frames (`frame.fd == true`) and payloads over 8 bytes are dropped —
     * TWAI is classic-only; configure the owning `transport_can` as CLASSIC.
     */
    void write_raw(const can_frame_data_t& frame) override;

    /** @brief Register the inbound-frame sink (invoked on the dispatch thread). */
    void on_receive(rx_fn_t rx) override;

    /** @brief True if the controller enabled and the RX machinery is live. */
    [[nodiscard]] bool ok() const noexcept { return node_ != nullptr; }

   private:
    // ISR-context RX-done hook: copy the frame out of the driver and queue it.
    static bool on_rx_done_isr(twai_node_handle_t node, const twai_rx_done_event_data_t* edata,
                               void* user_ctx);
    void run();  // dispatch thread: queue -> registered rx_fn_t

    twai_node_handle_t node_ = nullptr;
    QueueHandle_t rx_queue_ = nullptr;  // of can_frame_data_t, ISR -> dispatch
    rx_fn_t rx_;                        // guarded by m_
    std::mutex m_;                      // guards rx_
    std::mutex write_m_;                // serializes writes / teardown
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace tr::net
