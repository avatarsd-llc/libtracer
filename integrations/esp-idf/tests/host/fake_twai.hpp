/**
 * @file
 * @brief The host fake of the `esp_driver_twai` node API and the FreeRTOS
 *        primitives the TWAI link is built on — the driver its suites script.
 *
 * SPDX-License-Identifier: Apache-2.0
 * SPDX-FileCopyrightText: Copyright 2026 avatarsd LLC
 *
 * The declarations the chip TU compiles against live in `fake_idf/esp_twai*.h`,
 * the `fake_idf/freertos` headers and `fake_idf/driver/gpio.h`; this is the model behind
 * them, plus the levers a test needs.
 *
 * The behaviour it reproduces exactly, because the contracts under test rest on
 * it (`esp_driver_twai`'s node API):
 *
 *   - `twai_node_transmit` takes the frame POINTER and returns. The frame stays
 *     the driver's until the registered tx-done callback hands it back, which is
 *     why the link owns a slot pool at all (#383);
 *   - the tx-done callback is what refills the link's free-slot semaphore, so a
 *     controller that never completes a transmit — a bus-off node — is exactly a
 *     driver that never calls it. That is the default here: frames pile up in
 *     @ref frames_in_flight until a test calls @ref complete_one_tx;
 *   - `twai_node_transmit_wait_all_done` returns without completing anything, the
 *     bounded-drain answer a stalled controller gives;
 *   - on the inbound side the driver reports a frame through the rx-done callback
 *     and the callback fetches it with `twai_node_receive_from_isr`, which fills
 *     the header it parsed and copies the data field under the CALLER's
 *     `buffer_len`. A test scripts the frames that report arrives for (@ref
 *     script_rx) and says when (@ref deliver_one_rx); nothing arrives on its own.
 *
 * The counting semaphore is INSTRUMENTED, and that is the point: @ref
 * peak_tx_waiters records how many callers were parked in `xSemaphoreTake` AT
 * ONCE. A backpressure window taken while holding the link's write mutex can
 * never show more than one, however many writers are queued behind it — which is
 * what makes the #962 serialization an observable rather than a code reading.
 *
 * There is no bus, no controller and no interrupt: `complete_one_tx` and
 * `deliver_one_rx` run the driver's callbacks on the CALLING thread, which is
 * faithful in the one dimension that matters here (they run concurrently with the
 * link's own threads, and they may not block).
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "esp_twai.h"
#include "freertos/FreeRTOS.h"

namespace fake_twai {

/** @brief Drop all fake state: nodes, queues, semaphores and the instrumentation. */
void reset();

/** @brief Whether a node is currently allocated (the link's `ok()` precondition). */
[[nodiscard]] bool node_alive();

/** @brief Frames the driver holds — queued by transmit, not yet completed. */
[[nodiscard]] std::size_t frames_in_flight();

/** @brief Frames `twai_node_transmit` has ACCEPTED since @ref reset, ever.
 *
 * Survives node deletion on purpose: it is how a suite checks that a writer
 * parked across teardown never reached the controller the destructor deleted. */
[[nodiscard]] std::size_t transmits_accepted();

/**
 * @brief Complete the OLDEST in-flight frame, as the tx-done ISR would.
 * @return false when the driver holds nothing (or has no callback registered).
 */
bool complete_one_tx();

/**
 * @brief Peak number of callers parked in `xSemaphoreTake` at one moment.
 *
 * Across every counting semaphore this fake has handed out; the link creates
 * exactly one. A caller that takes a count without waiting still counts as one
 * parked caller, so the floor is 1 as soon as any write happens — the reading
 * that matters is whether it ever exceeded that.
 */
[[nodiscard]] unsigned peak_tx_waiters();

/** @brief The tick budget the most recent `xSemaphoreTake` was given. */
[[nodiscard]] TickType_t last_take_ticks();

/**
 * @brief One scripted inbound frame — the DRIVER's view of it, not the seam's.
 *
 * Deliberately spelt in the fields `twai_frame_header_t` carries rather than in a
 * `tr::net::can_frame_data_t`, because the contracts under test ARE the
 * conversion between the two: the seam's ingress admissibility rule and the
 * DLC-to-length decode with its classic-width clamp. A script written in the
 * seam's own carrier could not express the inputs that exercise them — it has no
 * way to say "11-bit identifier", "remote-transmission request", or "a DLC coding
 * more bytes than classic CAN carries".
 */
struct scripted_rx_frame_t {
    std::uint32_t id = 0;  /**< @brief Arbitration identifier, as the controller reports it. */
    bool extended = true;  /**< @brief The identifier-extension bit: a 29-bit ID when true. */
    bool remote = false;   /**< @brief The remote-transmission-request bit. */
    std::uint16_t dlc = 0; /**< @brief The RAW data-length CODE, not a byte count. */
    std::vector<std::uint8_t> data; /**< @brief The data field the controller latched. */
};

/** @brief Queue @p frame for the node to report on a later @ref deliver_one_rx. */
void script_rx(const scripted_rx_frame_t& frame);

/** @brief Scripted inbound frames the node has not reported yet. */
[[nodiscard]] std::size_t rx_scripted();

/**
 * @brief Report the OLDEST scripted inbound frame, as the rx-done ISR would.
 *
 * The mirror of @ref complete_one_tx: it runs the registered rx-done callback on
 * the CALLING thread, and that callback is what fetches the frame back through
 * `twai_node_receive_from_isr`.
 *
 * @return false when nothing is scripted (or no callback is registered).
 */
bool deliver_one_rx();

}  // namespace fake_twai
