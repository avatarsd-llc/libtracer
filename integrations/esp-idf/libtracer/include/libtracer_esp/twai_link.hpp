/**
 * @file
 * @brief `twai_link_t` — the ESP-IDF `can_link_t` over the on-chip TWAI
 *        controller.
 *
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
 *
 * TX ownership (#383): `esp_driver_twai` transmits ASYNCHRONOUSLY — it queues
 * the `twai_frame_t` POINTER and formats the data field only when the frame
 * reaches the hardware, possibly from the tx-done ISR. The link therefore owns
 * every in-flight frame in a `can_tx_pool_t` slot until the driver's tx-done
 * callback hands it back; write_raw never gives the driver stack storage.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "libtracer/can_tx_pool.hpp"
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
    std::uint32_t tx_timeout_ms = 20;  /**< @brief Bounded wait for a free in-flight TX slot
                                            (the FULL policy's backpressure window); an
                                            expired wait is a COUNTED drop, @ref
                                            twai_link_t::tx_dropped. */
    std::size_t stack_size = 0;        /**< @brief Dispatch-thread stack size in bytes, 0 =
                                            the global pthread default. Non-zero right-sizes
                                            this task via `esp_pthread_set_cfg` instead of
                                            inflating `CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT`
                                            for every pthread — the RAM lever (libtracer #486).
                                            Size to the measured high-water mark plus margin. */
};

/**
 * @brief The ESP TWAI `can_link_t` — classic CAN 2.0, 29-bit extended IDs.
 *
 * Mirrors `socketcan_link_t`'s contract: single-owner, per-frame writes
 * serialized under a lock, inbound frames delivered through the registered
 * @ref can_link_t::rx_fn_t on an internal dispatch thread. The driver's
 * RX-done callback runs in ISR context, so it only copies the frame into a
 * FreeRTOS queue; the dispatch thread (a plain `std::thread`, i.e. a pthread
 * task on FreeRTOS) pops and invokes the callback — user code never runs in
 * the ISR. @ref ok is false if the controller failed to come up.
 *
 * TX path (#383): each outbound frame is copied into a link-owned
 * @ref can_tx_pool_t slot that stays in flight until the driver's tx-done ISR
 * releases it — the driver only ever holds pointers into the pool, never the
 * caller's stack. The pool is sized `tx_queue_depth + 1` (the driver queue
 * plus the hardware slot), and a counting semaphore given back from the
 * tx-done ISR gates writers: a continuation burst deeper than the driver
 * queue BLOCKS (bounded by `tx_timeout_ms`) instead of silently losing
 * frames; only a wait that expires drops — counted, visible via
 * @ref tx_dropped.
 */
class twai_link_t : public can_link_t {
   public:
    /**
     * @brief Bring the on-chip TWAI controller up on @p config's pins/bitrate.
     * @param config Pins, bitrate, queue depths, and the TX FULL-policy window.
     */
    explicit twai_link_t(const twai_link_config_t& config);

    /** @brief Drain in-flight TX (bounded), stop dispatch, delete the node. */
    ~twai_link_t() override;

    twai_link_t(const twai_link_t&) = delete;
    twai_link_t& operator=(const twai_link_t&) = delete;

    /**
     * @brief Write one classic CAN frame to the bus (serialized, bounded wait).
     *
     * CAN-FD frames (`frame.fd == true`) and payloads over 8 bytes are dropped —
     * TWAI is classic-only; configure the owning `transport_can` as CLASSIC.
     * When every in-flight slot is taken the call blocks up to
     * `tx_timeout_ms` for the tx-done ISR to free one (backpressure); an
     * expired wait drops the frame and increments @ref tx_dropped.
     */
    void write_raw(const can_frame_data_t& frame) override;

    /** @brief Register the inbound-frame sink (invoked on the dispatch thread). */
    void on_receive(rx_fn_t rx) override;

    /** @brief True if the controller enabled and the RX machinery is live. */
    [[nodiscard]] bool ok() const noexcept { return node_ != nullptr; }

    /**
     * @brief Frames dropped by the TX FULL policy since construction.
     *
     * Counts writes that timed out waiting for an in-flight slot plus writes
     * the driver rejected — the #383 observability guarantee: a saturated TX
     * path is never a SILENT loss.
     */
    [[nodiscard]] std::uint32_t tx_dropped() const noexcept {
        return tx_dropped_.load(std::memory_order_relaxed);
    }

   private:
    /**
     * @brief One link-owned in-flight TX frame: the driver-visible descriptor
     *        plus the payload it points at (see the #383 file-header note).
     */
    struct tx_slot_t {
        twai_frame_t frame{};   /**< @brief The descriptor handed to twai_node_transmit. */
        std::uint8_t data[8]{}; /**< @brief The payload `frame.buffer` points at. */
    };

    /** @brief ISR-context RX-done hook: copy the frame out of the driver and queue it. */
    static bool on_rx_done_isr(twai_node_handle_t node, const twai_rx_done_event_data_t* edata,
                               void* user_ctx);
    /** @brief ISR-context TX-done hook: return the finished frame's slot to the pool. */
    static bool on_tx_done_isr(twai_node_handle_t node, const twai_tx_done_event_data_t* edata,
                               void* user_ctx);
    /** @brief Dispatch thread: queue -> registered rx_fn_t. */
    void run();

    twai_node_handle_t node_ = nullptr;
    QueueHandle_t rx_queue_ = nullptr;        /**< @brief Of can_frame_data_t, ISR -> dispatch. */
    rx_fn_t rx_;                              /**< @brief Guarded by m_. */
    std::mutex m_;                            /**< @brief Guards rx_. */
    std::mutex write_m_;                      /**< @brief Serializes writes / teardown. */
    can_tx_pool_t<tx_slot_t> tx_pool_;        /**< @brief In-flight TX storage, freed on tx-done. */
    SemaphoreHandle_t tx_free_sem_ = nullptr; /**< @brief Counts free pool slots; taken in
                                                   write_raw, given from the tx-done ISR. */
    std::uint32_t tx_timeout_ms_ = 20;        /**< @brief The bounded backpressure window. */
    std::atomic<std::uint32_t> tx_dropped_{0}; /**< @brief FULL-policy drop counter. */
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace tr::net
