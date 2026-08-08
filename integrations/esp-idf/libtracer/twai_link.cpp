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
#include "esp_thread.hpp"
#include "freertos/task.h"

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

/**
 * @brief The task-watchdog period, seconds — the ceiling on ONE frame's TX wait.
 *
 * Not a number of ours: `CONFIG_ESP_TASK_WDT_TIMEOUT_S` is the system's own
 * normative statement of how long a task may go unfed. IDF defines it through
 * `sdkconfig.h` (pulled in by FreeRTOS.h); the fallback is IDF's own Kconfig
 * default for that symbol, for a build with the task watchdog compiled out and
 * for the host test build, so the derivation has one provenance on every
 * target. `httpd_ws_link.cpp` reads the same symbol for its own send bound
 * (#835) — the two derivations differ (that one divides by a fan-out), so this
 * is the shared FACT, not a shared formula.
 */
#ifdef CONFIG_ESP_TASK_WDT_TIMEOUT_S
constexpr std::uint32_t kTaskWdtSeconds = CONFIG_ESP_TASK_WDT_TIMEOUT_S;
#else
constexpr std::uint32_t kTaskWdtSeconds = 5;
#endif

/**
 * @brief Clamp @p want, one frame's backpressure window, to the watchdog period.
 *
 * `twai_link_config_t::tx_timeout_ms` is caller-supplied and was previously
 * taken verbatim, so a config could ask a writing task to park longer than the
 * watchdog tolerates and reboot the board instead of dropping the frame (#962).
 * The bound this states is exactly the one it can state: ONE frame's wait fits
 * inside one watchdog window. A caller that writes a burst on one task still
 * sums its own frames' waits — that product is the caller's, not this knob's.
 */
[[nodiscard]] constexpr std::uint32_t clamp_tx_timeout_ms(std::uint32_t want) noexcept {
    constexpr std::uint32_t ceiling = kTaskWdtSeconds * 1000U;
    return want < ceiling ? want : ceiling;
}

/**
 * @brief RAII half of the parked-writer tally `write_raw` raises (#962).
 *
 * Only the DECREMENT lives here. The raise cannot: it has to share the
 * destructor's `write_m_` critical section with the `node_ == nullptr` check,
 * because that pairing is what fixes the set of writers the destructor must
 * wait out before it deletes `tx_free_sem_`. A writer that raises the tally
 * under the lock is one the destructor will see; a writer that arrives after
 * the destructor nulled `node_` takes the same lock, sees the gone node, and
 * leaves without ever touching the semaphore.
 */
class tx_writer_exit_t {
   public:
    /** @brief Adopt an ALREADY-raised @p tally; lower it on scope exit. */
    explicit tx_writer_exit_t(std::atomic<std::uint32_t>& tally) noexcept : tally_(tally) {}
    /** @brief Lower the tally, releasing anything the destructor's drain observes. */
    ~tx_writer_exit_t() { tally_.fetch_sub(1, std::memory_order_release); }

    tx_writer_exit_t(const tx_writer_exit_t&) = delete;
    tx_writer_exit_t& operator=(const tx_writer_exit_t&) = delete;

   private:
    std::atomic<std::uint32_t>& tally_; /**< @brief twai_link_t::tx_writers_. */
};

}  // namespace

twai_link_t::twai_link_t(const twai_link_config_t& config)
    // Pool = the driver's maximum in-flight frame count: its queue plus the
    // hardware TX slot. A free pool slot therefore implies driver-side room,
    // so twai_node_transmit below never needs the driver's own (queue-full)
    // wait — the pool semaphore is the ONE backpressure point.
    : tx_pool_(config.tx_queue_depth + 1),
      tx_timeout_ms_(clamp_tx_timeout_ms(config.tx_timeout_ms)) {
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

    // Right-size the dispatch thread's FreeRTOS task stack without inflating the global
    // CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT (libtracer #486). The save/set/spawn/restore
    // recipe this used to spell out in place now lives in esp_thread.hpp, shared with the
    // WS client link — which accepted the same knob and discarded it (#900).
    thread_ = esp::spawn_thread(config.stack_size, "twai_rx", [this] { run(); });
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
    // Releasing that lock FIXES the set of writers that can still touch
    // tx_free_sem_ (#962): each of them raised tx_writers_ inside the same
    // critical section that just saw a live node_, and every later arrival
    // takes the lock, sees nullptr, and leaves without going near the
    // semaphore. Wake the ones already parked — node_ is gone, so each returns
    // immediately — and wait for the tally to empty before the handle they are
    // waiting ON is deleted. vTaskDelay, not a yield: on a unicore chip a
    // higher-priority destroying task that only spun would starve exactly the
    // writers it is waiting for.
    while (tx_writers_.load(std::memory_order_acquire) != 0) {
        (void)xSemaphoreGive(tx_free_sem_);
        vTaskDelay(1);
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
    // TWAI is classic CAN only, so an FD frame is dropped best-effort here; the
    // LENGTH bound is not this port's opinion but the seam's shared egress rule
    // (#931), the same tr::net::can_tx_admissible socketcan_link_t applies.
    if (frame.fd || !can_tx_admissible(frame)) return;

    // Announce this writer while the node is known live, then LEAVE the lock
    // before parking (#962). write_m_ serializes the submission — the pool's
    // acquire contract and the node handle — and nothing else; holding it
    // across the wait made the window per-QUEUE instead of per-frame, so K
    // writers on a bus-off controller spent K x tx_timeout_ms_ in series and
    // teardown queued behind all of them. The tally is what lets the
    // destructor delete tx_free_sem_ without pulling it out from under a
    // writer parked on it.
    {
        const std::lock_guard lock(write_m_);
        if (node_ == nullptr) return;
        tx_writers_.fetch_add(1, std::memory_order_relaxed);
    }
    const tx_writer_exit_t leaving(tx_writers_);

    // FULL policy (#383): bounded backpressure, then a COUNTED drop. A
    // continuation burst deeper than the driver queue parks here until the
    // tx-done ISR frees a slot — it no longer silently loses frames; only a
    // wait that outlives tx_timeout_ms_ drops, and that drop is observable
    // via tx_dropped().
    if (xSemaphoreTake(tx_free_sem_, tx_wait_ticks(tx_timeout_ms_)) != pdTRUE) {
        tx_dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::lock_guard lock(write_m_);
    // Re-checked, not re-read for tidiness: teardown may have run while this
    // writer was parked, and the handle it nulled is about to be deleted. Hand
    // the token back — the destructor's drain is still counting this writer.
    if (node_ == nullptr) {
        (void)xSemaphoreGive(tx_free_sem_);
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
    std::uint8_t buf[tr::view::kCanClassicMaxData] = {};
    twai_frame_t rx = {};
    rx.buffer = buf;
    rx.buffer_len = sizeof(buf);
    if (twai_node_receive_from_isr(node, &rx) != ESP_OK) return false;
    // The seam's shared ingress rule (#931): 29-bit data frames only. The node
    // driver surfaces no error-frame flag on this path, so that leg is false by
    // construction — a bus-off/error condition arrives as a state callback, not
    // as a frame.
    if (!can_rx_admissible(rx.header.ide != 0, rx.header.rtr != 0, /*error=*/false)) return false;

    can_frame_data_t out;
    out.id = rx.header.id & 0x1FFFFFFFu;
    out.fd = false;
    out.len = static_cast<std::uint8_t>(twaifd_dlc2len(rx.header.dlc));
    // The same cap the socketcan sibling applies, phrased identically: the width
    // this frame's own mode carries (classic here, since TWAI has no other).
    const std::uint8_t cap = can_max_len(out.fd);
    if (out.len > cap) out.len = cap;
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
